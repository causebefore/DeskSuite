# 番茄钟功能设计（DeskMate）

- 日期：2026-07-27
- 目标项目：`devices/deskmate`（ESP32-S3 桌面终端固件）
- 状态：设计已确认并完成一致性修订，待写实施计划

## 1. 背景与目标

DeskMate 是带本地交互界面的桌面终端：400×300 RLCD、左右两个物理按键（各支持短按和长按）、
60 秒无活动进入 Light-sleep。本设计为其新增番茄钟产品页面，让用户能够“启动后放下”，并可在
计时期间切换到其他页面。

目标：

- 番茄钟不依赖网络、Hub、RTC 或 SNTP 的可用性即可准确计时。
- 当前页面显示阶段、剩余时间、轮次和今日完成数；其他页面通过状态栏角标感知运行状态。
- 阶段到期后唤醒设备并保持完成提示可见，等待用户手动确认下一阶段。
- 计时、页面、低功耗与设置写入遵守现有 Application / Presentation / UI 分层。

非目标：

- 不接入 Hub 后端，不跨设备同步。
- 不持久化运行中阶段、剩余时间或暂停状态；重启后回到 `IDLE`。
- 不增加提示音，不依赖 `audio_service`。
- 本阶段不交付新的自动化测试脚本，但必须保留人工验收条件。

## 2. 已锁定的需求决策

| 决策项 | 选择 | 备注 |
|---|---|---|
| 数据范围 | 纯本地，不碰 Hub | 计时、设置和计数都在设备端 |
| 计时结构 | 经典三阶段 | 默认专注 25 分钟、短休 5 分钟、长休 15 分钟，每 4 轮一长休 |
| 计时基准 | 单调时间 | 倒计时使用 `esp_timer_get_time()`，不依赖可信 UTC |
| 阶段衔接 | 手动确认 | 自然到期停在 `DONE`；用户确认后才启动下一阶段 |
| 跳过阶段 | 直接进入下一阶段 | 跳过专注不计完成数、不推进已完成轮次 |
| 完成提醒 | 唤醒和视觉提示 | 无提示音；完成提示保持到用户确认或取消 |
| 低功耗策略 | 复用内部 Timer 唤醒 | Light-sleep 间隔纳入番茄钟剩余时间 |
| 页面布局 | 单一英雄时间 + 时间轨道 | 不使用卡片网格、圆形进度环或装饰动画 |
| 后台运行 | 运行时可自由切页 | 左右短按保持系统切页，番茄钟由 Application 后台推进 |
| 设置入口 | Settings 新增“番茄钟设置” | 不使用 Emoji 文本；复用现有菜单框架 |
| 设置生效 | 仅 `IDLE` 可编辑 | 避免当前轮次混用新旧时长或长休间隔 |
| 计数归零 | 本地午夜 | 可信时间用于日期归一化，不参与倒计时 |
| 持久化范围 | 设置、今日完成数、未定日计数 | 运行中状态不持久化 |

## 3. 架构与状态所有权

番茄钟的阶段选择、轮次、跳过、确认、今日计数和设置生效规则都是产品策略，由
`app_pomodoro` 独占。该能力不访问硬件、不复用复杂事务，也不需要额外 Service；Application
内部 Task 和两个低频 one-shot `esp_timer` 足以提供串行化、阶段推进和午夜归一化。

### 3.1 新增与修改模块

```text
UI 层（main/ui/，唯一 LVGL Task）
  新增  main/ui/pages/ui_pomodoro_page.{c,h}
  改    main/ui/core/ui_router.c
  改    main/ui/widgets/ui_status_bar.{c,h}
  改    main/ui/resources/status_icon_resolver.{c,h}
  改    main/ui/pages/ui_settings_page.{c,h}

Presentation 层（main/presentation/）
  新增  main/presentation/pomodoro_presenter.{c,h}
  改    main/presentation/presentation_view_model.h
  改    main/presentation/presentation_page.h
  改    main/presentation/presentation_dispatch.{c,h}
  改    main/presentation/status_bar_presenter.{c,h}

Application 层（main/application/）
  新增  main/application/app_pomodoro.{c,h}
  新增  main/application/app_pomodoro_task.c
  改    main/application/app_page.c
  改    main/application/app_power_task.c
  改    main/app_main.c

Data 层（components/data/）
  新增  components/data/pomodoro_store/
        —— NVS 格式、设置与今日计数持久化

构建与资源
  改    main/CMakeLists.txt
  新增  components/data/pomodoro_store/CMakeLists.txt
  改    components/graphics/ui_platform/images/ 或现有图标生成输入
        —— 16×16 番茄钟运行、暂停、完成图标
```

