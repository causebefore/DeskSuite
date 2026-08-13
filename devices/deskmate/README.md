# DeskMate Device

> 本 README 只承担架构总览和文档索引。架构、数据流、API 与语言边界以
> `docs/architecture/` 下的规范为准；当前代码与规范存在偏移时，偏移属于待迁移事项，
> 不能反向改写规范。

## 架构总览

项目采用面向小型 ESP32 设备的轻量分层，不设置 Domain、能力优先目录或独立 Task 层：

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

Application 负责产品策略和完整用例编排，可以直接调用稳定的下层 API。Presentation 只把
Application 状态或下层不可变事实转换为 View Model 和呈现事件，不发起业务命令。UI 只消费
Presentation 契约、在唯一 LVGL 上下文渲染，并把用户意图交还 Application。Service 只在需要
封装持续执行资源、自动恢复、共享资源协调或复杂固定事务时使用，不是必经层。
电源 Application 仅通过 UI 的公共 Runtime 生命周期 API 完成整机启停握手，不访问页面和控件。

Task 是执行机制，不是架构层。产品调度 Task 位于 `main/application/`，UI Runtime Task 位于
`main/ui/`；Service 或 Communication 的 Task 同样留在对应所有者组件内部。

## 代码分区

| 分区 | 当前路径 | 核心职责 |
| --- | --- | --- |
| Main | [`main/main.c`](main/main.c)、[`main/app_main.c`](main/app_main.c) | 顶层启动顺序、依赖装配和关键失败分支 |
| Application | [`main/application/`](main/application/) | 产品用例、用户意图、产品调度和状态收敛 |
| Presentation | [`main/presentation/`](main/presentation/) | 事实到 View Model 的转换、页面契约和呈现事件 |
| UI | [`main/ui/`](main/ui/) | LVGL 页面、控件、UI Runtime Task 和视觉呈现 |
| Service | [`components/services/`](components/services/) | 可选的持续执行、自动恢复、完整事务与资源协调 |
| Shared Communication | [`../../shared/components/communication/`](../../shared/components/communication/) | 两套固件共用的 Wi‑Fi/Portal、网络诊断、传输、身份、后端上下文、SNTP、OTA 和日志能力 |
| Shared Service | [`../../shared/components/services/`](../../shared/components/services/) | 可跨产品显式装配的网页控制台，以及单向依赖 Communication 的叶子 Provider Adapter |
| Product Protocols | [`components/product_protocols/`](components/product_protocols/) | DeskMate Dashboard 产品协议 |
| Data | [`components/data/`](components/data/) | 产品数据结构、缓存和持久化语义 |
| System | [`components/sys/`](components/sys/) | 可信时间、存储基础、复位和看门狗等系统级能力 |
| Device | [`components/device/`](components/device/) | 与型号无关的设备能力和设备级资源所有权 |
| BSP / Drivers / Boards | [`components/bsp/`](components/bsp/)、[`components/drivers/`](components/drivers/)、[`components/boards/`](components/boards/) | 板级资源、芯片驱动和静态板型配置 |
| Graphics | [`components/graphics/`](components/graphics/) | 图形运行时和 UI 平台适配 |

## 关键流程

- 启动：`main/app_main()` 明确顶层顺序，`app_main_init()` 先装配本地数据、输入和 UI 固定
  资源；`app_main_start()` 优先启动 UI 并派发首屏，再初始化音频/AFE、启动其余运行期能力，
  最后开放按键，确保输入只进入已经就绪的页面依赖。
- 输入：`button_service` 产生不可变按键事实，Application 决定页面、语音或 OTA 等产品动作。
- 环境：Application 调度环境采样，`environment_service` 通过稳定 Device API 完成采样事务。
- 网络：`app_network` 拥有 DeskMate 的 Dashboard、OTA、语音租约和会话退避策略；
  Communication 的 `network_manager` 只拥有 Wi‑Fi/Portal 技术状态机，协议与传输不决定产品时机。
