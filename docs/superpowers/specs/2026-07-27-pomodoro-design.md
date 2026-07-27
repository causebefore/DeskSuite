# 番茄钟功能设计（DeskMate）

- 日期：2026-07-27
- 目标项目：`devices/deskmate`（ESP32-S3 桌面终端固件）
- 状态：已与用户确认设计，待写实施计划

## 1. 背景与目标

DeskMate 是带本地交互界面的桌面终端：400×300 单色墨水屏、左右两个物理按键（各支持短按/长按）、60 秒无活动进入 Light-sleep。本设计为其新增番茄钟功能，作为新的产品页面，让设备具备"启动后放下"的专注陪伴能力。

**目标**：用户启动一段专注后可切到其他页面，番茄钟在后台计时，状态栏角标实时显示剩余，到期用视觉提示唤醒用户。

**非目标**：不接入 Hub 后端、不跨设备同步、不持久化运行中状态、不交付测试脚本。

## 2. 已锁定的需求决策

| 决策项 | 选择 | 备注 |
|---|---|---|
| 数据范围 | 纯本地，不碰 Hub | 计时/设置/计数全在设备 NVS |
| 计时结构 | 经典三阶段（专注/短休/长休） | 默认 25/5/15 分钟，每 4 轮一长休 |
| 阶段衔接 | 手动确认 | 阶段到期不自动进下一阶段，停在 DONE 待确认 |
| 完成提醒 | 唤醒 + 亮屏为主（视觉），无提示音 | 不依赖 audio_service beep 能力 |
| 低功耗策略 | 复用内部 Timer 唤醒 | sleep interval 纳入番茄钟 deadline |
| 页面布局 | 横排 + 进度条（布局 B） | 按分钟节拍刷新，零秒级动画 |
| 后台运行 | 运行时可自由切页 | 左右短按归还系统切页，番茄钟在 service 后台跑 |
| 设置入口 | Settings 页新增"🍅 番茄钟设置"子项 | 复用 Settings 框架，番茄钟页面纯计时 |
| 设置交互 | 列表模式 + 行内编辑 | 进入即列表，反色高亮，右长按进编辑 |
| 计数归零 | 本地午夜（YYYYMMDD） | system_clock 可信时判定 |
| 持久化范围 | 只存设置 + 今日完成数 | 运行中状态不持久化，重启回 IDLE |
| 测试 | 不交付测试脚本 | 见"风险与偏离"章节 |

## 3. 架构与组件落点

严格遵循 DeskMate 既有三层范式（Application / Presentation / UI），新增 services 层组件拥有计时驱动，参考 `button_service` 模式。

### 3.1 分层与新增模块

```
UI 层（main/ui/，LVGL · 唯一 UI Task）
  新增  main/ui/pages/ui_pomodoro_page.{c,h}          —— 番茄钟页面绘制（布局 B）
  改    main/ui/core/ui_router.c                        —— init/show/update/reset 四处 switch + import
  改    main/ui/widgets/ui_status_bar.{c,h}             —— 加番茄钟角标控件
  改    main/ui/pages/ui_settings_page.{c,h}            —— 加番茄钟设置子页

Presentation 层（main/presentation/）
  新增  main/presentation/pomodoro_presenter.{c,h}      —— 订阅事件，填 view_model
  改    main/presentation/presentation_view_model.h     —— 加 pomodoro_view_model_t
  改    main/presentation/presentation_page.h           —— 加 PRESENTATION_PAGE_POMODORO
  改    main/presentation/presentation_dispatch.{c,h}   —— 加 POMODORO action 事件
  改    main/presentation/status_bar_presenter.{c,h}    —— 加番茄钟角标字段 + setter

Application 层（main/application/，变薄）
  新增  main/application/app_pomodoro.{c,h}             —— 按键意图→service 调用 + PHASE_DONE 处理
  改    main/application/app_page.c                      —— 加 PRESENTATION_PAGE_POMODORO consume case
  改    main/application/app_power_task.c                —— sleep interval 纳入 deadline + 唤醒分发
  改    main/application/app_main.c                      —— 注册 pomodoro_presenter

Services 层（components/services/）
  新增  components/services/pomodoro_service/            —— 状态机 + esp_timer + deadline 查询
        必须配 components/services/pomodoro_service/README.md

Data 层（components/data/）
  新增  components/data/pomodoro_store/                  —— NVS 持久化（设置 + 今日完成数）
        必须配 components/data/pomodoro_store/README.md

复用
  components/sys/system_clock                            —— 可信 UTC 查询、午夜归零
  components/device/device_button                        —— 现有 4 槽位按键（零改动）
```

