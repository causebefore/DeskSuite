# 项目分层与组件依赖

## 1. 总体结构

项目采用以下目标分层：

```text
Main / Composition Root
        ├──→ Application ───→ Presentation
        │          │                 ↑
        │          ├──→ Service（可选）│
        │          └──→ UI Runtime ───┘
        │          │
        └──────────┴──→ Communication | Data | System | Device
                                      ↓
                                     BSP
                                ┌─────┴─────┐
                                ↓           ↓
                             Drivers      Boards
```

层级表示允许的编译依赖，不表示一次调用必须机械地穿过每一层。整体依赖必须构成无环有向图。

## 2. 层级职责

### Main / Composition Root

Main 必须只负责顶层启动顺序、关键失败分支、依赖装配，以及启动顶层组件。具体启动阶段可以
委托给单一启动 Application，避免入口文件堆积设备初始化、回调适配和局部回滚细节。Main 不得
包含页面切换、网络重试、OTA 时机、语音策略或环境采样周期等运行期产品决策，也不得成为永久
业务循环。

### Application

Application 负责完整用例的触发时机、调用顺序和产品策略，包括页面状态、用户意图、联网时机、
OTA 交互、语音会话、数据刷新以及必需/可选能力的故障分类。

Application 可以直接调用 Communication、Data、System 和 Device 的稳定公共 API。
不能为了追求形式完整而强制增加 Service 包装。顶层启动 Application 可以为完成启动阶段用例
而单向调用其他 Application 的稳定公共 API；普通功能 Application 不得形成横向循环依赖。

当前 `main/application/` 是 Application 的实现位置。定义 FreeRTOS Task 的 Application 文件
仍以 `_task.c` 结尾，但 Task 不形成独立目录或架构层。若未来某个用例需要独立构建、复用和生命周期，
可以迁移为独立 ESP-IDF 组件；不得只为目录对称提前拆组件。

### Presentation

Presentation 是 Application 与 UI 之间的呈现适配层：

- 订阅 Application、Service、Communication、Data 或 System 发布的不可变事实事件，并通过稳定 API
  复制所需快照。
- 只执行字段裁剪、单位与枚举映射、显示状态归一化和有界文本格式化，生成只读 View Model。
- 定义 Application 与 UI 共享的页面呈现契约，并发布带类型的不可变呈现事件。
- 不处理按键产品语义，不启动 Portal、OTA、语音或网络会话，不拥有重试、降级、生命周期与
  资源协调策略。
- 不创建 Task、Queue 或 Timer；Presenter 回调必须快速返回，LVGL 更新由 UI Runtime 执行。

当前 `main/presentation/` 是 Presentation 的实现位置。Presenter 以
`<feature>_presenter.[ch]` 命名，跨页面 View Model 放在 `presentation_view_model.h`。Presentation
可以依赖稳定的下层事实 API，但不得包含 Application 或 UI 头文件；Application 和 UI 可以
单向依赖 Presentation 契约。

### UI

UI 是 LVGL 呈现边界：

- 负责 LVGL 页面、控件、样式和可视状态呈现。
- 只在 LVGL 所属上下文修改界面对象。
- 把按键、触摸或页面动作转换为用户意图交给 Application。
- 不直接拥有网络、OTA、语音、存储或设备重试策略。

UI 只依赖 Presentation 的 View Model、页面契约和呈现事件，以及 Graphics 平台接口；不得
包含 Application、Service、Communication、Data、System 或 Device 头文件。UI Runtime Task 只拥有
LVGL 执行上下文、UI 私有消息队列和启停握手，不因此拥有产品策略。

UI 通过 `ui_runtime.h` 暴露窄化的启停与状态契约。电源 Application 可以调用该生命周期契约
完成整机轻睡眠事务，但不得调用页面、控件、路由或私有消息接口；这条控制依赖不允许 UI 反向
包含 Application。

### Service（可选）

只有满足以下至少一种条件时才应使用 Service：

- 一个完整事务跨越多个下层组件，并要求固定顺序、提交、回滚或补偿。
- 需要由一个组件独占共享资源或事务状态。
- 同一复杂流程被多个 Application 用例复用。
- Device/BSP 的同步能力需要持续等待、周期调度、去抖执行、超时重试或自动恢复，并需要独立
  拥有 Task、Queue、Timer 或等价执行资源。