- 网页控制台：设备设置页选择“网页控制台”后，Application 申请专用网络租约并启动本地
  认证管理 Service；浏览器顶层只有“文件管理 / 设置 / 退出登录”，“设置”首页按客户分组呈现
  Hub Settings/连接测试 Actions、番茄钟 Settings 和“设备与系统” Status。产品不装配调试型
  Network Manager Status，不提供 Wi-Fi 分类、OTA 或重启动作。Hub 测试与保存由 `app_network`
  串行执行，番茄钟完成音乐通过 Files 支持的 `.mp3` 路径选择字段配置；页面返回必须等待
  Service 完整停止后才释放租约。设置首页只显示三个分组的静态说明，不读取领域数据；进入
  二级详情后才按需读取该分区的权威快照。通用字符串按有效 UTF-8 传输，Hub 地址另由 Network
  Application 收紧为 ASCII `http://` authority；Settings/Actions 的已知稳定失败作为确定终态
  呈现。详情把生效方式、单位和重启原因映射为客户可读中文，不暴露 `idle_only`、`power_on`
  等协议 token。
- 呈现：Service、Communication 或 Application 报告事实，Presenter 更新 View Model 并发布呈现事件，
  UI Runtime 在唯一 LVGL 上下文读取并渲染。
- 番茄钟：`app_pomodoro_task` 使用单调 deadline 串行推进专注、短休和长休；设置以独立版本
  仲裁本机与浏览器并发更新，时长、SD 卡完成音乐逻辑路径与本地完成数由 `pomodoro_store`
  保存；阶段完成后由 Audio Service 异步播放所选 MP3，设备启动时在音频与语音运行时就绪后
  复用同一路径，以 50% 默认输出音量播放一次开机提示音；系统 UTC 只负责本地日期归一化和
  预计结束时间。
- 低功耗：`app_power` 在 30 秒无按键活动且产品事务空闲时先选择模式。运行中的番茄钟页进入
  `OFFLINE_DISPLAY`，只停止 Network Manager 和 Wi-Fi Driver，保留 UI 与一秒刷新；其他场景
  可逆停止 UI Runtime，再通过 `device_power`/BSP 进入 Light-sleep。普通模式使用左右键 EXT1
  与 ESP32 内部 Timer；默认关闭的 `DESKMATE_RTC_INT_WAKE_TEST_ENABLED` 测试模式改用左右键
  与 GPIO15 RTC INT EXT1，并完全禁用内部 Timer。BSP 在每次睡眠事务开始时关闭全部 RTC INT 输出源、
  清除 AF/TF，保持 GPIO15 内部上拉并等待 10 ms 后读取释放基线；基线为低时不启动 Timer
  或 Light-sleep，基线为高才以 1 Hz 时钟装载 PCF85063 Timer。任一来源唤醒或睡眠入口失败后
  都会停止 Timer 并清除 TF。测试模式在 Light-sleep 期间保持 `RTC_PERIPH` 供电；若 IDF 仍
  因唤醒源预先有效而拒绝睡眠，BSP 会在返回路径再次采样 GPIO15 与 RTC 中断寄存器。左右键唤醒恢复正常交互；
  RTC Timer 唤醒后先同步补算番茄钟，再恢复 UI，阶段完成会重新开启正常清醒窗口。

## 规范文档

修改架构、数据流、公共 API、Task 或错误恢复前，按顺序阅读：

1. [架构规范入口与未决边界](docs/architecture/README.md)
2. [项目分层与组件依赖](docs/architecture/layering.md)
3. [数据流、并发与 Task](docs/architecture/data_flow.md)
4. [API 与所有权规范](docs/architecture/api_conventions.md)
5. [C/C++ 语言边界规范](docs/architecture/c_cpp_boundary.md)
6. [Application 与 Service 组件 README 规范](docs/architecture/component_readmes.md)
7. [Service 层补充规范](components/services/README.md)
8. [时间校准流程](docs/时间校准流程.md)
9. [低功耗与按键/RTC 唤醒流程](docs/低功耗流程.md)

仓库工作、构建、注释和提交要求见 [AGENTS.md](AGENTS.md)；Claude Code 使用同一规则，
入口为 [CLAUDE.md](CLAUDE.md)。