### 3.2 数据流

```
device_button（左右键，短/长）
  → app_key 投递 APP_KEY_EVENT
  → app_page_consume_input
      当前页 == POMODORO？
        是 → app_pomodoro_consume_input（按键意图翻译）
              → pomodoro_service_start/pause/skip/confirm/reset
        否 → 系统切页（默认环形导航）

pomodoro_service（状态机 + esp_timer）
  → esp_event APP_POMODORO_EVENT
      TICK（每秒） / PHASE_DONE（阶段到期）
  → pomodoro_presenter 订阅 → 填 pomodoro_view_model_t
  → status_bar_presenter_set_pomodoro()（状态栏角标）
  → ui_pomodoro_page / ui_status_bar 刷新
```

## 4. 状态机与计时驱动

### 4.1 状态空间

| 维度 | 取值 | 说明 |
|---|---|---|
| 阶段 phase | `IDLE` / `FOCUS` / `SHORT_BREAK` / `LONG_BREAK` | IDLE 是初始/重置态 |
| 运行态 run_state | `RUNNING` / `PAUSED` / `DONE` | 每个非 IDLE 阶段的子态；DONE = 阶段完成待确认 |
| 轮次 round_index | 0 .. (long_break_interval - 1) | 默认 interval=4；第 4 轮 FOCUS 完成进 LONG_BREAK |
| 今日完成数 today_focus_count | int，午夜归零 | 每个 FOCUS 完成时 +1（不计跳过） |

### 4.2 状态机（手动衔接）

- `IDLE` →（左长按 start）→ `FOCUS·RUNNING`
- `FOCUS·RUNNING`：每秒 tick；到期或右长按 skip → `DONE`；左长按 → `PAUSED` ↔ `RUNNING`
- `FOCUS·DONE`：左长按 confirm → 下一阶段
  - round_index++ 后若等于 interval → `LONG_BREAK·RUNNING`
  - 否则 → `SHORT_BREAK·RUNNING`
- `SHORT_BREAK / LONG_BREAK`：到期 → DONE；左长按 confirm → `FOCUS·RUNNING`，round_index 归零
- 右长按（按状态分流，见 5.2 按键表）：RUNNING/DONE → skip 跳过当前阶段；PAUSED → reset 回 `IDLE`；IDLE → 无操作。reset 时 today_focus_count 保留。

**关键：DONE 是"阶段完成待确认"的中间态。** 因为手动衔接，到期后不自动进下一阶段，停在 DONE 等用户左长按确认。RUNNING 态右长按 skip 直接跳过当前阶段进下一阶段（不 +1 计数）。

### 4.3 计时驱动（esp_timer + system_clock 绝对时间）

所有计时基于 `phase_end_utc` 绝对时间戳，不依赖"累计已运行秒数"。

| 阶段动作 | 实现 |
|---|---|
| 启动阶段 | `phase_start_utc = system_clock_get_utc()`；`phase_end_utc = phase_start_utc + phase_duration_sec`；启动 `esp_timer_start_periodic(1000ms)` |
| 每秒 tick | `remaining = phase_end_utc - system_clock_get_utc()`；`remaining <= 0` → 转 DONE，发 PHASE_DONE，停 timer |
| 暂停 | 记录 `paused_remaining = phase_end_utc - now`；停 timer |
| 恢复 | `phase_end_utc = now + paused_remaining`；重启 timer |