`app_pomodoro` 属于现有 `main` ESP-IDF 组件，应按
`docs/architecture/component_readmes.md` 在 `main/application/README.md` 同步说明其职责、
状态所有权、Task 和低功耗数据流。`pomodoro_store` 是简单 Data 组件，不创建 Task 或 Timer。

### 3.2 状态所有权和执行上下文

- `app_pomodoro_task` 是阶段、运行态、轮次、单调截止、当前阶段配置快照和完成提示 latch 的唯一
  写入者。
- 用户动作通过有界命令队列提交；公共 `request` API 只表示命令是否成功入队，最终状态通过
  Presenter View Model 和类型化 Presentation 事件报告。
- `esp_timer` 使用 one-shot 模式。回调只携带代次通知 `app_pomodoro_task`，不写状态、不访问
  NVS、不调用 Presenter。
- 状态快照通过 `app_pomodoro_get_status_copy()` 整结构复制，不返回内部指针。
- `app_pomodoro_task` 每次状态收敛后在状态锁外按值调用 `pomodoro_presenter_apply()`，随后通过
  `presentation_dispatch` 发布页面、状态栏或全局完成提示事件。Application 到 Presentation 的
  依赖保持单向，Presentation 不包含 Application 头文件。
- UI 只绘制 View Model，并把设置编辑提交为用户意图，不直接调用 Application、Data 或 NVS。

### 3.3 主数据流

```text
物理按键
  → button_service
  → app_key
  → app_page_consume_input
      当前页 == POMODORO 且为长按？
        → app_pomodoro_request_*()
      否则
        → 左右短按执行默认页面导航

UI 设置意图
  → ui_runtime 用户意图回调
  → app_pomodoro_request_update_settings_copy()
  → app_pomodoro_task
  → pomodoro_store

app_pomodoro_task
  → pomodoro_presenter_apply（按值完整呈现输入）
  → presentation_dispatch 类型化事件
  → Presentation 页面/状态栏刷新事件
  → UI Runtime
```

## 4. 状态机

### 4.1 状态空间

| 维度 | 取值 | 说明 |
|---|---|---|
| `phase` | `NONE` / `FOCUS` / `SHORT_BREAK` / `LONG_BREAK` | `IDLE` 时为 `NONE` |
| `run_state` | `IDLE` / `RUNNING` / `PAUSED` / `DONE` | `DONE` 表示自然到期、等待确认 |
| `completed_in_cycle` | `0..long_break_interval` | 当前长休周期内自然完成的专注轮数 |
| `today_focus_count` | `0..255` | 本地日期内自然完成的专注总数，饱和到 255 |
| `date_verified` | `bool` | 今日计数是否已经归入可信的本地日期 |
| `completion_latched` | `bool` | 阶段到期提示尚未被用户确认或取消 |
| `generation` | 非零单调代次 | 拒绝旧 Timer 通知和重复完成 |

### 4.2 自然到期

```text
IDLE
  → 开始
  → FOCUS · RUNNING

FOCUS · RUNNING
  → 自然到期
  → today_focus_count + 1
  → completed_in_cycle + 1
  → FOCUS · DONE
      completed_in_cycle >= long_break_interval
        → next_phase = LONG_BREAK
      否则
        → next_phase = SHORT_BREAK

FOCUS · DONE
  → 确认
  → next_phase · RUNNING

SHORT_BREAK · RUNNING
  → 自然到期
  → SHORT_BREAK · DONE
  → 确认
  → FOCUS · RUNNING
  → completed_in_cycle 保持不变

LONG_BREAK · RUNNING
  → 自然到期
  → LONG_BREAK · DONE
  → 确认
  → completed_in_cycle = 0
  → FOCUS · RUNNING
```

