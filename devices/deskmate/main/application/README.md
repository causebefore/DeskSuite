# Application

> Application 层负责 DeskMate 产品用例、用户意图、产品调度和跨能力状态收敛。

## 1. 定位

- 触发方：Composition Root、`button_service` 事实事件和其他明确的产品事件。
- 主要输出：向下层提交命令、更新产品状态，并通过 Presentation 发布页面或设置动作。
- 当前构建：本目录仍属于 `main` ESP-IDF 组件，不是独立聚合组件。

## 2. 职责边界

负责：

- 页面导航、按键语义、配网入口、OTA 时机、语音会话、网页控制台和低功耗顺序。
- 产品采样周期、Dashboard 同步时机、截止时间、重试、跳过和降级判断。
- 跨 Service、Communication、Data、System 和 Device 的完整用例编排。
- 拥有产品流程状态的 Task、Queue 和 Timer。

不负责：

- 不定义 LVGL 控件、样式或页面布局。
- 不把领域快照格式化成 View Model；该职责属于 Presentation。
- 不替 Service 管理可复用事务，也不包含 BSP、Driver 或 Board 头文件。
- 不以 Task 类型建立独立目录；Task 始终跟随状态所有者。

## 3. 主要流程

```text
用户或系统事实
    → Application 判断产品动作
    → Service / Communication / Data / System / Device 执行
    → 不可变结果事实
    → Application 收敛状态
    → Presentation 呈现
```

按键双边沿输入事件流为：

```text
GPIO 双边沿 → BSP ISR → Device 活动事实 → Button Service 临时 one-shot 扫描
    → Device 稳定事件 → app_key → 默认事件循环
```

GPIO ISR 只提交活动事实；`button_service` 在 ESP Timer Task 上下文完成消抖、长按推进和事件
转发，`app_key` 再把稳定事实解释为产品输入。

## 4. 依赖关系

| 方向 | 层级 | 用途 |
| --- | --- | --- |
| 调用 | Presentation | 发布页面切换、刷新等类型化呈现事件 |
| 调用 | UI Runtime | 仅由电源用例执行有界启停与状态查询 |
| 调用 | Service / Communication | 执行持续流程、联网事务和共享资源协调 |
| 调用 | Data / System / Device | 使用稳定同步能力与状态快照 |
| 调用 | `utils` | 低频输出 Application Task 的历史最小剩余栈 |
| 被调用 | Main | 装配并启动产品用例 |

Application 可以依赖 Presentation；电源用例还可以依赖 UI 的窄化 Runtime 生命周期契约。
Presentation 和 UI 均不得反向包含 Application 头文件。

## 5. 当前模块

| 模块 | 职责 |
| --- | --- |
| `app_key` | 把稳定按键事实转换为产品输入 |
| `app_page` | 维护顶层环形页面、Screen 切换门控和 500 ms 完成事件兜底 |
| `app_pomodoro` | 拥有本地番茄钟状态机、设置版本与异步更新结果、完成音乐逻辑路径、单调 deadline、日期归一化、NVS 计数、前台秒级显示事实和睡眠唤醒补算 |
| `app_settings` | 保存线程安全的设置菜单开启门控，把按键转换为设置动作或配网请求，并只在网页控制台明确停止后清理会话 |
| `app_ota` | 手动检查、确认安装、目标丢弃和 OTA 导航锁定 |
| `app_voice` | Processor → Voice 的产品生命周期、连续语音入口和整段会话网络租约；不转发通用音频状态 |
| `app_web_console` | SD/在线前置检查、Hub/Pomodoro/设备与系统三项产品 Provider 装配、网页控制台网络租约、Service 启停与安全回滚 |
| `app_power` | 拥有 30 秒活动窗口、番茄钟前台离线显示、语音/UI/网络可逆启停、内部 Timer 维护刷新和按键唤醒闭环 |
| `app_environment` | 电池与温湿度产品采样周期 |
| `app_network` | Hub URL 快照、版本、测试/更新共用单 pending 与结果、相关 I/O，及 Network Manager 会话退避、配网活动停网保护、统一后端上下文、Dashboard 截止与失败退避、同步维护回执、OTA、远端日志、互斥租约、链路通知和低功耗停网握手 |