### 4.4 对外 API（pomodoro_service.h 雏形）

```c
esp_err_t pomodoro_service_init(void);                   // 启动时调用，读 NVS 恢复设置
void     pomodoro_service_start(void);                   // IDLE → FOCUS·RUNNING
void     pomodoro_service_pause(void);                   // RUNNING ↔ PAUSED
void     pomodoro_service_skip(void);                    // 跳过当前阶段，不 +1
void     pomodoro_service_confirm(void);                 // DONE → 下一阶段
void     pomodoro_service_reset(void);                   // 任意 → IDLE，计数保留

pomodoro_status_t pomodoro_service_get_status(void);    // 全量快照
esp_err_t pomodoro_service_get_next_deadline_at_utc(int64_t *out_utc);  // IDLE/PAUSED 返回 NOT_FOUND
void     pomodoro_service_on_wakeup(void);               // Light-sleep 唤醒后对齐

// 事件（默认事件循环，APP_POMODORO_EVENT base）
//   POMODORO_SERVICE_EVENT_PHASE_DONE  —— 阶段到期待确认，app_pomodoro 触发视觉提示
//   POMODORO_SERVICE_EVENT_TICK        —— 每秒，presenter 按分钟节拍决定是否刷屏
```

**TICK 与 UI 刷新解耦**：service 每秒发 TICK，但 presenter 按"分钟边界"决定是否真正刷屏（mm:ss 跨分钟才刷；进度条只在分钟边界刷；状态变更立即刷）。墨水屏零动画零闪烁。

## 5. UI 设计

### 5.1 番茄钟页面（布局 B，400×300，状态栏 32px，内容区 268px）

| 区域 | 位置/尺寸 |
|---|---|
| 状态栏 | 0,0 → 400×32（系统共用） |
| 阶段标签（pill） | y≈44，左对齐 x=12，36px 高 |
| 今日计数 | y≈44，右对齐 x=388，12px 字，实心圆●已完成 + 空心圆○未完成 |
| 大数字 mm:ss | y≈90 起，72px 字号，左对齐 |
| 进度条 | y≈196，宽 376px（x=12..388），高 12px |
| 进度标注 | y≈214，左右两端，11px（已专注/共时长） |
| 按键提示 | y≈276，底边虚线分隔，12px 居中 |

**状态变体**（同一布局，内容/对比变化）：
- `RUNNING`：阶段 pill 实心、数字正常
- `PAUSED`：阶段 pill 虚线、数字半透明、进度标注显示"已暂停"
- `DONE`：整页反白 banner（"专注完成 ✓"填充黑色横条）+ "下一个：短休/长休" + 按键提示加粗

DONE 态反白 banner 是单色屏上最显眼的提醒（全屏闪动会残影，反白 banner 是标准做法）。

### 5.2 按键映射（计时模式，零 device_button 改动）

左右短按全部归还系统切页（番茄钟页面也不例外，后台运行范式）。

| 键 | IDLE | RUNNING | PAUSED | DONE |
|---|---|---|---|---|
| 左 短 | 系统切页 prev | 系统切页 prev | 系统切页 prev | 系统切页 prev |
| 右 短 | 系统切页 next | 系统切页 next | 系统切页 next | 系统切页 next |
| 左 长 | 开始 → RUNNING | 暂停 → PAUSED | 继续 → RUNNING | 确认 → 下一阶段 |
| 右 长 | 无操作 | 跳过 → 下一阶段 | 重置 → IDLE | 跳过 → 下一阶段 |

### 5.3 状态栏角标（其他页面可见进度）

放在状态栏中段空隙 x≈96–160（紧邻时间），番茄钟非 IDLE 态时显示。

