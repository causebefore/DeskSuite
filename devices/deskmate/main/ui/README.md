# UI

> UI 层拥有 DeskMate 的 LVGL 控件树和唯一 UI Runtime，只负责视觉呈现。

## 1. 定位

- 输入：Presentation 的页面契约、View Model 和 `PRESENTATION_EVENT`。
- 输出：LVGL 页面、状态栏、动画、视觉反馈，以及通过借用回调上报的窄用户意图。
- 执行上下文：唯一的 `ui_runtime_task`。

## 2. 职责边界

负责：

- 创建、更新和删除 LVGL 页面、控件、样式与动画。
- 把 Presentation 事件转换为 UI 私有 `ui_msg_t`。
- 在唯一 LVGL 上下文串行处理页面切换和刷新。
- 使用独立 Screen 管理顶层页面，使用 `lv_menu + lv_group` 管理两层设置菜单。
- 把菜单激活转换为启动配网、检查/安装/丢弃 OTA、启停网页控制台等窄用户意图。
- 管理 UI Runtime 的启动、停止、消息队列和控制握手。

不负责：

- 不直接读取 Service、Communication、Data、System 或 Device。
- 不决定配网、OTA、语音、重试、降级或轻睡眠产品策略。
- 不维护与 Presenter 重复的业务状态。
- 不从非 UI Runtime 上下文修改 LVGL 对象。

## 3. 主要流程

```text
PRESENTATION_EVENT
    → ui_task.c 的默认事件循环回调
    → 按值复制为 ui_msg_t
    → ui_runtime_task
    → ui_router / 页面 / 状态栏
    → Presenter getter 复制 View Model
    → LVGL 更新
```

UI 停止期间收到的刷新事件不会缓存业务数据；Runtime 只合并一枚恢复刷新标记和最后页面切换，
最新状态仍保存在 Presenter。恢复时先在 LVGL timer 停止状态下重新读取状态栏和当前页面
View Model，再恢复显示、同步提交完整刷新并等待显示传输完成。

首页中间信息带直接显示 Calendar Presenter 排序后的下一日程和 Mail Presenter 的完整未读邮件数，
不再绘制“暂无日程/暂无提醒”静态占位。邮箱页只展示 Dashboard 已按未读优先排序的前两封邮件：
未读不足两封时由最近已读邮件补位，没有未读时仍展示最近两封已读邮件。主题最多两行并统一
使用半粗黑字，未读额外使用实心左标；页面顶部始终显示完整未读计数。

日历、邮箱、限额和语音页在 `_show` 中创建最大有界控件树，后续 `_update` 只修改标签文本和
控件显隐。限额进度采用五段固定二值刻度，不在刷新时调整控件几何；有效空列表显示明确空态，
`PRESENTATION_DATA_EMPTY` 仅用于尚未取得数据。全 UI 使用黑、白、实心、轮廓和字重表达层级，
不依赖 I1 面板无法保留的灰色或透明度。

顶层每个页面按需创建独立 `lv_obj_create(NULL)` Screen。状态栏位于 `lv_layer_top()`；切换时
立即加载新 Screen 并自动删除旧 Screen，只让新页面内容按方向从 `+16 px` 或 `-16 px` 在
200 ms 内缓动到原位。这样保持方向反馈，同时避免在面板刷新边界内同时绘制两棵整屏控件树。
页面静态句柄按具体 Screen 实例校验归属，旧实例的延迟删除回调不得清理新实例资源。
设置菜单不注册新输入设备：短按手动调用 `lv_group_focus_prev/next()`，长按向焦点项或
`lv_menu` 标题栏返回按钮发送点击事件。
顶层页面按“首页 → 番茄钟 → 天气 → 语音 → 日历 → 邮箱 → 限额 → 设置”环形排列。
番茄钟页使用 48px 等宽倒计时、黑白四段时间轨道和反白阶段/完成状态；其他页面只在剩余分钟、
运行状态或完成 latch 变化时刷新状态栏角标，不因每秒节拍重绘。

设置根菜单按“网络设置 → 网页控制台 → 系统信息 → 检查更新 → 番茄钟设置”排列且焦点循环
（末项按右键绕回首项，首项按左键绕回末项）。番茄钟设置在 `IDLE` 可逐项编辑并以完整副本
上报用户意图；草稿保存开始编辑时的 `settings_version`，若浏览器已先更新则 Owner 拒绝陈旧
提交，UI 重新读取最新 View Model。同步接受结果返回非零请求 ID；设置页在匹配
`PENDING/SUCCEEDED/FAILED` 终态前保持保存中并禁止离开，Task 执行点或 NVS 保存失败会显示
该请求的真实错误。运行、暂停和完成待确认状态只读。进入
“网页控制台”子页才提交非阻塞启动意图；子页只展示启动阶段、本地 URL、六位访问码、
SD 总/剩余容量和中文错误，不提供配置表单或二维码。离开子页时 UI 先提交停止意图，并持续
等待 Presenter 报告 `STOPPED` 或其他允许退出的安全终态后才返回根菜单。

设备到浏览器的完整流程为：

```text
设备设置页选择“网页控制台”
  → app_web_console 检查 /sdcard
  → app_network 授予 APP_NETWORK_LEASE_WEB_CONSOLE
  → web_console_service 恢复事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → handler 提供 Files、番茄钟 Settings 与系统 Status
  → 设备返回时 Service 安全停止后释放网络租约
```

UI 只负责流程两端的启动/停止用户意图和 View Model 呈现。`app_network` 链路变化借用回调、
Application 状态收敛、Presenter 版本仲裁、认证和浏览器交互都不在 UI 上下文执行。设备端
“网页控制台”子页不复刻浏览器配置表单、文件操作或状态表格；本机番茄钟设置仍只通过独立设置
子页提交版本化完整副本。

