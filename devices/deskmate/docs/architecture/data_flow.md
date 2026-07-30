# 数据流、并发与 Task

## 1. 基本数据流

Application 负责完整用例编排；下层组件负责执行并报告事实：

```text
Application
    ├── 同步调用 ──→ 直接返回最终结果
    └── 异步命令 ──→ 所有者组件 ──→ 不可变完成事件
```

同步 API 返回时操作已经完成。异步 `request` API 返回值只表示命令是否成功提交，最终执行结果
必须通过明确事件、结果队列或回调返回。

项目不建立无类型的全局业务事件总线。允许使用 ESP-IDF Event Loop 作为传输机制，但 Event
Base、Event ID 和载荷类型必须由事件生产组件定义；消费者显式注册，事件载荷保持不可变。
组件之间也可以使用显式回调、队列或 Task Notification。

大型数据不随通知复制。所有者维护受保护的有界快照或缓冲区，通知只携带版本、索引或轻量事实，
消费者收到通知后通过所有者 API 复制所需快照。

## 2. 状态所有权

- 每份可变状态、每个硬件资源只能有一个写入所有者。
- 只有所有者组件定义的串行化上下文可以修改工作状态；组件存在专用 Task 时，只能由该 Task
  修改。无 Task 的同步组件可以使用受锁保护的同步 API 或原子操作串行化。
- 快照必须整结构复制，不能返回内部可变指针，也不能让读取方观察到新旧字段混合。
- `volatile` 不能提供跨任务同步。必须使用队列、互斥锁、临界区、原子操作或 Task
  Notification。
- 状态锁只保护短时间内存操作；持状态锁期间禁止 I/O、延时、阻塞等待或调用外部回调。
  SPI/I²C 等完整硬件事务可以使用独立资源锁，但范围必须有界。
- 外部回调必须在释放内部锁后执行，并明确执行上下文。异步事件回调必须快速返回，需要耗时
  处理时再次入队；同步流式回调只允许在调用上下文执行有界工作。
- 回调收到的事件指针只在回调期间有效，不得保存原指针。
- 队列满时必须明确选择拒绝、覆盖最新值或合并重复命令，禁止静默丢弃。具体队列策略未确认
  时不得自行推断统一规则。

## 3. Task 归属与命名

Task 是执行机制，不是架构层。项目不得新建 `components/tasks` 或同义目录。
原 `main/tasks/` 已撤销；产品 Task 位于 `main/application/`，UI Runtime Task 位于
`main/ui/`，其他 Task 继续位于拥有对应状态和资源的组件中。

### Device/BSP 与 Service 的执行边界

- Device/BSP 只提供同步、短时、可组合的硬件能力、状态快照和快速事件入口，不创建或持有
  长期运行的 Task、Queue、Timer。
- Driver/Device 负责硬件语义状态机，例如按键去抖、协议解析和设备状态转换；状态机通过同步
  单步推进 API 执行，不自行决定产品调度周期。
- Service 拥有持续等待和推进状态机所需的 Task、Queue、Timer，负责周期调度、超时、技术性
  重试退避、自动恢复和跨设备协调。产品级致命、降级、跳过或重启决策仍由 Application 拥有。
- Service 必须通过稳定 Device API 操作硬件，禁止包含 BSP、Driver 或 Board 头文件。

DeskMate 按键链路应表达为：

```text
button_service 调度
    → Device/BSP 同步采样或硬件事件
    → Driver/Device 去抖与语义转换
    → 不可变按键事件
    → Application 决定产品动作
```

环境采样周期属于 Application 产品策略；`environment_service` 只拥有一次采样事务、快照一致性
和必要的设备恢复。若未来 Service 内部需要固定技术性恢复 Timer，应与产品采样周期分开描述。

只有组件需要独立阻塞、周期调度、持续状态机或串行独占资源时才创建 Task。Task 始终留在拥有
对应流程状态和资源协调责任的 Application、Service 或 Communication 组件：

- Application Task：串行处理产品意图、截止时间和跨组件用例状态。
- Service Task：持续硬件事件、去抖、音频管线或可复用事务的唯一状态所有者。
- Communication `network_manager_task`：拥有 Wi‑Fi 连接、断线、Portal 候选验证和一轮技术重试。
- Communication 其他内部 Task：只拥有可复用通信事务，例如固件目标校验、下载、镜像验证与
  分区切换；联网时机、页面提示、用户确认和产品重启条件仍由 Application 拥有。
- Device/BSP 不得创建长期 Task。ISR 或 SDK 回调只复制事实并通知上层所有者，不执行阻塞
  操作。