番茄钟数据流为：

```text
按键、本机 UI 设置意图或浏览器 Settings Provider
  → 携带读取时的 settings_version
  → app_pomodoro 命令队列
  → app_pomodoro_task 串行推进状态
  → pomodoro_store 保存设置/完成计数
  → pomodoro_presenter_apply()
  → 专用 Presentation 刷新事件
```

运行阶段只使用 `esp_timer_get_time()` 计算 deadline 和剩余时间，不依赖 RTC、SNTP 或网络。
可信系统时间只用于把完成数归入本地日期、安排午夜 one-shot Timer，以及生成预计结束时间。
阶段状态、暂停剩余和当前轮次不写 NVS，设备重启固定回到 `IDLE`。Power Application 的同步
补算命令返回前已经更新状态和 Presenter，因此睡眠后恢复 UI 的首帧可直接显示 `DONE`。
番茄钟设置版本独立于阶段 `generation`，从 1 开始且只在 NVS 成功后递增。本机 UI 草稿与
浏览器稀疏更新都携带读取时版本；请求接受点和 Task 执行点在状态锁内再次检查请求 ID、单
pending、版本与 `IDLE`，从而拒绝陈旧提交。Task 按值复制候选设置和 Store 结构后释放状态锁
写 NVS；原请求在锁外 I/O 期间保持 `PENDING`，新的设置请求仍被单 pending 门拒绝。
重新持锁后，只有保存成功才一次性公开候选设置、递增版本、更新空闲阶段派生字段并将
`settings_saved` 置为 true；保存失败保留旧设置、版本、派生字段和原 `settings_saved`，只把
原请求收敛为 `FAILED` 并记录真实存储错误。本机 UI 意图的同步接受结果返回同一个请求 ID；
Application Task 把最新 ID 与结果按值推送给 Presenter，UI 只消费匹配 ID 的终态，不能把
“已入队”当成“已保存”。

阶段完成后，Pomodoro Task 在释放状态锁并发布快照后，把持久化的 `.mp3` 逻辑路径映射到
`/sdcard`，再以非零 `completion_generation` 向 Audio Service 提交绝对路径。旧 schema 自动
迁移到 `/pomodoro-complete.mp3`，升级后保持原固定文件行为。自然 TICK 完成和 Light-sleep
唤醒补算复用同一提交逻辑；Confirm、Reset 或开始下一阶段时在锁外取消旧代次。播放请求失败
只记录事实，不回滚 DONE、完成计数、持久化或 10 秒活动窗口。

Composition Root 在 Audio、Processor 与 Voice 都进入运行态后，向 Pomodoro Task 提交一次
开机提示音。Task 仅在 `IDLE` 接受，并复用相同的逻辑路径映射与 Audio Service 播放入口；
Audio Service 使用 50% 默认输出音量，提交失败只记录日志，不阻断其余启动流程。

`app_network` 不直接操作 Wi‑Fi Driver、Portal HTTP/DNS 或底层重连状态机；这些技术能力属于
Communication 的 `network_manager` 和 `connect`。Network Manager 一轮内部重试结束后，
`app_network` 才根据产品策略决定是否延时建立新会话。

`app_network_get_backend_context_copy()` 从持久化服务地址和 Token、编译期 `product_id` /
`firmware_target` 及共享硬件设备 ID 构造一次完整值快照。Dashboard、OTA、远端日志和语音
会话都复制该快照，不得各自读取设置或生成设备 ID。Network Manager 的完整诊断快照则统一
报告本次会话计数、最近断链原因以及实时 AP、信道、认证、RSSI、IPv4、网关和 DNS 事实；
Application 只消费这些事实，不把产品重试策略写回 Communication。