自然到期进入 `DONE` 时置 `completion_latched=true`。确认下一阶段或取消整组时才清除 latch。
Application 在状态锁外先把同一代次的完整呈现输入同步交给 Presenter，再发布完成提示事件。

### 4.3 暂停、重置与跳过

- `RUNNING → PAUSED`：保存 `paused_remaining_us`，停止 one-shot Timer。
- `PAUSED → RUNNING`：使用当前单调时间重新计算 deadline，并重启 one-shot Timer。
- `PAUSED → IDLE`：右长按重置整组，保留今日完成数。
- `DONE → IDLE`：右长按取消后续阶段并清除完成提示，保留今日完成数。
- 跳过 `FOCUS·RUNNING`：不增加 `today_focus_count`，不增加 `completed_in_cycle`，直接启动
  `SHORT_BREAK·RUNNING`。
- 跳过 `SHORT_BREAK·RUNNING`：直接启动 `FOCUS·RUNNING`，保留 `completed_in_cycle`。
- 跳过 `LONG_BREAK·RUNNING`：清零 `completed_in_cycle`，直接启动 `FOCUS·RUNNING`。

跳过不会经过 `DONE`，也不会产生“阶段完成”统计或提示。

## 5. 计时驱动

### 5.1 单调截止时间

运行中状态使用：

```text
phase_started_at_us
phase_deadline_at_us = phase_started_at_us + phase_duration_us
remaining_us = max(0, phase_deadline_at_us - esp_timer_get_time())
```

`esp_timer_get_time()` 是设备启动后的单调时间。RTC 未初始化、系统 UTC 不可信、断网或 SNTP
校正都不得暂停、提前或延后当前阶段。Light-sleep 返回后单调时间继续反映已经经过的睡眠时长，
Application 用同一公式补算。

系统 UTC 只用于：

- 本地日期和今日完成数归一化。
- 系统时间可信时计算“预计 HH:MM 结束”的辅助展示。

UTC 不可信时页面显示“约 N 分钟后结束”，倒计时照常运行。

### 5.2 one-shot Timer

Application 为阶段推进创建一个 one-shot `esp_timer`：

- `RUNNING` 时调度到下一秒边界或阶段 deadline，两者取更近值。
- 到达普通秒边界时只更新有界快照；番茄钟页可见时发布页面节拍事件。
- 状态栏只在向上取整后的剩余分钟变化时刷新。
- 到达 deadline 时由 `app_pomodoro_task` 幂等转入 `DONE`。
- `IDLE`、`PAUSED`、`DONE` 不运行 Timer。
- 每次重新调度都携带当前 `generation`；旧通知不得改变新阶段。

该设计不会在 Light-sleep 后补发多个周期 TICK。CPU 睡眠期间普通 Task 和软件 Timer 回调暂停；
内部硬件 Timer 唤醒后由 Power Application 主动执行一次同步补算。

今日日期归一化使用另一个低频 one-shot `esp_timer`：

- 系统时间初始化或收到 `SYSTEM_CLOCK_EVENT_UPDATED` 后，Application Task 先归一化当前日期，
  再按可信 UTC 计算距离下一个本地午夜的相对时长并启动日期 Timer。
- 午夜 Timer 只通知 Application Task；Task 清零旧日计数、合并未定日计数、更新 Presenter，
  然后安排下一个午夜。
- UTC 再次校准时取消旧日期 Timer 并按新时间重算，避免时钟调整后午夜触发漂移。
- UTC 不可信时不运行日期 Timer，不影响阶段 Timer。

### 5.3 公共 API 雏形