- RTC、温湿度、PMU 等没有独立阻塞源或持续流程时保持同步调用，不为分层形式增加 Service
  或 Task。
- Main 不成为永久调度 Task。

Task 文件命名是强制规则：

- 定义 Task 入口的 C 文件必须以 `_task.c` 结尾，C++ 文件必须以 `_task.cpp` 结尾。
- Task 入口函数必须以 `_task` 结尾。
- 公共头文件和 API 前缀按能力命名，不把 `_task` 暴露为模块名；例如
  `app_network.h` / `app_network_*` 的内部实现可以位于 `app_network_task.c`。
- Task 的创建、删除、`TaskHandle_t` 和主循环放在对应 Task 文件中。
- 禁止新增 `*_worker.c`、`*_thread.c` 或 `*_loop.c` 表示 FreeRTOS Task。
- Task 句柄、队列句柄不得出现在公共 API。
- 普通定时器或回调没有创建 Task 时，不使用 `_task` 文件后缀。
- Task 默认阻塞等待事件，不使用固定短周期忙轮询；默认不绑定 CPU 核，优先级和栈大小必须
  有测量依据。

## 4. DeskMate 主要数据流

### 输入到页面

```text
Button Device 事实
    → button_service 去抖/事件化
    → app_key
    → 对应 Application
    → presentation_dispatch 类型化呈现事件
    → UI Runtime Task
    → LVGL
```

`button_service` 不决定打开哪个页面或启动哪项功能；UI 不直接操作 Button Device。

### 环境数据

```text
Application 采样时机
    → environment_service
    → device_environment
    → BSP / Driver
    → environment_service 一致快照
    → Presenter 复制并生成 View Model
    → UI 拉取并呈现
```

设备型号、总线和原始单位在 Device/BSP/Driver 内消化，Service 与 Application 只看到统一单位。

### 网络、Dashboard 和 OTA

```text
Application 用户意图/产品时机
    → app_network 串行产品命令与租约
    → Communication network_manager 建立 Wi-Fi/Portal 会话
    → app_network 构造统一后端上下文
    → DeskMate Product Protocol / firmware_ota / remote_log / voice
      共同读取 URL、Token、稳定设备 ID、产品 ID 与固件目标
    → 类型化结果事实
    → app_network 收敛产品状态
```

Communication 不直接调用 Data、Presentation 或 UI，不决定是否保持联网，也不处理产品按键。
页面状态由设备 Application 本地拥有，页面切换不产生网络请求。
固件安装成功后的底层强制重启属于 `firmware_ota` 已声明的原子事务；是否发起检查或安装仍由
Application 决定。

Dashboard 的数据路径不经过四个领域中转组件：

```text
deskmate_api 执行 HTTP GET 并单次解析 JSON
    → 完整类型化 Dashboard 结果
    → dashboard_store 校验并复制整份快照
    → Weather / Calendar / Mail / Quota Presenter 显式刷新各自 View Model
    → 统一发布一次 Dashboard 呈现更新
    → UI Runtime 拉取最新 View Model
```

协议层拥有 schema、设备 ID、`next_refresh_at_utc` 和四个业务块的唯一 JSON 解析；Data Store
只提交完整类型化快照，不再次解析 JSON，也不向四个领域组件转发事件。Presenter 不订阅 Data
事件，而由 `app_network` 在完整快照提交成功后按固定顺序显式刷新。完成四次刷新尝试后只发布
一次轻量呈现通知，避免一次 Dashboard 响应形成四层中转和四次独立 UI 更新；若单个 Presenter
刷新失败，它保留上一份 View Model 并记录错误，其余 Presenter 仍可使用新快照。

成功 Dashboard 响应中的 `next_refresh_at_utc` 是清醒态和 Light-sleep 状态下下一次自动同步
的唯一正常调度权威。`app_network` 在清醒态把它换算为一次性 Timer；`app_power` 在
Light-sleep 前把同一截止时间换算为内部唤醒间隔。完整同步失败后才使用本地失败退避，失败
退避只负责错误恢复，不构成第二套正常刷新周期。

后端上下文是无 Task、无持久化状态的按值快照。持久化字段由 Application 装配，稳定硬件设备
ID 由共享 `protocol_identity` 生成，产品协议和通用 Tool 不得保存第二套身份来源。网络诊断
通过 `network_manager_get_diagnostics_copy()` 一次复制 Manager 会话事实，并实时补充 AP、
信道、认证、RSSI、IPv4、掩码、网关和 DNS；底层查询错误保留在快照字段中，不覆盖其余事实。

### 轻睡眠