`app_network` 是 Hub URL snapshot、严格单调 version、测试/更新共用单 pending、异步结果和
网络/持久化 I/O 的唯一所有者。Web Settings/Actions 与 Portal 两个入口都先调用同一纯 C helper
规范化地址；Portal 保存用短锁 reservation 与 Web pending 互斥，锁外保存其完整网络配置，避免
SSID、密码或 Token 与 Web Hub 更新交错覆盖。测试请求只对候选的 `/healthz` 做无凭据有界 GET，
不持久化；更新请求不复用旧测试结果，必须在 Network Task 对同一候选重新健康检查，再只提交
一次包含新 `service_url` 的 `network_cfg`。持久化成功后才在同一短锁中发布规范化 URL、递增
version 并立即使旧测试事实失效；规范化或存储失败保留旧 snapshot/version。随后远端日志按
stop/configure/start 最佳努力切换，失败只记录事实且不回滚已提交 URL。调用前已经复制后端
上下文的在途事务继续使用原值，新 URL 只影响后续新建事务。

设置菜单的数据流为：

```text
button_service → app_key → app_settings → Presentation 设置动作 → UI
UI 用户意图 → app_main → app_settings / app_ota / app_web_console → app_network / web_console_service
```

焦点、菜单历史和子页位置只属于 LVGL。`app_settings` 不复制这些状态；离开设置页、UI
重建或轻睡眠准备时，先非阻塞提交网页控制台停止意图，再读取 Application 运行摘要；只有
明确到达 `STOPPED` 才清除菜单门控与尚未安装的 OTA 目标。仍在启动、运行、停止或保留资源的
错误态一律关闭失败并保持当前导航门，不能把任意 `ESP_ERR_INVALID_STATE` 当作安全终态。
手动 OTA 检查始终要求用户确认，即使持久化自动安装策略已开启；自动检查来源才允许继续应用
自动安装策略。

Dashboard 的 HTTP 与 JSON schema 契约位于 Product Protocols：
`deskmate_api_get_dashboard()` 对成功响应只解析一次并返回完整类型化结果，`dashboard_store`
只校验并提交整份快照，不缓存原始 JSON，也不再次解析。`app_network` 拥有拉取时机、重试和
Weather → Calendar → Mail → Quota 的显式 Presenter 刷新顺序；四个 Presenter 直接读取
`dashboard_store` 对应切片，不再经过四个中间 Data 组件或订阅其事件。完成四次刷新尝试后只
发布一次统一呈现更新；单个 Presenter 刷新失败时记录错误并保留该页上一份 View Model，已有
有效数据改标 `STALE`，尚无数据改标 `ERROR`，且不阻断其余页面采用新快照。完整同步失败或
Network Manager 离线时，`app_network` 会在统一页面刷新前把四个旧 `OK` View Model 标记为
`STALE`，避免缓存数据继续显示为在线。401 只收敛为鉴权失败，不清除 Token 或尝试注册。成功响应中的
`next_refresh_at_utc` 是下一次 Dashboard 自动同步
的唯一正常调度权威：清醒态使用一次性 `esp_timer` 对齐绝对截止，Light-sleep 使用同一截止
时间决定何时恢复网络。完整同步失败后才以
`CONFIG_DESKMATE_DASHBOARD_FAILURE_RETRY_SEC` 为第一档，连续失败依次按 1、5、15、60 倍退避并
在最后一档封顶；成功后回到第一档。失败退避保存包含 Light-sleep 时间的单调截止，RTC/SNTP
暂不可用时也不会在每分钟屏幕维护唤醒时提前联网。本地退避只负责错误恢复，不再存在 NVS 相对刷新周期。
系统时间重新校准后会按同一绝对截止重新换算清醒态 Timer。它还在每个网络会话上线
后按当前服务地址和稳定设备 ID
启动产品标识为 `2` 的远端日志上传，并在停止 Network Manager 前同步停止上传 Task，不额外延长
在线窗口或增加轻睡眠唤醒就绪事件。远端日志只在网络上线且服务地址有效时才以 8 条队列、4 条批次
的低内存配置初始化，日志突发可丢弃但不得影响轻睡眠等核心产品 Task。音频采集、处理和语音
事务仍由 Service 链拥有；`app_voice` 只串行编排 Processor 与 Voice 的 `start/stop`，只在
`RUNNING` 且当前为语音页面时解释右键长按，并为一次连续会话申请实时语音租约。Voice Service
在每轮回复排空后自动再次录音，默认后续 5 秒没有有效人声才正常结束；同一租约覆盖全部回合并在
唯一终态释放。当前不启用运行时唤醒词设置。