只转发一个下层调用、只转换函数名或没有独立事务语义的 Service 属于过度封装。Service 只依赖
稳定下层 API，不得感知 GPIO、总线或芯片型号。Service 私有实现可以直接使用 FreeRTOS Task、
Queue、Timer 等执行机制，但不得通过公共 API 泄漏 RTOS 句柄。

### Communication / Data / System / Device

这些组件提供稳定的技术、产品数据或设备能力，不负责顶层产品时机和致命/降级决策：

- Communication：位于 DeskSuite `shared/` 的可复用 Wi‑Fi/Portal 链路状态机、完整网络诊断
  快照、传输、稳定硬件身份、统一后端上下文、SNTP 网络取样、OTA 和日志上传基础能力；
  Dashboard 等产品协议保留在本设备 `components/product_protocols/`。`network_manager`
  只拥有技术连接状态、候选配置验证和一轮组件内重试。
- Data：产品数据结构、缓存、解析结果和对应的持久化语义。
- System：可信时间、复位、看门狗和通用存储机制；实际目录为 `components/sys`。SNTP 的单次
  网络取样属于 Communication，候选接受、可信锚点与 RTC 回写仍属于 System。
- Device：外设能力、设备状态和设备级资源所有权。

同级组件默认互不依赖。确有稳定的提供者—使用者关系时可以直接依赖，但必须单向、无环且在
CMake 中明确声明。包含产品判断或跨多个同级组件的流程应上移到 Application 或 Service。

项目不再设置独立 `Network` 层或 `components/network/`。DeskMate 的联网时机、Dashboard、
本地页面状态、OTA 自动安装、连接会话退避、实时语音租约和轻睡眠停网属于 Application；
Communication 不得因拥有 `network_manager_task` 而吸收这些产品策略。

### 业务持久化边界

- 产生业务状态的组件拥有该状态的持久化结构、版本、兼容与迁移语义；System/Data 只提供
  与其职责相符的键值、Blob、文件、事务或缓存机制。
- 网络配置由网络能力所有者定义，页面与功能设置由对应产品能力所有者定义；System 不得为了
  统一入口而聚合其他组件的业务结构。
- 持久化键、命名空间和编码格式可以由业务组件声明，再通过稳定存储 API 保存；Application
  仍负责损坏、恢复默认值、降级或请求重启等产品策略。
- 当前 `components/sys/system_storage.*` 中保留的部分业务结构属于待迁移实现，不作为后续
  组件继续向 System 增加业务结构的依据。

### Device / BSP / Drivers / Boards 的硬件边界

硬件访问采用稳定的单向依赖：`Device → BSP → Drivers / Boards`。

- Device 对上提供电源、显示、环境、RTC、音频、按键和存储等与型号无关的设备能力，只依赖
  BSP 公共能力；不得包含具体芯片 Driver、Board 头文件、GPIO 或底层总线头文件。Device 只
  提供同步设备操作、状态快照和快速硬件事件入口，不得创建或管理 FreeRTOS Task。
- BSP 拥有板级总线、引脚、中断注册、Driver 实例和芯片到设备语义的转换。仅当前目标 BSP
  可以读取对应 Board 配置并装配 Driver。BSP 可以提供 ISR 或 SDK 回调入口，但不得为产品
  持续流程创建或管理 FreeRTOS Task。
- Drivers 只负责可复用的芯片协议和寄存器操作，通过参数、配置结构或 I/O 回调接收外部资源；
  不得包含 Board、BSP、Device、Service 或 Application 头文件。
- Boards 只声明当前硬件型号的引脚、总线实例、电压和静态硬件参数，不创建资源、不调用
  Driver，也不保存运行时状态。

DMA/TE 等必须紧贴硬件时序的短期传输状态机可以由 BSP 私有实现串行推进，但它不能拥有页面、
刷新策略或长期产品调度。若实现需要长期 Task，应将执行资源上移到 Service，BSP 只保留有界
传输和完成通知。

### 同一能力的多外设适配

- 同一业务能力的不同外设必须在 Device 层收敛为统一、与型号无关的公共 API。例如上层只
  使用“读取环境数据”或“设置音量”，不能感知具体传感器、Codec 或总线型号。
- Device Adapter 负责把统一能力映射到 BSP 提供的板级资源和操作；不得把具体型号类型泄漏
  到 Device 公共头文件。