```c
esp_err_t app_pomodoro_init(void);
esp_err_t app_pomodoro_start(void);
esp_err_t app_pomodoro_stop(uint32_t timeout_ms);
esp_err_t app_pomodoro_deinit(void);

esp_err_t app_pomodoro_request_start(void);
esp_err_t app_pomodoro_request_toggle_pause(void);
esp_err_t app_pomodoro_request_skip(void);
esp_err_t app_pomodoro_request_confirm(void);
esp_err_t app_pomodoro_request_reset(void);
esp_err_t app_pomodoro_request_update_settings_copy(
    const app_pomodoro_settings_t *settings);

esp_err_t app_pomodoro_get_status_copy(
    app_pomodoro_status_t *out_status);
esp_err_t app_pomodoro_get_next_wakeup_interval_ms(
    uint32_t *out_interval_ms);

esp_err_t app_pomodoro_reconcile_after_wakeup(
    uint32_t timeout_ms,
    app_pomodoro_wakeup_result_t *out_result);
```

要求：

- 所有公共 API 添加中文 Doxygen，并声明同步/异步、调用上下文、输出有效性和错误码。
- `request` API 返回 `ESP_OK` 只表示命令已提交；最终状态通过 Presenter 和类型化
  Presentation 事件报告。
- `get_next_wakeup_interval_ms()` 在非 `RUNNING` 时返回 `ESP_ERR_NOT_FOUND`。
- `reconcile_after_wakeup()` 是 Power Application 使用的有界同步命令；返回前必须完成状态补算，
  并通过 `out_result` 报告阶段是否刚刚完成。

## 6. UI 设计

### 6.1 视觉方向

用户是在键盘旁偶尔瞥一眼设备的工作者。页面必须像安静、精确的桌面计时器，而不是带积分和
装饰效果的游戏界面。

- 领域元素：时间块、刻度、阶段、轮次、截止、完成铃。
- 灰阶语义：黑色表示当前专注，深灰表示已完成，浅灰表示休息或尚未开始。
- 标志元素：横向“时间轨道”，同时表达当前阶段进度与长休周期轮次。
- 不使用圆形进度环、卡片网格、阴影、渐变、Emoji 或装饰动画。

### 6.2 可用字体和图标

页面只使用现有字体：

- 剩余时间：`ui_common_new_num48()`。
- 阶段、今日计数、预计结束、提示：16px regular/semibold。
- 完成标题可使用 24px semibold。

不得使用当前不存在的 72px、12px 或 11px 字体。运行、暂停和完成状态使用本地 16×16
单色 SVG 转换资源，并通过 `status_icon_resolver` 提供；不得依赖 `🍅`、`☕`、`⏸`、`✓`
等字体字形。

### 6.3 番茄钟页面

内容区为状态栏下方的 400×268：

顶层页面顺序调整为：

```text
首页 → 番茄钟 → 天气 → 语音 → 日历 → 邮箱 → 限额 → 设置 → 测试
```

| 区域 | 建议位置/尺寸 |
|---|---|
| 阶段反白标签 | x=12，y=16，约 88×28 |
| 今日计数 | x=260，y=20，w=128，16px 右对齐 |
| 大数字 `mm:ss` | x=0，y=56，w=400，h=56，48px 居中 |
| 预计结束/相对结束 | x=0，y=118，w=400，16px 居中 |
| 四段时间轨道 | x=12，y=158，w=376，h=18 |
| 轮次与阶段说明 | x=12，y=184，w=376，16px |
| 按键提示分隔线 | x=12，y=224，w=376，h=1 |
| 按键提示 | x=12，y=234，w=376，h=22，16px 居中 |

状态变体：

- `IDLE`：显示默认专注时长、`准备专注`、今日计数和“长按左键开始”。
- `RUNNING`：阶段标签反白；数字每秒更新；时间轨道显示当前阶段进度。
- `PAUSED`：保留数字，中央增加黑底反白“已暂停”，提示“左长继续 / 右长重置”。
- `DONE`：显示全宽黑底反白完成 banner、完成时长、下一阶段和确认/取消提示。

时间轨道使用四个等宽段：

- 已完成专注轮次：实心黑色。
- 当前专注轮次：黑色边框，内部按阶段进度填充。
- 未开始轮次：浅灰或空心边框。
- 休息阶段：当前段使用浅灰填充，不改变已完成轮次。

`today_focus_count` 使用文字，不绘制数量不受限的圆点：日期可信时显示“今日 N 轮”，日期尚未
可信时显示“已完成 N 轮”，其中 N 包含当前内存中的未定日完成数。

### 6.4 页面和状态栏刷新

