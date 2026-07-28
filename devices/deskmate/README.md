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
| Product Protocols | [`components/product_protocols/`](components/product_protocols/) | DeskMate Dashboard 产品协议 |
| Data | [`components/data/`](components/data/) | 产品数据结构、缓存和持久化语义 |
| System | [`components/sys/`](components/sys/) | 可信时间、存储基础、复位和看门狗等系统级能力 |
| Device | [`components/device/`](components/device/) | 与型号无关的设备能力和设备级资源所有权 |
| BSP / Drivers / Boards | [`components/bsp/`](components/bsp/)、[`components/drivers/`](components/drivers/)、[`components/boards/`](components/boards/) | 板级资源、芯片驱动和静态板型配置 |
| Graphics | [`components/graphics/`](components/graphics/) | 图形运行时和 UI 平台适配 |

## 关键流程

- 启动：`main/app_main()` 明确顶层顺序，`app_main_init()` 装配产品能力，
  `app_main_start()` 启动长期运行组件。
- 输入：`button_service` 产生不可变按键事实，Application 决定页面、语音或 OTA 等产品动作。
- 环境：Application 调度环境采样，`environment_service` 通过稳定 Device API 完成采样事务。
- 网络：`app_network` 拥有 DeskMate 的 Dashboard、OTA、语音租约和会话退避策略；
  Communication 的 `network_manager` 只拥有 Wi‑Fi/Portal 技术状态机，协议与传输不决定产品时机。
- 网页文件管理：设备设置页选择“网页文件管理”后，Application 申请专用网络租约并启动本地
  认证文件 Service；页面返回必须等待 Service 完整停止后才释放租约。
- 呈现：Service、Communication 或 Application 报告事实，Presenter 更新 View Model 并发布呈现事件，
  UI Runtime 在唯一 LVGL 上下文读取并渲染。
- 番茄钟：`app_pomodoro_task` 使用单调 deadline 串行推进专注、短休和长休；设置与本地完成数
  由 `pomodoro_store` 保存，系统 UTC 只负责本地日期归一化和预计结束时间。
- 低功耗：`app_power` 在 60 秒无按键活动且产品事务空闲时先选择模式。运行中的番茄钟页进入
  `OFFLINE_DISPLAY`，只停止 Network Manager 和 Wi-Fi Driver，保留 UI 与一秒刷新；其他场景
  可逆停止 UI Runtime，再通过 `device_power`/BSP 配置左右键 EXT1 与 ESP32 内部 Timer 并进入
  Light-sleep。左右键唤醒恢复正常交互；内部 Timer 取屏幕维护、Dashboard 截止和番茄钟阶段
  截止中的最近值。唤醒后先同步补算番茄钟，再恢复 UI，阶段完成会重新开启正常清醒窗口。

网页文件管理的完整流程为：

```text
设备设置页选择“网页文件管理”
  → app_web_file 检查 /sdcard
  → app_network 授予 APP_NETWORK_LEASE_WEB_FILE
  → web_file_service 恢复事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → handler 串行浏览、下载、事务上传或执行单项文件变更
  → 设备返回时 Service 安全停止后释放网络租约
```

运行期间 `app_network` 的链路变化借用回调只通知 `app_web_file_task` 重新读取最新 STA
事实；断线时设备页暂时清空 URL，重连或 IPv4 变化时只更新 URL，不重启 Service，也不更换
访问码。Application 把已收敛的完整快照和严格单调展示版本推给 Presenter，UI 只消费
View Model；离开子页或关闭设置菜单时，只有 Application 明确报告资源已经安全释放才允许退出。

当前开放 `/sdcard` 的浏览、下载、原始 `PUT` 上传、创建目录、常规文件重命名/移动，以及
常规文件和空目录删除。浏览器多选操作按顺序调用单项接口，不提供跨文件原子事务；仍不包含
配置编辑、递归目录删除、目录移动/重命名、WebDAV 或 WebSocket。

## 网页文件管理配置与验收状态

- 受版本控制的 `sdkconfig.defaults` 和 `sdkconfig.ci` 已显式启用 FatFs UTF-8、堆分配长文件名
  并把 FatFs 长文件名上限设为 255 个 UTF-16 代码单元，禁用 HTTPD WebSocket，同时把 URI
  上限设为 2048 字节；Service 运行期配置八个精确 URI handler。
- 根 `sdkconfig` 是 `.gitignore` 排除的本地生成文件，本次未复制、修改或提交。用户自行编译前
  应通过仓库统一配置/构建流程重新生成，或确认本地值已与上述受控源同步：UTF-8 已启用、
  ANSI/OEM 与 WebSocket 已关闭、URI 上限为 2048。需要编译时只在 DeskSuite 根目录使用
  `& .\ds.ps1 build deskmate`，不得绕过统一脚本直接调用下层构建工具。
- 当前功能按用户要求只做代码和静态核查，不运行自动化测试、固件编译或实机验收。500 MiB
  上传、上传取消/重试、中文及特殊文件名、创建/移动/删除、会话互斥、覆盖恢复、断网/掉电和
  安全停止等硬件检查仍由用户在目标设备上执行。

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