- Driver 处理芯片协议和寄存器，BSP 负责板级资源与实例装配，Device Adapter 完成设备语义
  和统一单位的转换。
- 具体 Adapter 由目标构建或 Composition Root 装配。Application 和 Service 禁止包含具体
  外设型号头文件或通过型号分支选择业务流程；确需判断时只能依据 Device 暴露的能力信息。

### Utils

Utils 不是架构层，只允许存放无状态、无硬件所有权、无产品策略的通用算法。只在一个组件使用
的辅助函数应保留为该组件的 `static` 函数，不创建新的 `common`、`helpers` 或 `misc` 聚合
目录。

## 3. 依赖规则

- Main 可以依赖所有需要装配的公共接口。
- Application 可以依赖 Presentation、Service 和稳定的下层公共接口；顶层启动 Application
  可以按上述边界单向依赖其他 Application。
- 电源 Application 可以依赖 UI 的公共 Runtime 生命周期契约；其他 Application 不得直接
  调用 UI 页面、控件、路由或消息接口。
- Presentation 只依赖自身契约和下层组件发布的稳定事实 API；Application 通过 Presenter
  setter 或类型化呈现事件把产品状态交给 Presentation。Presentation 不得反向包含
  Application 或 UI 头文件。
- UI 只依赖 Presentation 契约和图形平台，不直接编排底层能力。
- Service 可以依赖完成其事务所需的 Communication、Data、System、Device。
- Device 只依赖 BSP 的稳定能力接口；BSP 再依赖 Drivers、共享总线 BSP 和 Boards。
- Application 和 Service 不得依赖 BSP、Drivers 或 Boards，也不得记录具体芯片型号作为
  产品语义。
- Driver 必须通过配置或 I/O 注入接收板级差异，禁止反向包含 Board 或 BSP。
- 设备事件需要持续等待、去抖、串行化或自动恢复时，由 Service 拥有对应 Task，并通过 Device
  的稳定 API 完成操作；禁止把该 Task 下沉到 Device 或 BSP。
- Service 可以在私有实现中直接依赖 FreeRTOS；Task、Queue、Timer 句柄必须保持私有。
- 底层不得反向包含 Application、Presentation、UI 或 Service 头文件。
- 事件类型由事件生产组件定义，不建立全局 `events.h`。
- 禁止包含其他组件的私有头文件、访问其内部结构体或 `extern` 可变全局变量。
- 公开头文件实际使用的依赖放入 `REQUIRES`；仅实现使用的依赖放入 `PRIV_REQUIRES`。
- 不得为了复用一个微小函数依赖整个高层组件；简单且不稳定的逻辑允许局部重复。

## 4. 组件与文件结构

简单叶子组件可以采用扁平结构：

```text
<component>/
├── CMakeLists.txt
├── include/
│   └── <component>.h
├── [src/]
│   ├── <component>.[c|cpp]
│   ├── <component>_<topic>.[c|cpp]
│   ├── <component>_<purpose>_task.[c|cpp]
│   └── <component>_internal.h
├── <component>.[c|cpp]
├── <component>_<topic>.[c|cpp]
├── <component>_<purpose>_task.[c|cpp]
├── <component>_internal.h
└── test/
    └── test_<component>.[c|cpp]
```

- `include/` 只放公共 API；内部头文件不得加入公共 include path。
- 主生命周期和公共 API 放在 `<component>.[c|cpp]` 或 `src/<component>.[c|cpp]`。
- 格式、协议等内聚实现可以拆到 `<component>_<topic>.[c|cpp]`。
- Presentation 适配器使用 `<feature>_presenter.[ch]`，View Model 类型使用
  `<feature>_view_model_t`；不得继续用模糊的 `app_<page>` 同时表示产品用例和 UI 数据。
- 源文件语言和跨语言头文件必须遵守 [C/C++ 语言边界规范](c_cpp_boundary.md)。
- 定义 FreeRTOS Task 的文件必须以 `_task.c` 或 `_task.cpp` 结尾。
- 同组件源码都使用组件名前缀，禁止孤立的 `parser`、`state`、`worker` 文件名。
- 复杂组件可以使用 `src/`；简单组件不为形式增加目录。现有组件不得只为目录偏好搬移文件。
- 拆分源文件不等于创建新 ESP-IDF 组件；只有具备独立公共能力和构建边界时才增加
  `CMakeLists.txt`。
- 未立项能力不得提前创建空目录、空接口或占位 Task。
