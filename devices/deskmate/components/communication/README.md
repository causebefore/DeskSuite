# Communication

> Communication 提供可复用的联网、传输、协议和通信工具；DeskMate 的联网时机、Dashboard、
> OTA 自动安装、语音租约和轻睡眠策略由 Application 决定。

## 1. 定位

本目录不再对应一个独立的 DeskMate `Network` 产品层。通信相关代码按两条单向链组织：

```text
app_network
    └── network_manager
            └── connect

app_network / voice_service
    └── protocols
            └── transport

app_network
    └── tools/firmware_ota
            ├── protocols
            └── transport
```

`network_manager` 报告 Wi‑Fi 与 Portal 技术事实；`app_network` 解释这些事实并决定新会话、
Dashboard 同步、OTA、语音租约和整机停网。`transport` 不依赖 `connect` 或
`network_manager`，调用前确保网络在线始终是上层责任。

Communication 采用 C/C++ 混合实现，公共头文件保持 C ABI。C++ 只用于内部资源所有权和实现，
不得把 STL、异常或 C++ 类型泄漏给 C Application、Service。

## 2. 当前构建选择

DeskMate 当前编译以下子组件：

| 子组件 | 当前职责 |
| --- | --- |
| `connect` | Wi‑Fi Driver、STA、SoftAP、Portal、DNS 劫持、扫描和链路快照 |
| `network_manager` | Wi‑Fi/Portal 会话状态机、候选配置验证和一轮技术性重试 |
| `transport` | 同步 HTTP、流式 HTTP 下载/上传和 WebSocket 传输 |
| `protocols` | 统一设备身份、Dashboard、语音和日志上传协议 |
| `tools/firmware_ota` | 独立 Task 执行固件检查、不可变目标缓存、下载、校验和分区切换 |

从 PhotoPainter 同步过来的显示帧、显示状态和设备状态协议源码继续保留，便于两个设备端对齐
Communication 基线，但不加入 DeskMate 的 `protocols/CMakeLists.txt`，因此不进入当前固件。
`tools/remote_log` 同样保留源码；DeskMate 当前使用 ESP-IDF Log V1，尚未把该 Tool 加入
`EXTRA_COMPONENT_DIRS`。只有启用 Log V2 文本模式并完成产品生命周期接入后才能编译它。

## 3. 子组件职责

### 3.1 `connect`

`connect` 只拥有底层 Wi‑Fi 与 Portal 资源：

- 初始化和反初始化 ESP Wi‑Fi、Netif 与事件入口。
- 启动 STA、保持 Portal 时验证候选 STA、查询关联/IP/RSSI 快照。
- 启动和停止 SoftAP、HTTP Portal、DNS Task、二维码与 Wi‑Fi 扫描 Task。
- 把链路事件、Portal 提交和用户活动通过快速回调报告给唯一上层所有者。

它不读取 DeskMate 设置、不选择何时配网、不决定重试次数，也不调用 Dashboard、OTA 或 UI。

### 3.2 `network_manager`

`network_manager` 是通用 Wi‑Fi/配网状态机，状态包括：

```text
STOPPED
    → CONNECTING → ONLINE
          ↓           ↓
      RETRY_WAIT ─────┘
          ↓
        ERROR

PROVISIONING → VALIDATING → ONLINE
      ↑             │
      └─────────────┘
```

它通过 `network_manager_config_store_t` 注入配置加载、保存和清除回调。回调在
`network_manager_task` 中同步执行，只能访问有界持久化存储，不得重入 Network Manager。
候选配置只有在 STA 获得 IPv4 后才提交；验证失败时保持 Portal 并返回可编辑状态。

一轮内部连接重试结束后只发布 `NETWORK_STATE_ERROR`，后续是否建立新会话由 Application
决定。`network_manager_stop()` 同步回收 Portal、DNS、扫描、事件回调和 Wi‑Fi Driver；
超时或清理失败时禁止上层假定资源已经释放。

主要公共接口：

| 接口 | 语义 |
| --- | --- |
| `network_manager_init_borrow()` | 注入持久化回调并初始化资源 |
| `network_manager_start()` / `network_manager_stop()` | 启停一轮完整 Wi‑Fi/Portal 会话 |
| `network_manager_get_status_copy()` | 复制状态、错误码和 Portal 活动序号 |
| `network_manager_get_portal_info_copy()` | 按需复制 Portal 展示信息；未激活时返回 `active=false` 的零值快照 |
| `network_manager_set_notify_callback_borrow()` | 注册不携带状态载荷的快速变化通知 |
| `network_manager_request_start_portal()` | 保留已有配置并进入 Portal |
| `network_manager_request_forget_and_start_portal()` | 清除配置并进入 Portal |

### 3.3 `transport`

`transport` 只处理传输机制：

- `transport_http_perform_borrow()`：有界缓冲式 HTTP 请求。
- `transport_http_stream_borrow()`：同步流式上传和响应读取。
- `transport_http_download_borrow()`：同步 GET 分片下载。
- `transport_websocket_*()`：WebSocket 连接、发送、接收和关闭。

请求描述中的 URL、Header、Body 与回调只在调用期间借用。缓冲式响应必须由调用方调用
`transport_http_response_release()`；WebSocket 消息使用配对释放接口。传输层不理解设备身份、
Dashboard、OTA 或语音语义。

### 3.4 `protocols`

当前 DeskMate 固件只链接：

