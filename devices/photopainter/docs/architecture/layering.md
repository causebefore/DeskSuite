# 项目分层与组件依赖

## 1. 总体结构

项目采用以下目标分层：

```text
Main / Composition Root
        ↓
Application
        ├───────────────┐
        ↓               │
Service（可选事务层）   │
        └───────────────┤
                        ↓
Communication | Storage | System | Device
                        ↓
                       BSP
                  ┌─────┴─────┐
                  ↓           ↓
               Drivers      Boards
```

层级表示允许的编译依赖，不表示一次调用必须机械地穿过每一层。整体依赖必须构成无环有向图。

## 2. 层级职责

### Main / Composition Root

Main 必须只负责保留顶层启动顺序、关键失败分支，以及创建或取得依赖、启动顶层组件。具体启动
阶段可以委托给单一的启动 Application 实现，以避免入口文件堆积设备初始化、回调适配和局部
回滚细节。Main 不得包含一小时刷新、配网选择、降级或重试等运行期产品决策，也不得成为永久
业务循环。

### Application

Application 负责完整用例的触发时机、调用顺序和产品策略，包括可信时间恢复、调度窗口、
有界网络会话、配网超时以及必需/可选能力的故障分类。具体时间和阈值属于产品规范。

Application 可以直接调用 Communication、Storage、System 和 Device 的稳定公共 API。不能为了追求形式完整而强制增加 Service 包装。

顶层启动 Application 可以为完成启动阶段用例而单向调用其他 Application 的稳定公共 API；
顶层先后顺序和关键失败分支仍由 Main 明确表达，普通功能 Application 不得借此形成横向循环依赖。

### Service（可选）

只有满足以下至少一种条件时才应使用 Service：

- 一个完整事务跨越多个下层组件，并要求固定顺序、提交或补偿。
- 需要由一个组件独占共享资源或事务状态。
- 同一复杂流程被多个 Application 用例复用。
- Device/BSP 的同步能力需要持续等待、周期调度、去抖执行、超时重试或自动恢复，并需要独立
  拥有 Task、Queue、Timer 或等价执行资源。

只转发一个下层调用、只转换函数名或没有独立事务语义的 Service 属于过度封装。
Service 只依赖稳定的 Device API，不得感知 GPIO、总线或芯片型号。Service 私有实现可以
直接使用 FreeRTOS Task、Queue、Timer 等执行机制，但不得通过公共 API 泄漏 RTOS 句柄。

### Communication / Storage / System / Device

这些组件提供稳定的技术或设备能力，不负责产品级时机和致命/降级决策：

- Communication：链路、传输与协议事实。
- Storage：NVS、文件、分区和可移动介质等通用持久化机制，不定义业务数据结构。
- System：系统时间、复位、身份、看门狗等系统级能力；当前实际目录名为 `components/sys`。
- Device：外设能力、设备状态和设备资源所有权。

同级组件默认互不依赖。确有稳定的提供者—使用者关系时可以直接依赖，但必须单向、无环且在 CMake 中明确声明。包含产品判断或跨多个同级组件的流程应上移到 Application 或 Service。

### 业务持久化边界

- 产生业务状态的组件拥有该状态的持久化结构、版本、兼容与迁移语义；Storage 只提供键值、
  Blob、文件、事务或分区等保存机制。
- 网络配置由网络能力所有者定义，显示 active/pending 等提交状态由显示能力所有者定义；
  Storage/System 不得为了统一入口而聚合其他组件的业务结构。
- 持久化键、命名空间和编码格式可以由业务组件声明，再通过 Storage 的稳定 API 保存；
  Application 仍负责损坏、恢复默认值、降级或请求重启等产品策略。
- `network_manager` 已通过配置持久化回调解除对 System 类型的编译依赖；当前
  `components/sys/system_storage.*` 中保留的网络配置编码仍属于待迁移实现，不作为后续组件继续
  向 System 增加业务结构的依据。显示集合状态已经迁移到 `display_collection_service` 的 SD
  A/B 状态槽，由显示能力自行拥有版本、校验和恢复语义。

### Device / BSP / Drivers / Boards 的硬件边界

硬件访问采用稳定的单向依赖：`Device → BSP → Drivers / Boards`。

- Device 对上提供“电源、显示、环境数据、RTC、音频、存储”等设备能力，只依赖 BSP
  公共能力；不得包含具体芯片 Driver、Board 头文件、GPIO 或底层总线头文件。Device 只提供
  同步设备操作、状态快照和快速硬件事件入口，不得创建或管理 FreeRTOS Task。
- BSP 拥有板级总线、引脚、中断注册、Driver 实例和芯片到设备语义的转换。仅由当前硬件
  目标选择的 BSP 可以读取对应 Board 配置并装配 Driver。BSP 可以提供 ISR 或 SDK 回调入口，
  但不得为产品持续流程创建或管理 FreeRTOS Task。
- Drivers 只负责可复用的芯片协议和寄存器操作，通过参数、配置结构或 I/O 回调接收外部
  资源；不得包含 Board、BSP、Device、Service 或 Application 头文件。