```text
稳定按键事件
    → app_power 重置活动窗口
    → app_voice 有界收敛已经结束会话遗留的实时语音租约
    → Application 检查语音 / OTA / 网络租约阻止条件
    → 运行中的番茄钟页？
        → 是：app_network 有界停网，UI 保持运行并每秒刷新
        → 否：app_voice 有界停止
             → UI Runtime Task 有界停止
             → app_network 有界停网
             → Device / BSP 使用双按键 + 内部 Timer 进入 Light-sleep
               （显式测试配置可改用双按键 + PCF85063 Timer 的 RTC INT）
    → 维护源唤醒后恢复 UI、同步刷新屏幕，再次停止 UI 并继续睡眠
    → 按键唤醒后按网络 → 语音 → UI 恢复，提交按键事实并重新开始活动窗口
```

无活动截止时间、准备顺序、重试和失败阻断由 `app_power` 的唯一 Application Task 拥有。
`app_voice_reconcile_network_lease()` 只在语音会话已经空闲时释放本地仍记录的实时语音租约；
释放失败保留代次并作为普通阻止条件等待下一次有界重试，不直接把电源流程推进到 `BLOCKED`。
Device/BSP 只提供一次同步轻睡眠事务，并按编译配置锁存 Timer 或 RTC INT 维护唤醒事实，
不拥有周期刷新策略；RTC INT 测试模式使用调用方传入的间隔装载外部 RTC Timer，并在睡眠
返回前停止 Timer、清除 TF，但不启用 ESP32 内部 Timer。该测试配置默认关闭。
`app_network` 提供不依赖 UI 或 Light-sleep 的低功耗停网/恢复握手。番茄钟前台离线显示期间
Dashboard 截止到达时临时恢复网络完成维护，再次停网；用户活动或离开运行中的番茄钟页时恢复
正常网络策略。

`app_environment`、`button_service` 和 `rtc_service` 不参与本轮低功耗停启，生命周期保持
RUNNING；CPU 进入硬件 Light-sleep 后它们自然不执行，唤醒后无需重建 Task、Timer 或回调。
详细产品流程见
[低功耗流程](../低功耗流程.md)。

### UI 更新

任何非 LVGL 上下文产生的数据或事件必须先由 Presenter 更新 View Model，再通过
`PRESENTATION_EVENT` 发布轻量呈现事实。UI Runtime 将事件转换为私有 `ui_msg_t`，只有其
LVGL 上下文可以创建、删除或修改界面对象。Presenter 不得包含 UI 头文件，UI 也不得包含
Application 或下层能力头文件。电源 Application 只允许包含公共 `ui_runtime.h` 完成启停握手，
不得访问页面、控件、路由或私有消息。回调不得持有底层状态锁跨越呈现事件发布。

Presentation 与 UI 的数据流为：

```text
下层或 Application 不可变事实
    → <feature>_presenter 更新有界 View Model
    → PRESENTATION_EVENT 仅通知“什么需要刷新”
    → UI Runtime Task
    → UI 通过 <feature>_presenter_get_view_copy() 复制最新 View Model
    → LVGL 渲染
```

## 5. 生命周期

只有确实存在相应阶段的组件才提供完整生命周期：

```text
UNINITIALIZED → INITIALIZED → RUNNING
       ↑             ← STOP ←
       └──── DEINIT ────────┘
```

非法状态调用返回 `ESP_ERR_INVALID_STATE`。`start()` 失败必须回滚到 `INITIALIZED`，不能留下
半创建的 Task、队列或回调。简单同步组件不为形式增加空的 `start/stop/deinit`。

异步组件的 `stop()` 必须在公共契约中说明是同步等待终止还是只提交停止请求。可失败停止未达到
终态时不得释放其 Task、队列、回调或底层资源。

## 6. 产品调度与有界会话的架构边界

- Application 拥有页面交互、日历时间窗口、截止时间、重试、跳过和降级策略；这些产品参数
  不属于通用架构规则。
- System Clock 只提供带可信状态的时间快照和校时能力。日历计算使用可信墙上时钟，清醒期间
  的超时等待使用单调时钟，不能维护相互竞争的两套持续走时。
- Communication 只报告关联、IPv4、Portal、超时和传输结果等事实，并执行调用方发起的有界
  会话；是否开始、延长、结束或跳过产品流程由 Application 决定。
- Data/System/Device 只报告缓存、持久化或设备操作结果，不决定失败后展示错误、重试、降级
  或重启等产品策略。
- 具体采样周期、联网顺序、OTA 提示、语音会话和页面收敛属于产品要求；调整这些要求不等于
  修改架构分层。