- 番茄钟页在前台且 UI 清醒时，`mm:ss` 每秒更新。
- 页面不在前台时，不触发其他业务页面重绘。
- 状态栏只显示 16×16 本地图标和向上取整的剩余分钟，例如图标 + `18m`，不显示 `mm:ss`。
- 状态栏现有页面标题与时间之间只有约 72px 空隙；实现前必须使用
  `ui_platform_font_measure_text()` 验证图标和文本总宽度，超过时退化为仅显示图标。
- `PAUSED` 显示暂停图标，`DONE` 显示完成图标，`IDLE` 不显示。
- 不新增状态栏 LVGL Timer；刷新由 Presentation 状态变化事件驱动。
- 阶段自然完成且当前页不是番茄钟时，UI 在状态栏下方显示 10 秒全宽黑底反白提示：
  “专注完成，进入番茄钟确认”或“休息结束，进入番茄钟确认”。提示消失后完成图标继续常显。
- 全局完成提示不能只依赖瞬时 Presentation 事件。Pomodoro View Model 必须携带
  `completion_latched` 和 `completion_generation`；UI Runtime 启动/恢复重同步时也读取该
  View Model。UI 只对尚未展示的 generation 创建提示，从而保证睡眠中到期不会丢提示，也不会
  在同一 DONE 状态的每次维护唤醒中重复显示。

### 6.5 按键映射

左右短按始终归还系统页面导航。

| 键 | `IDLE` | `RUNNING` | `PAUSED` | `DONE` |
|---|---|---|---|---|
| 左短 | 上一页 | 上一页 | 上一页 | 上一页 |
| 右短 | 下一页 | 下一页 | 下一页 | 下一页 |
| 左长 | 开始专注 | 暂停 | 继续 | 确认并开始下一阶段 |
| 右长 | 无操作 | 跳过当前阶段 | 重置为 `IDLE` | 取消整组并回 `IDLE` |

页面提示必须与本表完全一致。

### 6.6 Settings 子页

在现有设置根菜单末尾增加“番茄钟设置”。`settings_location_t` 和 `settings_item_t` 仍是
`ui_settings_page.c` 的 UI 私有枚举，不进入 Presentation。

列表态：

- 左短/右短：选择上一项/下一项。
- 右长：进入当前项编辑。
- 左长：返回设置根菜单。

编辑态：

- 当前行使用黑底反白，不使用彩色或红框。
- 左短/右短：按步长减小/增大。
- 右长：提交 `UI_USER_INTENT_POMODORO_SETTINGS_SAVE`。
- 左长：取消本次编辑。

配置项：

| 配置项 | 范围 | 步长 | 默认 |
|---|---|---|---|
| 专注时长（分钟） | 5–90 | 5 | 25 |
| 短休时长（分钟） | 1–30 | 1 | 5 |
| 长休时长（分钟） | 5–60 | 5 | 15 |
| 长休间隔（轮） | 2–8 | 1 | 4 |

UI 只提交完整设置副本。Application 校验范围并调用 `pomodoro_store`；成功后 Presenter 更新列表，
失败时保留原值并显示中文错误。只有 `IDLE` 允许进入编辑态；`RUNNING`、`PAUSED` 或 `DONE`
只读显示设置，并提示“结束当前番茄钟后可修改”。这样当前轮次不会混用不同的 duration 或
long-break interval。

## 7. Light-sleep 集成

### 7.1 下一次内部 Timer 间隔

`app_power_task.c::next_light_sleep_interval_ms()` 继续作为业务截止的唯一汇聚点：

```text
interval = refresh_interval_ms
interval = min(interval, dashboard_remaining_ms)   // UTC 计划
interval = min(interval, pomodoro_remaining_ms)    // 单调持续时间
```

- `IDLE`、`PAUSED`、`DONE`：番茄钟 getter 返回 `ESP_ERR_NOT_FOUND`。
- `RUNNING`：返回距离单调 deadline 的剩余毫秒，至少为 1ms。
- 番茄钟计算不依赖 `system_clock.valid`。
- 日志原因应区分“屏幕维护周期”“Dashboard 同步截止”和“番茄钟阶段截止”。

### 7.2 Timer 唤醒顺序