`app_network` 同一时刻只授予一个带类型和代次的互斥网络产品租约，当前类型为实时语音和
网页控制台。两类租约都阻止 Dashboard 手动/自动同步、OTA 检查与安装、显式或无配置自动
Portal，以及整机进入 Light-sleep；对应租约释放成功后才按当前产品开关恢复 Dashboard 和
OTA Timer。租约只占用这些产品策略，不停止 Network Manager 的技术状态机：网页控制台 Service
运行期间，已保存 STA 仍可在断线后重连并更新 IPv4 地址。显式或无配置自动 Portal 请求在
网络 Application Task 内先用同一状态锁检查活动租约并占用 Portal 过渡状态，Network Manager
立即拒绝或发布明确状态后才清除该占位，因此异步 Portal 切换窗口不能插入新租约。
`app_network` 还允许唯一固件进程期静态订阅者注册链路变化借用回调。回调在现有耐久 Manager
pending 标志由 `app_network_task` 收敛后复制，并在网络状态锁外调用；它不携带事件历史或
Manager 内部指针，订阅者只能合并自身通知并重新读取最新事实。网页控制台订阅回调只在自身
Task 锁内设置 pending 并发送 Task notification，不访问磁盘、Presenter 或网络控制 API。

租约 release 命令必须先原子认领仍匹配请求、尚未过期的同步回执槽，随后才能清除活动租约；
已超时并由调用方放弃或已经复用的槽不会改变租约。租约代次从 `1` 单调发放到 `UINT32_MAX`，
最大值只发放一次，之后以 `0` 作为耗尽哨兵并拒绝新租约，直至设备重启，避免极旧句柄重新匹配。

网页控制台的完整数据流为：

```text
设备设置页选择“网页控制台”
  → app_web_console 检查 /sdcard
  → app_network 授予 APP_NETWORK_LEASE_WEB_CONSOLE
  → web_console_service 恢复事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → Files handler 串行浏览、下载、事务上传或执行单项目录/文件变更
  → 浏览器顶层显示“文件管理 / 设置 / 退出登录”，并在“设置”首页呈现 Hub、番茄钟、设备与系统三个客户分组
  → 首页只显示分组说明；进入二级详情后才读取对应 Settings 或 Status 快照
  → Hub Settings/Actions 经 app_web_console_provider 共享同一个 hub section
  → 番茄钟 Settings 经 app_web_console_provider 提交版本化更新并查询结果
  → 设备与系统 Status 经 app_web_console_provider 读取单份系统快照
  → 设备返回时 Service 安全停止后释放网络租约
```

`app_web_console.cpp` 拥有产品阶段、网页控制台网络租约代次、是否仍需清理 Service 的事实以及运行摘要
边界；`app_web_console_task.cpp` 独占停止意图、Task 句柄、Task 创建/删除和生命周期执行。
`app_web_console_provider.c` 是无状态产品适配层，只把通用 Hub、Pomodoro 和系统字段映射到
`app_network`、`app_pomodoro` 与 `system_info` 公共 API，不拥有请求、版本、队列、持久化、
HTTPD 或 Network Manager 诊断状态。`web_console_service` 独占 HTTPD、认证、handler、文件
事务和传输资源。Composition Root 必须先初始化
`app_pomodoro`，再初始化会借用其 Provider 的 `app_web_console`；失败时按相反顺序回滚。
Application 在授予租约后还会复核 Network Manager `ONLINE` 与当前 STA IPv4；它不因 STA
短暂断线停止 Service。Service 启动成功后，`app_web_console_task` 先在
状态锁外读取当前内存链路与 Service 状态，验证六位访问码，再在一个状态锁临界区写入 URL、
访问码和 `RUNNING`，随后只发布一次完整 Presenter 快照。运行期间 Network Manager 断线、
重连或 IPv4 更新通过上述双层合并通知唤醒同一 Task；Task 在没有停止请求时重新读取
`connect` 内存快照，断线时清空 URL，重连或地址变化时替换 URL，访问码和 HTTPD 生命周期
保持不变。公共状态 Getter 只在短临界区复制已经收敛的快照，不轮询 Network、Service 或文件
系统。每次产品状态迁移及实际 URL 变化后，Application 都在自身状态锁内为待推送快照分配严格单调的 64 位
展示版本，再在锁外向 Presenter 推送完整有界展示事实。Presenter 只接受晚于已应用版本的
输入；Application 的私有静态互斥量把 Presenter 仲裁、持久刷新 pending 和轻量呈现事件入队
串成一个受控步骤，更高版本只能在当前已接受版本完成派发尝试后生效，因此并发 Getter 和状态
迁移不会让旧调用产生滞后的刷新事件。Presenter 接受新版本后先置 pending，只有默认 Event
Loop 接受 `STATUS_UPDATE` 才清除；任何后续状态推送也会重试当前 pending。运行等待阶段的同一
一次性 Task 使用有界间隔只重试事件入队，不轮询网络；启动错误、停止错误和 `STOPPED` 终态
在 Task 解绑删除前必须完成入队，期间更晚停止序列始终优先并进入下一轮清理。版本
`UINT64_MAX` 只使用一次，耗尽后拒绝继续推送且不发布刷新事件，也拒绝
新的启动意图，避免页面仍显示可安全退出的 `STOPPED` 时继续取得资源；已经活动的流程仍允许
执行停止清理。版本禁止回绕复用；Presenter 不反向读取 Application 或资源所有者。

