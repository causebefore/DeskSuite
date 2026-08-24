
# PhotoPainter Device

> 本 README 只承担架构总览和文档索引。架构、数据流、API 与可靠性规则以 `docs/architecture/` 下的规范为准；当前代码与规范存在偏移时，偏移属于待迁移事项，不能反向改写规范。

## 架构总览

项目采用面向小型 ESP32 设备的轻量分层，不设置 Domain、能力优先目录或独立 Task 层：

```text
Main / Composition Root
        ↓
Application
        ├───────────────┐
        ↓               │
Service（可选执行/事务层）│
        └───────────────┤
                        ↓
Communication | Storage | System | Device
                        ↓
                       BSP
                  ┌─────┴─────┐
                  ↓           ↓
               Drivers      Boards
```

Application 负责产品策略和完整用例编排，可以直接调用稳定的底层 API。Service 只在需要封装跨组件事务、持续执行资源、自动恢复或复杂固定流程时使用，不是必经层。Device/BSP 保持同步、短时且可组合，硬件语义状态机与持续执行机制分离。具体刷新时间、网络事务顺序和显示收敛策略属于产品规范，不作为通用架构规则。

## 连接状态与配网

| 状态 | 显示与操作 | 休眠 |
| --- | --- | --- |
| 无 Wi-Fi | `NO NETWORK` / `HOLD MIDDLE 3S TO SETUP`；物理中键长按 3 秒松开后显示现有二维码 | 冷启动/按键唤醒等待 180 秒；定时唤醒刷新后立即退避深睡 |
| Wi-Fi 正常、Hub 不可用或未配置 | `NO SERVER` / 同一中键长按提示 | 同上 |
| Wi-Fi 与 Hub 正常 | 原照片页；中键长按 3 秒也可主动配网 | 沿用内容刷新与低功耗计划 |
| Portal | 现有二维码，仅由网页完成配置 | 180 秒无真实网页活动后保留二维码深睡 |

深睡后的第一次按键只负责唤醒；松开后需要再次长按物理中键（逻辑 `DEVICE_BUTTON_RIGHT`、
GPIO4）才进入 Portal。配网意图跨重启和 Portal 超时保留，候选 Wi-Fi 与 Hub 配置有效后清除。
OTA 模态与安装事务会消费配网长按。



## 设备拉取式 OTA

固件只保留一种构建产物，不再保存 `NORMAL` / `OTA` 运行模式。正常内容刷新不会自动检查
固件；旧设备遗留的 `ota_dev_mode` NVS 键会被忽略，不产生迁移写入。

左键持续至少 3 秒并松开后提交一次性固件检查请求。固件检查、用户确认和安装由电源管理
Application 协调，共享 `firmware_ota` 的独立 Task 只拥有固件目标
校验、下载、镜像校验和启动分区切换事务。检查与安装通过异步命令提交，最终结果以不可变完成
事件回传给电源管理 Task；它不启停 Wi-Fi，不处理按键、深睡、SD 卡或外部 Flash。服务端不
主动推送，设备也不开放固件上传接口。

请求若遇到内容或显示事务正在执行，会先进入 `CHECK_PENDING`；收敛后停止空闲内容刷新 Task，
最多等待 30 秒 Wi-Fi 上线，再执行最多 10 秒的 OTA HTTP 查询。有效无更新显示
`FIRMWARE IS UP TO DATE`，任一连接、TLS、HTTP 或解析异常显示 `SERVER UNAVAILABLE`；两者均
先关闭网络。发现更新时保持 Wi-Fi 并显示 `UPDATE AVAILABLE`，确认键单击开始安装，左键单击
取消。提示页 180 秒无操作会恢复照片并按原刷新计划直接深睡；手动退出则重新开始正常 180 秒
操作窗口。下载失败显示 `UPDATE FAILED`，并要求重新检查后才能重试。