番茄钟补算必须发生在恢复 UI 之前：

```text
内部 Timer 唤醒
  → app_pomodoro_reconcile_after_wakeup()
      remaining > 0
        → 保持 RUNNING，重新调度 one-shot Timer
      remaining <= 0
        → 同步转 DONE、在锁外更新 Presenter、返回 PHASE_COMPLETED
  → 若 PHASE_COMPLETED
      → app_power 在自身上下文更新活动代次
      → 按正常清醒恢复顺序恢复网络、语音和 UI
      → UI 从已经更新的 Presenter 快照绘制完成提示
      → 退出睡眠会话并重新开始 60 秒清醒窗口
  → 否则
      → 按现有维护流程决定是否联网
      → 恢复 UI 完成维护刷新
      → 条件允许时再次停止 UI 并继续 Light-sleep
```

`app_pomodoro_reconcile_after_wakeup()` 返回前必须完成状态和 Presenter 输入快照的收敛，不能仅
异步投递一个稍后处理的完成事件。这样 `resume_ui_runtime()` 首次重同步即可读到 `DONE`。

### 7.3 完成提示 latch

- 自然到期设置 `completion_latched=true`。
- Power Application 将本次自然到期视为用户活动，阻止当前睡眠会话立即再次入睡。
- 完成提示至少保持现有 60 秒清醒窗口；窗口到期后即使重新睡眠，状态仍保持 `DONE`。
- 后续维护唤醒仍显示 `DONE` 状态；只有用户左长确认或右长取消才清除 latch。
- 当前页面不是番茄钟时，状态栏完成图标必须可见；不自动切页，避免打断用户上下文。

## 8. 持久化

NVS 命名空间为 `"pomodoro"`：

| 字段 | key | 类型 | 说明 |
|---|---|---|---|
| 格式版本 | `schema_ver` | u8 | 当前为 1 |
| 专注时长 | `focus_min` | u8 | 5–90 |
| 短休时长 | `short_min` | u8 | 1–30 |
| 长休时长 | `long_min` | u8 | 5–60 |
| 长休间隔 | `interval` | u8 | 2–8 |
| 今日完成数 | `today_count` | u8 | 饱和到 255 |
| 今日日期戳 | `today_date` | u32 | 本地 `YYYYMMDD` |
| 未定日完成数 | `pending_count` | u8 | UTC 不可信期间完成的专注数 |

规则：

1. 初始化时读取并校验设置；缺失值使用默认值，越界或未知版本报告事实，由 `app_pomodoro` 决定
   恢复默认并写回。
2. 系统时间可信时，在启动和 `SYSTEM_CLOCK_EVENT_UPDATED` 时执行日期归一化，并通过独立的
   午夜 one-shot Timer 保证空闲状态下也能准时清零；状态 Getter 保持只读。
3. 日期变化时先把 `today_count` 清零并写入新日期；随后无论日期是否变化，都把
   `pending_count` 饱和合并到当前日期并清零。
4. 系统时间不可信时完成专注，只增加持久化的 `pending_count`；时间恢复可信后把它计入当前
   本地日期。
5. 每次专注自然完成后提交计数；跳过不写完成数。
6. 运行中 `phase`、`run_state`、deadline、暂停剩余和轮次不写 NVS；设备重启后回 `IDLE`。

## 9. 错误与降级

- 命令队列满：`request` API 返回明确错误，不改变状态。
- Timer 创建或调度失败：保留当前可证明状态并发布错误事实；Application 不伪装为已运行。
- Store 写入失败：当前内存设置或计数仍可使用，但 View Model 标记“未保存”，后续显式动作重试。
- 系统时间不可信：倒计时不降级；预计结束改为相对时间，计数进入 `pending_count`。
- Power 同步补算超时：不能证明是否已经完成阶段，进入现有 Power 恢复错误路径，禁止静默继续
  睡眠。
- 状态栏图标资源不可用：只隐藏图标，不影响页面和计时。

## 10. 改动汇总