一次性 Task 创建失败属于 request API 的同步拒绝，不是已接受异步命令的终态。创建门仍被
当前调用占用且 `s_task` 尚未发布时，Application 在 Presenter 推送互斥量内写入最新
`ERROR` View Model；该版本被接受后清除本命令准备态尚未进入默认 Event Loop 的刷新 pending，
不新增无人重试的异步事件。UI 调用方按 request 的非 `ESP_OK` 返回立即读取 Presenter 并携带
`action_error` 重绘；若此前事件已成功入队，它处理时也只会读取最新 ERROR。启动 Task 创建
窗口内已经返回成功的并发停止意图在释放创建门后接续一次清理创建；停止 Task 自身创建失败不
递归重试，保留停止序列、Service 和租约所有权，由收到同步错误的调用方稍后再次提交停止。

停止严格按以下顺序收敛：

```text
停止意图 → STOPPING
  → web_console_service_stop(6000 ms)
  → web_console_service_deinit()
  → app_network_release_web_console_lease()
  → STOPPED
```

`web_console_service_stop()` 超时、清理失败或 `deinit()` 未完成时，Application 保留租约和
明确错误态，后续停止意图只重试同一清理所有权；绝不在 HTTPD 或 handler 仍可能存活时释放
租约。Service 已反初始化但租约释放失败时同样保留代次供幂等重试。启动失败只逆序释放本轮
实际取得的资源；Service 报告 `STOPPING/CLEANUP_FAILED` 时保留租约，不能伪装为可退出。
任何 request API 返回非 `ESP_OK` 时均不承诺后续 Presentation 事件，调用方直接处理同步错误。
当前 Files 契约支持创建目录、常规文件移动/重命名，以及常规文件和空目录的单项删除；不包含
递归目录删除、目录移动/重命名、WebDAV 或 WebSocket。产品 Provider 最终只有 Hub、Pomodoro、
设备与系统三项：Hub Settings 与 Hub Actions 使用同一个 `hub` section；Pomodoro Settings 暴露
四项时长和一个完成音乐逻辑路径，后者通过通用 `.mp3` 文件选择元数据复用认证后的 Files 目录
接口，Provider 回调本身不遍历 SD 卡；设备与系统 Status 只暴露七项系统只读事实，不再组合
调试型 Network Manager 诊断 Provider。产品不提供 Wi-Fi 分类；网络 SSID、敏感凭据、OTA、重启
和 Dashboard 不进入字段表或 Actions。
浏览器批量文件操作只是顺序调用单项接口，不声明跨多个目录项的原子事务。

同步回执 waiter 在同一个 `s_state_lock` 临界区完成 deadline 最终仲裁：`COMPLETED` 先复制
结果并释放槽，`EXECUTING` 解锁后等待最终信号，已到期的 `PENDING` 当场释放；不存在检查
执行态后再分次放弃槽的窗口。Network Manager 通知使用耐久 pending 标志，队列 marker
只负责唤醒；即使 marker 因队列已满无法投递，网络 Task 也会在每次阻塞接收前主动收敛最新
Manager 快照，不使用周期轮询或额外 Task。