| 状态 | 角标 |
|---|---|
| FOCUS·RUNNING | 🍅 + mm:ss（如 🍅18:23） |
| SHORT_BREAK·LONG_BREAK·RUNNING | ☕ + mm:ss |
| PAUSED | 🍅⏸（不显秒，省刷新） |
| DONE | 🍅✓（常显，墨水屏不闪） |
| IDLE | 不显示 |

**刷新**：service 每分钟边界 + 状态变更时调 `status_bar_presenter_set_pomodoro()`，复用现有 STATUS_BAR_UPDATE 链路。widget 层脏检查只重绘角标，不扰动其他控件。**不新增 UI 定时器。**

状态栏改动清单：
- `status_bar_presenter.h` 的 `status_bar_view_model_t` 加字段（`pomodoro_running`、`pomodoro_phase`、`pomodoro_remain_sec`）
- `status_bar_presenter.c` 加 `status_bar_presenter_set_pomodoro()` setter（仿 `set_server_online`）
- `ui_status_bar.c` 加 `s_pomodoro_label` 控件（`create_controls` 创建、`ui_status_bar_update` 脏检查绘制、`ui_status_bar_deinit` 清理）

### 5.4 Settings 页"🍅 番茄钟设置"子项

在现有 4 个子项（网络设置/网页文件管理/系统信息/检查更新）后新增第 5 项。复用 `settings_location_t` + `settings_item_t` + `lv_menu + lv_group` 反色高亮框架。

- `presentation` 层：`SETTINGS_LOCATION_POMODORO` 枚举值
- `ui_settings_page.c`：根菜单加 `create_root_item(SETTINGS_ITEM_POMODORO, ..., "🍅 番茄钟设置")`；新增 `render_pomodoro()` 渲染子页

**子页两态**：

1. **列表态（LIST）**：进入即列表，4 个配置项 + 底部"今日完成"只读行。反色高亮当前项。
   - 左短/右短：PREV/NEXT（上下选）
   - 右长：ACTIVATE（进入该项编辑）
   - 左长：BACK（退出设置，回 Settings 根菜单）

2. **编辑态（EDIT_VALUE）**：当前行变红框 + 显示 −/+ 步进器 + 范围提示。
   - 左短：−（值减，按步长）
   - 右短：+（值加）
   - 右长：确认 → 存 NVS → 回列表态
   - 左长：取消 → 回列表态（不存）

配置项（4 项，统一结构）：

| 配置项 | 范围 | 步长 | 默认 |
|---|---|---|---|
| 专注时长（分钟） | 5 – 90 | 5 | 25 |
| 短休时长（分钟） | 1 – 30 | 1 | 5 |
| 长休时长（分钟） | 5 – 60 | 5 | 15 |
| 长休间隔（轮） | 2 – 8 | 1 | 4 |

## 6. 低功耗集成

### 6.1 interval 注入点

`app_power_task.c` 的 `next_light_sleep_interval_ms()`（约 :446-467）是业务层 interval 计算的唯一汇聚点。番茄钟照搬 `app_network_get_next_dashboard_sync_at_utc` 的 getter 模式。

```
现状：interval = min(refresh_60s, dashboard_remaining)
新增：interval = min(refresh_60s, dashboard_remaining, pomodoro_remaining)
```

- IDLE/PAUSED 态：`app_pomodoro_get_next_deadline_at_utc()` 返回 `ESP_ERR_NOT_FOUND`，app_power 跳过这一项，**不破坏现有 sleep 行为**
- RUNNING 态：返回 `phase_end_utc`
- `pomodoro_remaining <= 0`：返回 1ms（立即唤醒）

### 6.2 唤醒后分发

`app_power_task.c` Timer 唤醒分支（约 :678 起），在 `resume_ui_runtime()` 之后新增调用 `app_pomodoro_on_wakeup()`：