| 类别 | 改动点 |
|---|---|
| Application | 新增 `app_pomodoro.{c,h}`、`app_pomodoro_task.c` |
| Presentation | 新增 `pomodoro_presenter.{c,h}`，扩展页面、View Model、状态栏和事件契约 |
| UI | 新增 `ui_pomodoro_page.{c,h}`，扩展 Router、状态栏、设置页和用户意图 |
| Data | 新增 `components/data/pomodoro_store/` 及 `CMakeLists.txt` |
| 资源 | 新增 16×16 单色运行、暂停、完成图标及 resolver 映射 |
| 低功耗 | 扩展 `app_power_task.c` interval 聚合、唤醒前同步补算和完成保持 |
| Composition Root | 修改 `main/app_main.c` 初始化、启动、停止和反向清理顺序 |
| 构建 | 修改 `main/CMakeLists.txt`，声明新增源码和 `pomodoro_store` 依赖 |
| 文档 | 更新根设备 README、`main/application/README.md`、`main/presentation/README.md`、`main/ui/README.md` 和低功耗流程 |
| 硬件层 | `device_button`、`device_power`、BSP、Driver 和 Board 零改动 |

## 11. 验收条件

本阶段不新增自动化测试脚本，但实施完成后必须按风险执行以下人工检查，并记录结果：

1. 默认设置下依次完成 4 个专注和 3 个短休，确认第 4 个专注后进入长休；短休不会清零轮次。
2. 分别在专注、短休和长休阶段执行跳过，确认计数与下一阶段符合状态机。
3. 运行中切换到天气、日历等页面，确认倒计时继续且只更新状态栏角标，不重绘当前业务页面。
4. 未联网且系统时间不可信时启动 5 分钟专注，确认仍准时完成，预计结束使用相对时间。
5. 运行期间接受 RTC/SNTP 校时，确认剩余时长没有跳变。
6. 让设备进入 Light-sleep，阶段截止时确认先补算番茄钟、再恢复 UI，首次画面即为 `DONE`。
7. 完成提示保持可见；60 秒后重新睡眠仍保持 `DONE`，按键唤醒后仍可确认下一阶段。
8. 连续执行多轮 Light-sleep，确认没有每秒 TICK 补发、事件队列突发或重复完成计数。
9. 运行中、暂停或完成待确认时进入设置，确认番茄钟设置只读；回到 `IDLE` 后可编辑，写入失败时
   显示“未保存”并保留原值。
10. 跨本地午夜和时间不可信两种场景检查今日计数，确认不会继续显示昨天数据或丢失未定日计数。
11. 检查状态栏图标与文本不覆盖页面名、系统时间、Wi-Fi、服务端和电池区域。
12. 重启设备，确认运行中状态被丢弃并回到 `IDLE`，设置和今日计数正确恢复。

若用户明确要求编译，只能在 DeskSuite 根目录执行：

```powershell
& .\ds.ps1 build deskmate
```

不得绕过统一脚本调用 `idf.py`、`cmake` 或 `ninja`。

## 12. 实施注意事项

- 所有公共 C/C++ API 使用中文 Doxygen，至少包含 `@brief`；参数和返回值补全方向及错误码。
- 所有项目自有日志与错误信息使用中文。
- 新增 Application 职责后同步更新 `main/application/README.md`；实质修改 Presentation、UI 和
  Power 数据流时同步更新对应 README 与低功耗流程。
- commit message 使用 `type(scope): 中文描述`，按职责拆分，只暂存当前任务文件或代码块。
- 默认不主动编译；仅在用户明确要求时使用仓库统一脚本。

## 13. 参考文件

- `devices/deskmate/README.md`
- `devices/deskmate/docs/architecture/layering.md`
- `devices/deskmate/docs/architecture/data_flow.md`
- `devices/deskmate/docs/architecture/api_conventions.md`
- `devices/deskmate/docs/architecture/component_readmes.md`
- `devices/deskmate/docs/低功耗流程.md`
- `devices/deskmate/main/application/app_power_task.c`
- `devices/deskmate/main/application/app_settings.c`
- `devices/deskmate/main/ui/pages/ui_settings_page.c`
- `devices/deskmate/main/ui/widgets/ui_status_bar.c`
- `devices/deskmate/components/graphics/ui_platform/fonts/font_runtime.c`