## 6. Task 与状态所有权

| Task 文件 | 唯一所有状态 |
| --- | --- |
| `app_power_task.c` | 无活动窗口、离线显示状态、睡眠编号、Timer 刷新计数、按键唤醒状态和失败终态 |
| `app_environment_task.c` | 两类产品采样截止时间和采样命令 |
| `app_network_task.c` | Hub URL snapshot/version、测试/更新共用单 pending 与结果、Hub I/O、网络产品命令队列、Dashboard 截止与失败退避、OTA、类型化互斥租约、会话退避和策略 Timer |
| `app_pomodoro_task.c` | 番茄钟阶段状态、设置版本、单 pending 设置请求及其最新结果 |
| `app_web_console_task.cpp` | 网页控制台启动、运行和可失败停止的一次性产品状态机 |

Task 入口、句柄、队列和主循环都留在对应 `_task.c` 内，公共 API 不暴露 RTOS 句柄。

`app_web_console_task.cpp` 的一次性 Task 只在启动、运行和停止期间存在。它不创建命令队列：重复启动
明确拒绝，每个有效停止意图都在 Task 锁内取得严格单调的 64 位序列并通知活动 Task；链路变化
使用同一把 Task 锁合并为耐久 pending，并通过 Task notification 唤醒。Task 醒来始终先检查
停止序列，只有仍处于 `RUNNING` 且没有停止请求时才刷新 URL；停止清理期间到达的链路通知在
解绑 Task 时清除，不能在 Service 停止后发布运行快照。每轮
有界清理先记录已消费序列；失败后在同一把锁内比较最新序列，只有没有更新请求时才解绑并
进入 `ERROR`，否则立即继续下一轮。成功进入 `STOPPED` 可以原子消费全部并发停止请求。
`UINT64_MAX` 只分配一次，耗尽后停止与后续启动都拒绝，避免回绕后吞掉清理重试。Task 退出前
清空私有句柄。

当前低功耗阶段把“停网”和“Light-sleep”拆成两个可组合步骤。无活动窗口结束后，如果
`app_pomodoro_requires_live_display()` 报告运行中的番茄钟正处于前台，`app_power` 进入
`APP_POWER_STATE_OFFLINE_DISPLAY`：只由 `app_network` 停止 Dashboard 截止、失败退避及其他
产品 Timer、远端日志、Network Manager 和 Wi-Fi Driver，UI Runtime 与一秒番茄钟 Timer
继续运行，并持有 `ESP_PM_NO_LIGHT_SLEEP` 锁阻止自动 Light-sleep，同时允许 DFS 在刷新间隙
降低 CPU/APB 频率。Dashboard 截止到达时临时恢复网络并完成维护，活动代次未改变则再次停网；
任意按键、阶段完成或离开运行中的番茄钟页都会释放 PM 锁并恢复正常网络策略。

配网 Portal 首次进入 `PROVISIONING/VALIDATING`，或其显式用户活动序号前进时，`app_network`
以独立的 5 分钟窗口（`APP_NETWORK_PORTAL_ACTIVITY_WINDOW_SEC`）建立单调停网保护截止。该窗口
不与整机按键空闲窗口共用配置：手机扫码、连热点和等待 Captive 弹窗期间不会刷新设备本地活动
时钟，保护时长必须覆盖这段无本地活动的人机交互。在截止前到达的低功耗
停网命令返回 `ESP_ERR_INVALID_STATE`，由 `app_power` 沿既有退避重新尝试；停网前若读到尚未由
Application Task 收敛的新活动序号，也先拒绝本轮命令，避免通知与停网交错时关闭热点。自动状态
查询不推进活动序号，因此连续无用户活动后仍会正常停网，不会让 Portal 永久保持。

每次收集低功耗阻止条件前，`app_power` 都同步调用
`app_voice_reconcile_network_lease()` 修复语音侧可能遗留的实时租约。没有本地租约时直接
成功；语音 Service 仍忙时保留租约并返回 `ESP_ERR_INVALID_STATE`；释放失败时同样保留原代次，
供下一轮幂等重试。该错误本身不进入 `BLOCKED`，仍存在的租约会作为正常阻止条件按既有退避重试。