- `remaining = phase_end_utc - system_clock.now`
- `remaining > 0`：重启 esp_timer 继续 RUNNING（下一轮 sleep interval 仍取 min，多轮拼接覆盖整个阶段时长）
- `remaining <= 0`：转 DONE，发 PHASE_DONE

### 6.3 完成提示（纯视觉）

`app_pomodoro` 收到 `POMODORO_SERVICE_EVENT_PHASE_DONE`：

1. 调 `app_power_notify_activity()` —— 打断当前 sleep 会话，重置 60s idle 窗口，确保设备完全清醒
2. UI 更新：
   - 番茄钟页面（若在前台）：DONE 反白 banner
   - 状态栏角标：🍅✓（任何页面都可见）

**不依赖提示音**。唤醒+亮屏+反白 banner 是主要提示通道。

### 6.4 system_clock 不可信时的降级

| 触发条件 | 行为 |
|---|---|
| 断网刚醒，`system_clock.valid == false` | `app_pomodoro_on_wakeup()` 不推进状态机；`phase_end_utc` 保留；UI 显示"等待校时" |
| `next_light_sleep_interval_ms()` 检测 clock 不可信 | 现有逻辑已处理——返回 `refresh_interval_ms`，不取番茄钟 deadline |
| 校时后恢复 | 下一轮 Timer 唤醒时用新 UTC 补算：已过期则转 DONE + 视觉提示（补显）；未到期则继续 RUNNING |

**边界**：校时延迟可能导致视觉提示"迟到"。这是纯本地方案的固有代价。

## 7. 持久化（NVS，命名空间 `"pomodoro"`）

| 字段 | key | 类型 | 何时写 |
|---|---|---|---|
| 专注时长 | `focus_min` | u8 | Settings 编辑确认时 |
| 短休时长 | `short_min` | u8 | 同上 |
| 长休时长 | `long_min` | u8 | 同上 |
| 长休间隔 | `interval` | u8 | 同上 |
| 今日完成数 | `today_count` | u8 | 每次专注完成 +1 |
| 今日日期戳 | `today_date` | u32（YYYYMMDD） | 同上（午夜归零判定） |

**运行中状态（phase/round/phase_end_utc）不持久化**：重启后回 IDLE。理由：番茄钟是短时专注工具，掉电罕见；重启后让用户重新开始更干净，避免"恢复了一个过期番茄钟"的混乱和补算复杂度。

**午夜归零**：每次专注完成时读 system_clock → 本地 YYYYMMDD → 与 `today_date` 比较，不同则归零。system_clock 不可信时用 NVS 旧值，下次校时或下次完成时纠正。

## 8. 改动汇总

| 类别 | 改动点 | 规模 |
|---|---|---|
| 新增组件 | `components/services/pomodoro_service/`（状态机 + esp_timer + deadline 查询） | 中 |
| 新增组件 | `components/data/pomodoro_store/`（NVS 持久化，配 README） | 小 |
| 新增分层模块 | `main/application/app_pomodoro.{c,h}`（按键意图 + PHASE_DONE 处理） | 小 |
| 新增分层模块 | `main/presentation/pomodoro_presenter.{c,h}` | 小 |
| 新增分层模块 | `main/ui/pages/ui_pomodoro_page.{c,h}`（纯计时 UI，布局 B） | 中 |
| 扩展现有 | `ui_settings_page.{c,h}` 加番茄钟子页（SETTINGS_LOCATION_POMODORO + render + 编辑态） | 中 |
| 扩展现有 | `status_bar_presenter.{c,h}` + `ui_status_bar.{c,h}` 加角标 | 小 |
| 扩展现有 | `presentation_page.h` + `presentation_view_model.h` + `presentation_dispatch.{c,h}` | 小 |
| 扩展现有 | `ui_router.c`（4 处 switch）+ `app_page.c`（consume case）+ `app_main.c`（注册 presenter） | 小 |
| 低功耗集成 | `app_power_task.c`（interval 注入 :446-467 + 唤醒分发 :678 附近） | 中 |
| `device_button` / `bsp_power` | **零改动** | — |