固件交互期间普通休眠被暂停，写入开始后全部按键和休眠通知均被忽略。所有固件状态页显示前
写入一次性 NVS 画面恢复标记；正常照片页面物理刷新成功后才清除，
失败会保留到后续刷新或下次启动重试。启动分区切换后立即重启。
统一构建脚本会把单调递增的 `ota_version` 嵌入固件，OTA 只接受严格更高的目标版本；旧版本、
同版本和旧清单均不会覆盖线刷的新固件。该限制只作用于 OTA，人工线刷仍可降级。

在 DeskSuite 根目录执行普通编译：

```powershell
& .\ds.ps1 build photopainter
```

首次仍需通过串口烧录。此后执行以下命令，会先编译，再按 DeskSuite 根目录
`products.toml` 的 `[ota_publish]` 配置，通过 SSH 与 Docker 将固件和目标清单原子
发布到 Ubuntu 生产 Hub：

```powershell
& .\ds.ps1 ota photopainter
```

只有需要发布到本地 Hub 目录时才显式指定 `-ServiceRoot`：

```powershell
& .\ds.ps1 ota photopainter -ServiceRoot .\services\hub
```

发布将
`photopainter_esp32s3_v1` 清单与其他目标隔离，固件进入全局哈希制品库。设备请求统一
`/api/v1/ota/check`，同时上报 `product_id=1` 和 `firmware_target`。发布以 ESP 镜像
Validation SHA-256 作为 `artifact_id`，以完整文件 SHA-256 校验下载内容。设备切换启动分区
后必须立即重启；新镜像在本地关键通信能力与 OTA Task 启动成功后确认有效，失败则由
ESP-IDF A/B 回滚。

清单也可以提供公开 GitHub Release 的 HTTPS 绝对地址。此时 Hub 仍判断是否需要更新，但设备
直接从 [DeskSuite 固件 Release 仓库](https://github.com/causebefore/desksuite-firmware) 下载，
不会把 Hub Token 或设备身份发送到 GitHub；未提供外部地址的旧清单继续使用 Hub 下载接口。

## 代码分区

| 分区 | 当前路径 | 核心职责 |
| --- | --- | --- |
| Main | [`main/`](main/) | 在 `app_main()` 中保留顶层启动顺序和关键失败分支 |
| Application | [`components/application/`](components/application/) | 产品策略、调度和降级决策；`bootstart_app` 承载各启动阶段的具体实现 |
| Service | [`components/services/`](components/services/) | 可选的持续执行、自动恢复、完整事务与资源协调 |
| Shared Communication | [`../../shared/components/communication/`](../../shared/components/communication/) | 两套固件共用的链路、网络诊断、传输、身份、后端上下文、SNTP、日志和 OTA 实现 |
| Product Protocols | [`components/product_protocols/`](components/product_protocols/) | PhotoPainter 显示帧、集合和设备状态契约 |
| Storage（目标层） | 尚未独立；当前兼容实现位于 [`components/sys/`](components/sys/) | 目标为 NVS、文件、分区等通用持久化机制；现有 `system_storage` 仍属待迁移实现 |
| System | [`components/sys/`](components/sys/) | 可信时间、复位和看门狗等系统级能力 |
| Device | [`components/device/`](components/device/) | 外设能力和设备级资源所有权 |
| Drivers / BSP / Boards | [`components/drivers/`](components/drivers/)、[`components/bsp/`](components/bsp/)、[`components/boards/`](components/boards/) | 芯片驱动、板级资源和板型配置 |
| Shared Utils | [`../../shared/components/utils/`](../../shared/components/utils/) | 两套设备共用的无状态、无产品策略通用算法；不是架构层 |

## 规范文档

修改架构、数据流、公共 API、Task 或错误恢复前，按顺序阅读：

1. [架构规范入口与未决边界](docs/architecture/README.md)
2. [项目分层与组件依赖](docs/architecture/layering.md)
3. [数据流、并发与 Task](docs/architecture/data_flow.md)
4. [API 与所有权规范](docs/architecture/api_conventions.md)
5. [C/C++ 语言边界规范](docs/architecture/c_cpp_boundary.md)
6. [Application 与 Service 组件 README 规范](docs/architecture/component_readmes.md)
7. [Service 层补充规范](components/services/README.md)

仓库工作、构建、注释和提交要求见 [AGENTS.md](AGENTS.md)。