- Boards 只声明当前硬件型号的引脚、总线实例、电压和静态硬件参数，不创建资源、不调用
  Driver，也不保存运行时状态。

### 同一能力的多外设适配

- 同一业务能力的不同外设必须在 Device 层收敛为统一、与型号无关的公共 API。例如上层只
  使用“读取环境数据”或“提交显示刷新”，不能感知具体传感器、屏幕控制器或总线型号。
- Device Adapter 是 Device 内部的具体实现适配器，负责把统一能力映射到 BSP 提供的板级
  资源和操作；Adapter 不得把具体型号类型泄漏到 Device 公共头文件。
- 型号、总线、寄存器和原始数据格式差异由 Device Adapter、BSP 或 Driver 按各自职责消化：
  Driver 处理芯片协议和寄存器，BSP 负责板级资源与实例装配，Device Adapter 完成设备语义
  和统一单位的转换。
- 具体 Adapter 由目标构建或 Composition Root 装配。Application 和 Service 禁止包含具体
  外设型号头文件、记录型号枚举，或通过型号分支选择业务流程；确需判断时只能依据 Device
  暴露的能力信息。



### Utils

Utils 不是架构层，只允许存放无状态、无硬件所有权、无产品策略的通用算法。只在一个组件使用的辅助函数应保留为该组件的 `static` 函数，不创建新的 `common`、`helpers` 或 `misc` 聚合目录。

## 3. 依赖规则

- Main 可以依赖所有需要装配的公共接口。
- Application 可以依赖 Service 和稳定的下层公共接口；顶层启动 Application 可以按上述边界
  单向依赖其他 Application。
- Service 可以依赖完成其事务所需的 Communication、Storage、System、Device。
- Device 只依赖 BSP 的稳定能力接口；BSP 再依赖 Drivers、共享总线 BSP 和 Boards。
- Application 和 Service 不得依赖 BSP、Drivers 或 Boards，也不得记录具体芯片型号作为
  产品语义。
- 当前根目录扁平工程必须在构建配置中只选择一个 Board，以及与之匹配的目标 BSP；禁止在
  共享源码中通过硬件型号宏混编两套 Board/BSP。未来增加多硬件目标时，应由不同构建配置或
  独立工程入口完成选择，不能把目标判断下沉到 Application/Service。
- Driver 必须通过配置或 I/O 注入接收板级差异，禁止反向包含 Board 或 BSP。
- 设备事件需要持续等待、去抖、串行化或自动恢复时，由 Service 拥有对应 Task，并通过 Device
  的稳定 API 完成操作；禁止把该 Task 下沉到 Device 或 BSP。
- Service 可以在私有实现中直接依赖 FreeRTOS；Task、Queue、Timer 句柄必须保持私有。
- 底层不得反向包含 Application 或 Service 头文件。
- 事件类型由事件生产组件定义，不建立全局 `events.h`。
- 禁止包含其他组件的私有头文件、访问其内部结构体或 `extern` 可变全局变量。
- 公开头文件实际使用的依赖放入 `REQUIRES`；仅 `.c`/`.cpp` 实现使用的依赖放入
  `PRIV_REQUIRES`。
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
├── <component>.[c|cpp]                   # 扁平结构时
├── <component>_<topic>.[c|cpp]           # 扁平结构时
├── <component>_<purpose>_task.[c|cpp]    # 扁平结构时
├── <component>_internal.h                # 扁平结构时
└── test/
    └── test_<component>.[c|cpp]
```

- `include/` 只放公共 API。
- 内部头文件不得加入公共 include path。
- 主生命周期和公共 API 放在 `<component>.[c|cpp]` 或 `src/<component>.[c|cpp]`。
- 格式、协议等内聚实现可以拆到 `<component>_<topic>.[c|cpp]`。
- 源文件语言和跨语言头文件必须遵守 [C/C++ 语言边界规范](c_cpp_boundary.md)。
- 定义 FreeRTOS Task 的 C 文件必须以 `_task.c` 结尾，C++ 文件必须以 `_task.cpp` 结尾；详细规则见 [数据流、并发与 Task](data_flow.md)。
- 同组件源码都使用组件名前缀，禁止孤立的 `parser.[c|cpp]`、`state.[c|cpp]`、
  `worker.[c|cpp]`。
- 具有多个实现文件、私有头文件或明显内部子模块的复杂组件可以使用 `src/`；简单组件不为
  形式增加目录。现有组件不得只为满足目录偏好在扁平结构与 `src/` 结构之间搬移文件。
- 使用 `src/` 时应通过 `PRIV_INCLUDE_DIRS` 暴露私有头文件，并在同一组件内保持一致布局。
- 拆分源文件不等于创建新 ESP-IDF 组件；只有具备独立公共能力和构建边界时才增加 `CMakeLists.txt`。
- 未来低功耗、音频、公网或协议功能在立项前不创建空目录、空接口或占位 Task。