## 9. 风险与偏离

### 9.1 已知风险

1. **低功耗回归**：番茄钟改 `app_power_task.c` 的 sleep interval 算法和唤醒分发，属于新增 Power 生命周期。`docs/低功耗流程.md` 第 166-173 行要求"完成 100 次 Light-sleep 实机验收前不增加新生命周期"，番茄钟新增会算入这个回归。
2. **视觉提示迟到**：system_clock 不可信期间计时暂停，校时后补算；若断网超过 phase_end_utc 几分钟，校时后才显示完成提示。这是纯本地方案的固有代价。
3. **多轮 sleep 拼接**：25 分钟专注跨多个 60s sleep 周期，依赖每轮唤醒后的 system_clock 绝对时间补算正确性。需实机验证。

### 9.2 偏离项目规范（用户明确决定）

**本设计不交付任何测试脚本**，这与项目现有规范存在偏离，明确记录如下：

- `devices/deskmate/AGENTS.md` 和 `docs/低功耗流程.md` 第 166-173 行要求"新增 Power 生命周期必须配套 `tools/tests/*.ps1` 静态契约脚本"。现有 4 个脚本（`check_power_rebuild_stage1.ps1` / `check_rtc_sleep_decoupling.ps1` / `check_button_event_driven.ps1` / `check_voice_power_lifecycle.ps1`）是这类集成的回归门控先例。
- 番茄钟改 `app_power_task.c` 属于"新增 Power 生命周期"，按规范应至少交付 `check_pomodoro_power_integration.ps1`（验证 getter 契约：IDLE 返回 NOT_FOUND、RUNNING 返回有效 UTC）等契约脚本。
- **用户在 2026-07-27 的设计讨论中明确选择"完全不写测试"**，包括静态契约脚本和实机验收清单。
- 后续若出现 sleep 回归 bug 或番茄钟/低功耗交互问题，本偏离是责任追溯依据。建议在实施过程中如发现高风险点，重新评估是否补回契约脚本。

## 10. 实施注意事项

- 所有公共 C/C++ API 使用中文 Doxygen 注释（至少 `@brief`，有参数/返回补 `@param`/`@return`）
- 所有项目自有日志和错误信息使用中文（`ESP_LOG*`、断言说明）
- `pomodoro_service` 和 `pomodoro_store` 两个新组件必须按 `docs/architecture/component_readmes.md` 在组件根目录建 README.md
- commit message 格式 `feat(pomodoro): 中文描述`，按职责拆分多个 commit，只暂存本任务文件
- 不得绕过 `ds.ps1` 直接调用 `idf.py`/`cmake`/`ninja`
- 默认不主动编译，只有用户明确要求才在 DeskSuite 根目录执行 `& .\ds.ps1 build deskmate`

## 11. 参考文件

- `devices/deskmate/docs/低功耗流程.md` —— Light-sleep 完整流程
- `devices/deskmate/docs/architecture/README.md` —— 分层与组件规范
- `devices/deskmate/docs/architecture/component_readmes.md` —— 组件 README 要求
- `devices/deskmate/main/application/app_power_task.c:446-467` —— interval 算法注入点
- `devices/deskmate/main/application/app_power_task.c:678` —— Timer 唤醒分发注入点
- `devices/deskmate/main/application/app_settings.c:53-105` —— Settings consume_input 范式
- `devices/deskmate/main/ui/pages/ui_settings_page.c:947-1053` —— Settings handle_action 状态机范式
- `devices/deskmate/main/ui/widgets/ui_status_bar.c` —— 状态栏布局（中段空隙 x≈92-164）
- `devices/deskmate/components/services/button_service/` —— service + esp_timer 模式参考