阶段自然完成且当前页不是番茄钟时，`ui_pomodoro_banner` 在状态栏下显示十秒全宽反白提示。
提示是否需要创建由 Presenter 的 latch 和 generation 决定，而非依赖瞬时事件；UI Runtime
停止期间完成、随后恢复时也不会漏提示，同一代次不会在维护唤醒中重复显示。

## 4. 目录职责

| 目录 | 职责 |
| --- | --- |
| `include/` | 仅包含 `ui_runtime.h` 公共生命周期契约 |
| `core/` | UI Task、私有消息类型、主入口和页面路由 |
| `pages/` | 各产品页面的 LVGL 控件与刷新逻辑 |
| `widgets/` | 可复用 UI 控件 |
| `overlays/` | 当前仅保留番茄钟阶段完成横幅 |
| `foundation/` | UI 内部格式化和公共辅助 |
| `resources/`、`icons/` | 图标解析与静态视觉资源 |

## 5. 依赖关系

UI 只依赖 Presentation 契约、LVGL 和 Graphics 平台，且不包含 Application 或 Communication
头文件。Wi‑Fi、Portal、Dashboard 与 OTA 的差异已经由 Application 和 Presentation 转换为
页面契约，UI 不感知 `network_manager` 或传输协议。
UI 通过 [`include/ui_runtime.h`](include/ui_runtime.h) 对外暴露启停和状态；电源 Application
只能调用这组生命周期 API，页面数据和刷新仍完全通过 Presentation 解耦。
UI 用户意图回调由 Composition Root 注册，只允许非阻塞投递或短临界区操作，禁止在回调中调用
LVGL 或重新进入 UI Runtime。
设置菜单关闭意图还会由 Application 再次提交网页控制台停止请求，作为页面切换或控件树重建绕过
子页 Back 处理时的产品级清理门；只有 Application 状态快照明确为 `STOPPED` 后才接受关闭，
否则保留设置菜单门控并等待后续刷新。UI 页面反初始化本身不直接调用 Application。

## 6. Task 与生命周期

[`core/ui_task.c`](core/ui_task.c) 定义 `ui_runtime_task`，拥有：

- UI 业务队列和控制队列。
- `STARTING/RUNNING/STOPPING/STOPPED/FAILED` 状态机。
- LVGL、字体和控件树的初始化与反初始化上下文。
- Presentation 事件订阅及到 UI 私有消息的转换。

`stop()` 只关闭业务入口、停止 LVGL timer、等待显示 DMA 静止并把 Task 留在阻塞态；控件树、
字体映射、draw buffer、显示 Task、SPI 和面板控制器全部保留。`start()` 从 Presenter 重同步
保留控件树后恢复显示，并在返回 `ESP_OK` 前等待一次完整刷新传输完成；只有 `deinit()` 才在
UI Task 的内部 SRAM 栈上完整释放资源并让 Task 退出。

Task 栈必须位于内部 SRAM；停止态不得释放其队列、字体映射或图形资源。
`PRESENTATION_EVENT_STATUS_UPDATE` 使用独立的持久可合并 pending：默认 Event Loop handler
先在 Runtime 状态锁内置位，再以零等待尽力投递业务队列 marker，并始终用 Task notification
唤醒。UI Task 每次阻塞前、控制命令之后和两条业务消息之间主动取得 pending 并读取最新 View
Model；重复 marker 只作幂等唤醒。因此业务队列已满不会丢失网页控制台终态刷新，也不会从 Event
Loop 阻塞等待 UI。锁序固定为短时 Runtime 状态锁后释放，再访问 Queue/Task notification；
停止先关闭 `business_open`，等待已登记发送者退出并排空 marker 后清除运行态 pending；
期间丢弃的消息以及入口关闭后到达的状态刷新会合并到恢复快照，Task、队列和控件树继续保留。
生产者在 UI Task 最后一次检查后置位时一定同时发送 notification，Task 无论尚未阻塞或已经
阻塞都不会丢失唤醒。

停止收敛、控制命令入队或控制等待失败而恢复 `RUNNING` 时，会在同一个 Runtime 状态锁临界区
重新开放 `business_open`、强制置状态刷新 pending、复制仍存活 Task 句柄并登记活跃通知者，
随后在锁外通知。这样关闭入口窗口内主动丢弃或合并的状态会重新读取最新 View Model；通知登记
与停止排空共同保证不会访问已删除 Task。成功停止、启动初始化和 `UNINIT` 不走该恢复路径，
控制 Queue 仍在每轮刷新 drain 前优先。
设置 Screen 删除或 Runtime 停止时，页面同步删除 `lv_group` 和一次性结果 Timer，并清空所有
借用的 LVGL 句柄。普通 Light-sleep `stop()` 不删除 Screen；完整 `deinit()` 时才执行这组清理。

## 7. 验证

- 静态核对 UI 不包含 Application 或下层能力头文件。
- 静态核对所有 LVGL 写操作只从 UI Runtime 路径到达。
- 静态核对日历、邮箱、限额和语音页刷新只调用 `lv_label_set_text()`、`lv_obj_add_flag()` 与
  `lv_obj_clear_flag()`，且无旧 UI API、灰阶/透明度或空壳覆盖层残留。
- 本轮未运行固件编译、截图或实机交互检查；后续编译需通过统一命令
  `& .\ds.ps1 build deskmate`，交互验收需在目标硬件完成。

相关规范：

- [`../../docs/architecture/layering.md`](../../docs/architecture/layering.md)
- [`../../docs/architecture/data_flow.md`](../../docs/architecture/data_flow.md)
