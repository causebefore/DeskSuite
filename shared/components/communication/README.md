# Shared Communication

> PhotoPainter 与 DeskMate 共用的唯一 Communication 物理源码目录。

## 1. 边界

本目录只拥有可跨产品复用的通信机制和协议事实：

```text
Application / Product Protocol
        ├── network_manager ──→ connect
        ├── protocols ────────→ transport
        ├── time_sync ─────────→ ESP-NETIF SNTP
        ├── firmware_ota ─────→ protocols + transport
        └── remote_log ───────→ protocols + transport
```

- `connect`：Wi-Fi Driver、STA、SoftAP、Portal、DNS、扫描和完整链路快照。
- `network_manager`：Wi-Fi/Portal 技术状态机、候选配置验证和单轮技术重试。
- `transport`：同步 HTTP、流式下载/上传和 WebSocket 传输。
- `protocols`：稳定硬件设备 ID、统一后端上下文、URL、语音与日志上传等通用协议。
- `time_sync`：串行拥有 ESP-NETIF SNTP 客户端并提供单次 UTC 网络样本。
- `tools/firmware_ota`：固件检查、目标缓存、下载校验和 OTA 分区事务。
- `tools/remote_log`：Log V2 批次采集、会话和远端上传。

本目录不决定联网时机、产品重试策略、页面状态、显示内容、按键行为、深睡或 OTA 自动安装。
这些决策由各设备 Application 拥有。

## 2. 产品协议隔离

以下契约不进入共享目录：

- PhotoPainter 的显示集合、PPF 帧、显示状态和设备状态上传：
  `devices/photopainter/components/product_protocols/photopainter_protocol/`
- DeskMate 的 Dashboard schema：
  `devices/deskmate/components/product_protocols/deskmate_protocol/`

产品协议可以依赖共享 `protocols` 和 `transport`，共享组件不得反向依赖产品协议。

## 3. Network Manager 契约

`network_manager` 通过 `network_manager_config_store_t` 注入配置加载、保存和清除回调。候选配置
只有在 STA 获得 IPv4 后才提交；验证失败时保持 Portal 和原有 active 配置。

`network_manager_get_portal_info_copy()` 在 Portal 尚未激活时仍返回 `ESP_OK`，同时输出
`active=false` 的零值快照。这是瞬时等待条件，不是错误；调用方应等待后续变化通知再读。

`network_manager_stop()` 同步回收 Portal、DNS、扫描、回调和 Wi-Fi Driver。失败或超时时，上层
不得假定资源已经释放，也不得绕过 Manager 直接重建设备网络。

`network_manager_get_diagnostics_copy()` 统一返回 Manager 状态、当前会话的连接/成功/断链/
Portal 计数，以及实时 AP、BSSID、信道、认证、RSSI、IPv4、掩码、网关和 DNS 快照。底层
链路查询错误单独放在 `link_snapshot_error`，不会掩盖其余 Manager 事实。

## 4. 后端上下文与设备身份

`protocol_backend_context_t` 是按值复制的完整后端事实，统一包含 `base_url`、可选 Token、
稳定 `device_id`、`product_id` 和 `firmware_target`。构造时未显式提供设备 ID，则
`protocol_identity_get_hardware_device_id_copy()` 基于 Wi-Fi Station 基础 MAC 生成
`esp32-xxxxxxxxxxxx`。上下文不读取 Storage，也不拥有重试或生命周期状态。

OTA、远端日志、共享语音以及两端产品协议都从该上下文读取 URL、鉴权与身份，禁止各自生成
设备 ID 或维护字段副本组合。

## 5. SNTP 网络取样

`time_sync_sample_sntp_once_copy()` 只完成一次 SNTP 客户端初始化、样本等待、UTC 秒与耗时复制
和清理。是否接受候选、是否取第二个样本、可信锚点、RTC 回写、周期与重试仍由各设备
`system_clock` 和 Application 决定。

## 6. OTA v2

两套设备统一调用：

```text
POST /api/v1/ota/check
GET  /api/v1/ota/artifacts/<artifact_id>
```

检查请求包含 `protocol_version=2`、`product_id`、`firmware_target`、`device_id` 和当前制品
状态。Hub 先按 `firmware_target` 选择清单，再校验清单中的 `product_id` 和
`firmware_target`，因此路由不再承担产品隔离。

`firmware_ota` 只在调用方已经建立的在线会话中工作；它不启动或等待 Wi-Fi。检查成功后缓存
不可变目标，安装开始后不可取消，完整文件摘要和 ESP 镜像均校验通过后才切换启动分区。

## 7. 产品构建差异

共享源码中的诊断差异通过 Kconfig 选择，不复制文件：

| 配置 | 默认 | 当前产品选择 |
| --- | --- | --- |
| `CONFIG_COMMUNICATION_TASK_STACK_STATS` | 关闭 | DeskMate、PhotoPainter 均关闭；仅在临时诊断构建中显式启用 |
| `CONFIG_COMMUNICATION_HTTP_TIMING_LOGS` | 关闭 | PhotoPainter 开启，DeskMate 关闭 |
| `CONFIG_CONNECT_PORTAL_*` | 通用值 | 两个产品分别覆盖 SSID、密码和默认服务地址 |

诊断开关只改变日志，不改变协议、状态机或生命周期。固件目标和产品 ID 不由 Kconfig 注入；
根构建工具从 `products.toml` 生成构建头，再由产品装配层构造统一后端上下文。

## 8. 语言与所有权

公共头文件保持 C ABI。C++ 仅用于内部资源所有权和实现，不向 C Application/Service 暴露
STL、异常或 C++ 类型。所有借用参数只在调用期间有效；需要跨 Task 保存的值必须复制。

`transport` 不依赖 `connect` 或 `network_manager`，调用前确认网络在线始终是上层责任。
Communication 的通知回调只报告事实并快速返回，产品状态收敛必须回到各自 Application Task。