| 协议 | 公共头 | 端点或职责 |
| --- | --- | --- |
| 设备身份 | `protocol_identity.h` | HTTP/WS 统一生成 `X-Device-Id` 与可选 Bearer Token |
| DeskMate API | `deskmate_api.h` | 拉取并校验 `/api/v1/dashboard` schema 3 及 `next_refresh_at_utc` |
| 语音 | `voice_protocol.h` | 语音流控制消息和响应解码 |
| 日志上传 | `log_upload.h` | 日志批次上传协议 |
| URL | `protocol_url.h` | 服务基础地址规范化与路径拼接 |

同一个 `service_url` 表示两个设备共用服务端主机。DeskMate 只使用共享服务端的
`GET /api/v1/dashboard` 原始业务 JSON；PhotoPainter 的显示接口仍保持 PPF/Manifest 契约。
设备不注册、不刷新 Token，也不上报页面。`DEVICE_API_TOKEN` 为空时保留局域网开发模式；
非空时 HTTP 与 WebSocket 都发送相同的稳定设备 ID 和 Bearer Token。

协议层负责序列化、解析、状态码和响应约束，不拥有重试、周期 Timer、页面状态或产品降级。
Dashboard 原始 JSON 由 `app_network` 交给 `dashboard_store`；协议层同时校验绝对刷新 UTC
秒数，但不决定何时联网，也不直接更新 Data。

### 3.5 `tools/firmware_ota`

`firmware_ota` 拥有独立 Task 和一次 OTA 技术事务：

- 上报当前镜像身份并检查不可变目标。
- 缓存经过版本、防回滚、大小与摘要约束的目标。
- 流式写入备用分区并校验完整文件和 ESP 镜像。
- 切换启动分区；安装成功事件回调返回后立即重启。

它不启动或等待 Wi‑Fi，不决定自动检查/安装，也不调用 Presentation。`app_network` 在
`NETWORK_STATE_ONLINE` 后配置服务身份、提交命令，并把完成回调复制回自己的命令队列。

DeskMate 在 `sdkconfig.defaults` 中把检查路径固定为
`/api/v1/deskmate/ota/check`。两个设备端共用服务端时不得回退到通用
`/api/v1/ota/check`，否则可能取得另一产品固件；共享服务端必须单独实现并发布 DeskMate
清单端点。

设备侧已经使用新清单中的 `ota_version`、Validation SHA-256、完整文件 SHA-256 和受控下载
URL。当前 `tools/dev_ota_version.ps1` 仍面向旧 `DeskMate/server` 与旧清单结构，不属于共享
服务端的有效发布链路；发布工具迁移必须与 DeskMate 服务端清单隔离一起完成，不能用旧脚本
向 PhotoPainter 清单覆盖发布。

## 4. DeskMate 产品边界

以下职责明确不进入 Communication：

- Dashboard 同步周期、重试、401 鉴权失败收敛和四类 Data 快照刷新。
- 连接失败后的新 Network Manager 会话退避。
- 页面状态与导航；页面切换不会触发网络请求。
- OTA 手动入口、自动检查开关、自动安装和呈现状态。
- 实时语音网络租约，以及租约与 Dashboard/OTA/轻睡眠的仲裁。
- 整机轻睡眠前停网和唤醒后恢复顺序。

这些职责由 [`../../main/application/app_network_task.c`](../../main/application/app_network_task.c)
串行拥有。Dashboard 数据格式和缓存位于 `components/data`；音频采集、处理与语音会话仍位于
Service，`app_voice` 只编排产品意图和网络租约。

## 5. 配置与生命周期

项目级配置位于 `sdkconfig.defaults`：

- `CONFIG_CONNECT_PORTAL_*`：DeskMate Portal SSID、密码和表单默认服务地址。
- `CONFIG_NETWORK_MANAGER_DEFAULT_*`：无持久化配置时的可选连接默认值。
- `CONFIG_FIRMWARE_OTA_CHECK_PATH`：产品隔离的 OTA 检查端点。

当前启动关系：

```text
app_network_init
    → network_manager_init_borrow
    → firmware_ota init / callback / start
    → app_network_task
    → network_manager_start
```

轻睡眠只停止 Network Manager 会话和 Application 策略 Timer；空闲的 Firmware OTA Task
保留。活动 OTA 事务会阻止轻睡眠，已缓存但尚未安装的目标不会阻止轻睡眠或语音。

## 6. 故障与恢复

- `connect`、`network_manager`、`transport` 和 Tool 保留原始 ESP-IDF 错误事实。
- `network_manager` 只执行组件内一轮技术重试；Application 才能开始下一轮会话。
- HTTP、解析或 OTA 失败不会由 Communication 决定整机重启。
- `network_manager_stop()` 清理失败属于顶层不可安全恢复事实，Application 必须阻止轻睡眠或
  后续会话，而不能绕过 Manager 直接重建 Wi‑Fi。
- OTA 下载一旦开始不可取消；写入失败清除缓存目标，必须重新检查。

## 7. 验证

- `transport` 不依赖 `connect` 或 `network_manager`。
- Communication 不包含 Application、Presentation、UI、Service、BSP、Driver 或 Board 头文件。
- `network_manager` 的通知回调和 Firmware OTA 完成回调只复制事实并快速返回。
- DeskMate 构建只链接 `protocols/CMakeLists.txt` 中显式选择的协议源码。
- 固件构建与实机 Wi‑Fi、Portal、Dashboard、语音和 OTA 验证必须通过项目统一流程另行执行。