其他场景继续执行完整 Light-sleep：睡前先确认没有活动采集或异常音频状态，再关闭语音新会话
入口并使 AFE Task 停泊、输入关闭；Audio Service 有待播放、播放中、排空中或取消中的输出事务
时暂缓睡眠，其自身 Task 在空闲时保持常驻停泊。关闭 UI 与网络后会再次读取三个 Service，避免
提示音恰在首次检查与入睡之间提交。随后停止 LVGL timer、等待显示 DMA 静止并进入睡眠。
ESP32 内部 Timer 默认每 60 秒唤醒一次，若服务端截止时间更近则缩短本轮间隔以对齐计划整点；
普通周期保持停网；可信 UTC 到达 Dashboard 返回的 `next_refresh_at_utc` 后，电源 Task 恢复
网络、同步等待 Dashboard 完成、保存新截止时间并再次停网，随后 UI 从 Presenter 重同步并
等待完整显示传输。同步失败保留旧截止时间，并按默认 1、5、15、60 分钟的本地退避截止重试；
一分钟屏幕维护唤醒不会提前联网，因此失败退避不增加唤醒次数，只减少 Wi-Fi 恢复次数。Timer 维护窗口不启动语音
Runtime；左右键唤醒则按以下链路恢复产品按键事实：

```text
EXT1 左右键掩码 → app_power 按网络 → 语音 → UI 恢复
    → button_service_request_light_sleep_wakeup_copy()
    → 原按键状态机或快速释放短按重放
```

Button Service 保持 RUNNING，不增加睡前 `stop()` 或醒后 `start()`；提交失败属于恢复错误
并进入 BLOCKED。普通按需扫描使用配置的 10 ms 周期；Device 扫描或唤醒后物理状态读取失败时，
下一次单次扫描固定退避 250 ms，成功后立即恢复正常周期，避免错误状态下持续高频唤醒。
活动录音、上传、播放、AFE drain、异常停机后残留的音频输入输出或语音租约是暂时睡眠阻止
条件，不会在睡前取消会话；会话结束后按 10 秒配置重试。语音 Runtime 的 Light-sleep
`stop()` 保留 Codec、AFE、模型、缓冲和已创建 Task，`deinit()` 不进入每轮睡眠路径。
Dashboard 同步失败是可重试的数据错误；语音、网络或 UI 无法证明已安全停止/恢复才进入
`BLOCKED`。

`app_environment` 的同一 Application Task 继续分别拥有 2000 ms 电池采样截止时间和
30000 ms 温湿度采样截止时间。OTA Timer、Firmware OTA 初始化/启动和 Network Task 生命周期
本次按键调度改造不变。

## 7. 验证

- 静态核对除 `app_power_task.c` 的 `ui_runtime.h` 外，Application 不包含 UI 头文件。
- 静态核对所有 Task 文件和入口均以 `_task` 结尾。
- 按键按需扫描和 Light Sleep 桥接执行
  `.\tools\tests\check_button_event_driven.ps1`。
- 语音生命周期和低功耗顺序执行
  `.\tools\tests\check_voice_power_lifecycle.ps1`。
- 低功耗维护契约执行 `.\tools\tests\check_power_rebuild_stage1.ps1`。
- 网页控制台产品 Provider 回归执行
  `.\tools\tests\check_web_console_product_providers.ps1`。
- Hub URL host helper、Portal/Web 互斥、单 Blob 提交和远端日志重配契约执行
  `.\tools\tests\check_app_network_hub_settings.ps1`。
- 固件编译必须通过仓库统一命令 `& .\ds.ps1 build deskmate` 执行。

上述 PowerShell 与网页 Python/Node 验证只覆盖 host/static 契约，不等于固件编译、OTA 发布、
设备安装、真实设备、浏览器交互或真实 Hub 验收。

相关规范：

- [`../../docs/architecture/layering.md`](../../docs/architecture/layering.md)
- [`../../docs/architecture/data_flow.md`](../../docs/architecture/data_flow.md)
