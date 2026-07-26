# Communication 层

> 本文只说明 Communication 内部的子组件划分与职责边界。全项目规则以 [`docs/architecture/`](../../docs/architecture/README.md) 为准。

## 1. 定位

Communication 层负责设备与服务端之间的网络通信，基础能力按“链路控制”和“业务数据”拆成
两条相互独立的依赖链；需要协调两条链的通用技术流程放在 `tools`：

```
Application / Service
    ├── 链路控制 ──▶ network_manager ──▶ connect
    ├── 业务数据 ──▶ protocols ──▶ transport
    └── 通信工具 ──▶ remote_log ─┬─▶ network_manager
                                 └─▶ protocols
```

两条基础链分别保持单向、无环依赖。`transport` 不编译依赖 `network_manager` 或 `connect`；
调用普通传输接口前网络已经可用仍由 Application 或 Service 编排。Communication 内部 Tool
只有在自身职责需要协调完整技术流程时，才可同时依赖两条基础链，并且不得让基础组件反向依赖
Tool。

### 1.1 实现语言与 ABI

Communication 采用 C/C++ 混合实现，对外接口仍保持 C ABI：公共头文件以及跨 C/C++ 私有边界均使用
`extern "C"` 保护，因此现有 C Application、Service 和组件不需要修改调用方式。

新增或修改实现时，语言选择和跨语言约束统一遵守
[项目 C/C++ 语言边界规范](../../docs/architecture/c_cpp_boundary.md)，本 README 不维护按语言划分的
源码清单。

## 2. 子组件说明

### 2.1 connect

Wi-Fi 物理链路与配网 Portal 的底层能力，与产品生命周期无关。直接依赖 ESP-IDF Wi-Fi、Netif、Event 和 HTTP Server。
`connect.cpp` 使用 `ConnectRuntime` 收拢 Wi-Fi Driver 状态、STA netif、事件实例、回调和生命周期锁；
Portal、二维码、DNS 和扫描继续保留为独立 C 模块。

**公共接口**（`connect/include/connect.h`）：

| 接口 | 说明 |
|---|---|
| `connect_init()` / `connect_deinit()` | 初始化与反初始化 Wi-Fi 驱动和内部资源 |
| `connect_request_start_station_copy()` | 发起一次 STA 连接 |
| `connect_request_start_station_with_portal_copy()` | 保持 Portal，使用 APSTA 验证候选 STA 配置 |
| `connect_disconnect_station_keep_portal()` | 候选失败后只断开 STA，保留 Portal |
| `connect_complete_portal_station()` | 候选成功后关闭 Portal 并保留 STA 链路 |
| `connect_start_portal_copy()` | 启动配网热点 Portal |
| `connect_set_portal_status_borrow()` | 更新 Portal 状态页展示信息 |
| `connect_get_link_snapshot_copy()` | 同步查询当前 STA 链路快照 |
| `connect_set_callbacks_borrow()` | 注册链路事件、配网提交和真实页面活动回调 |
| `connect_render_portal_qr_borrow()` | 通过显示格式无关的 sink 输出配网二维码矩形 |
| `connect_stop()` | 停止 Portal 与 Wi-Fi 资源 |

**内部子模块**：

- `connect.cpp` — `ConnectRuntime`、STA 链路和 Wi-Fi/IP 事件
- `connect_portal.c` — Portal HTTP 服务主逻辑
- `connect_portal_form.c` — 配网表单解析
- `connect_portal_qr.c` — 显示格式无关的配网二维码生成
- `connect_portal_dns.c` / `connect_portal_dns_task.c` — Captive Portal DNS 劫持
- `connect_portal_scan_task.c` — AP 扫描任务

二维码模块不依赖具体屏幕驱动：调用方通过 `connect_qr_sink_t` 接收布局和黑色矩形，可适配不同
显示屏。浏览器配网继续使用 Portal 表单，二维码只负责把 Wi-Fi 配置 payload 提供给外部显示入口。
Portal 热点名称、热点密码和表单默认服务地址分别由
`CONFIG_CONNECT_PORTAL_SSID`、`CONFIG_CONNECT_PORTAL_PASSWORD` 和
`CONFIG_CONNECT_PORTAL_DEFAULT_SERVICE_URL` 配置；组件只提供不含产品名的通用默认值，具体产品
应在项目级 `sdkconfig.defaults` 中覆盖。

`ConnectRuntime` 不在析构函数里自动调用 `connect_stop()` 或 `connect_deinit()`。停止和反初始化可能失败，
失败时必须保留 `cleanup_failed` 与底层资源，供上层进入明确的故障收敛流程。

### 2.2 network_manager

Wi-Fi 链路、重连与配网的状态机，管理连接的完整生命周期。`network_manager.cpp` 负责公共状态、
回调和初始化状态，`network_manager_task.cpp` 中的 `NetworkManagerRuntime` 持有命令队列、Task、
会话编号、重试截止时间、当前配置、配置持久化回调、状态元数据和 Portal 展示信息。

**状态流转**：

```
STOPPED ──start()/有配置──▶ CONNECTING ──────────────▶ ONLINE
  │                              ▲  │                     │
  │                              │  ▼                     ▼
  │                              └──RETRY_WAIT ◀──── disconnect
  │                                  │
  │                              重试耗尽
  │                                  ▼
  └──start()/无配置───────────────▶ ERROR ──Application 请求──▶ PROVISIONING
                                      │                           │
                                      │                      Portal 提交
                                      │                           ▼
                                      │                       VALIDATING ──IPv4──▶ ONLINE
                                      │                           │
                                      │                           └──失败──▶ PROVISIONING
                                      └──stop()──▶ STOPPING ──▶ STOPPED
```

**状态说明**：

| 状态 | 含义 |
|---|---|
| `STOPPED` | 当前没有活动网络会话，底层 Wi-Fi 资源已完成清理 |
| `CONNECTING` | 正在使用当前配置发起 STA 连接并等待可用 IPv4 |
| `ONLINE` | STA 已关联且具有可用 IPv4 |
| `RETRY_WAIT` | 上一次连接失败，正在等待退避截止时间后重试 |
| `PROVISIONING` | 配网 Portal 已启动，等待用户提交配置 |
| `VALIDATING` | Portal 保持在线，正在用 APSTA 验证尚未持久化的候选配置 |
| `ERROR` | 当前会话出现不可自动恢复的错误，等待上层结束会话 |
| `STOPPING` | 正在同步停止 Task、Portal、DNS、扫描和 Wi-Fi 资源 |

Portal 表单提交回调只负责把提交内容复制为 `NETWORK_COMMAND_PORTAL_SUBMISSION` 命令；页面打开、
真实点击、滚动、输入和修改另行发布活动命令，自动扫描与状态轮询不发布。候选配置、状态转换和
后续 STA 连接全部在 `network_manager_task` 串行执行。候选配置获得 IPv4 后才通过注入的保存回调
持久化并替换 active 配置；失败时只断开候选 STA，Portal 与旧配置均保留。内部命令保持 POD，
适合由 FreeRTOS 队列按值复制。变化通知回调在锁外调用，避免用户回调阻塞内部临界区。

配置的加载、保存和清除由 `network_manager_config_store_t` 注入。Network Manager 定义网络配置
语义，但不包含 `system_storage` 头文件，也不依赖具体 NVS、文件或其他存储实现；当前产品由
`bootstart_app` 将这三个回调适配到 `system_storage`。

公共状态只包含 `state`、对应的原始 `esp_err_t` 和 `portal_activity_sequence`，三者在同一临界区
复制，供 Application 安全判断状态与 Portal 无交互截止时间。回调只负责快速通知，不携带状态；
大型 Portal 二维码信息和持久化配置存在性通过专用函数按需读取。重试计数、候选配置和
ESP-IDF 原始断开原因保持为 Communication 内部事实，不进入公共状态。

`NetworkManagerRuntime` 同样不依赖析构自动停止。`network_manager_stop()` 超时或清理失败时会保留
`STOPPING` 或 `CLEANUP_FAILED` 生命周期，禁止在资源状态不确定时启动新会话。
持久化提供者返回 `ESP_ERR_NOT_FOUND` 时使用的编译期回退值由 `CONFIG_NETWORK_MANAGER_DEFAULT_SSID`、
`CONFIG_NETWORK_MANAGER_DEFAULT_PASSWORD` 和 `CONFIG_NETWORK_MANAGER_DEFAULT_SERVICE_URL`
配置；这些符号不携带产品前缀。

**公共接口**（`network_manager/include/network_manager.h`）：

| 接口 | 说明 |
|---|---|
| `network_manager_init_borrow()` | 注入配置持久化回调并初始化管理器 |
| `network_manager_start()` | 启动管理任务，读取持久化配置并发起连接 |
| `network_manager_stop()` | 同步停止会话并释放 Wi-Fi 驱动 |
| `network_manager_get_status_copy()` | 原子复制当前状态、错误码和 Portal 活动序号 |
| `network_manager_get_portal_info_copy()` | Portal 活动期间按需复制二维码展示信息 |
| `network_manager_has_saved_config()` | 查询 active 网络配置是否已经持久化 |
| `network_manager_set_notify_callback_borrow()` | 注册不携带状态载荷的快速变化通知回调 |
| `network_manager_request_start_portal()` | 请求进入配网模式 |
| `network_manager_request_forget_and_start_portal()` | 清除配置并进入配网模式 |

### 2.3 transport

不感知业务协议的底层传输抽象，提供同步 HTTP 缓冲请求、流式上传/下载和 WebSocket 消息收发。
公开 API 保持 C ABI，资源所有权由内部 C++ 实现管理。

**公共接口**（`transport/include/transport.h` 聚合导出）：

**HTTP**（`transport_http.h`）：

| 接口 | 说明 |
|---|---|
| `transport_http_perform_borrow()` | 同步缓冲式 HTTP 请求，支持 GET/POST/PUT/PATCH/DELETE |
| `transport_http_stream_borrow()` | 同步流式上传与响应读取 |
| `transport_http_download_borrow()` | GET 流式下载，响应数据通过回调分块交付 |
| `transport_http_response_release()` | 释放响应体内存 |

`transport_http.cpp` 使用 RAII 管理 `esp_http_client_handle_t` 和请求期间的临时缓冲区；所有返回路径
都会关闭已显式打开的连接并销毁 Client。成功交给调用方的响应体除外，仍由
`transport_http_response_release()` 显式释放。

**WebSocket**（`transport_websocket.h`）：

| 接口 | 说明 |
|---|---|
| `transport_websocket_open()` | 建立 WebSocket 连接 |
| `transport_websocket_send_text()` / `_binary()` | 发送文本/二进制消息 |
| `transport_websocket_receive()` | 从接收队列取消息（支持分片自动组装） |
| `transport_websocket_is_connected()` | 查询连接状态 |
| `transport_websocket_dropped_messages()` | 查询丢弃消息计数 |
| `transport_websocket_close()` | 关闭连接并释放资源 |

`transport_websocket.cpp` 使用公开头文件声明的 opaque handle 对应一个 C++ 对象，统一持有 Client、
接收队列、连接信号量和分片缓冲。`transport_websocket_close()` 删除该对象并释放全部所有权资源。
内部对分片消息进行自动组装，超大消息和队列溢出时会计入 `dropped_messages` 统计；接收缓冲分配
使用 SPIRAM，队列元素 `transport_websocket_message_t` 保持可按值复制。

### 2.4 protocols

业务协议层，建立在 transport 之上，负责各业务领域的请求构造与响应解析。

**公共接口**（`protocols/include/protocols.h` 聚合导出）：

| 协议 | 头文件 | 说明 |
|---|---|---|
| 设备状态 | `device_status_protocol.h` | 温湿度与电池状态 JSON 上传 |
| 显示 | `display_protocol.h` | Manifest v3 绝对 UTC 调度、多页面集合查询、PPF 帧流式下载 |
| 语音 | `voice_protocol.h` | 语音帧语义定义（ASR/TTS/思考/错误）、流式解码器、WebSocket 控制消息 |
| 日志 | `log_upload.h` | 远端日志会话启动与批量上传 |

**设备状态与显示关键流程**：
1. `device_status_protocol_upload_borrow()` — 上传最近一次有效的温湿度和电池状态
2. `display_protocol_get_manifest_copy()` — 查询当前设备显示集合 Manifest（支持 304 Not Modified）
3. `display_protocol_download_frame_borrow()` — 流式下载 Manifest 指定的 PPF 帧文件

### 2.5 tools/firmware_ota

可跨项目迁移的应用固件 OTA 工具。它拥有独立 Task，在调用方已经保持网络在线时完成制品检查、
流式下载、完整文件 SHA-256 校验、备用分区写入和启动分区切换。工具不拥有 Wi-Fi 或产品休眠
策略；事务一旦接受便不可取消，切换启动分区后必须立即重启。公共接口见
`tools/firmware_ota/include/firmware_ota.h`，完整边界见组件 README。

### 2.6 tools/remote_log

ESP-IDF 远端日志采集与上传工具。组件通过链接器包装 Log V2 `esp_log()` 入口，并由
`remote_log_init()` 启用非阻塞捕获；包装入口继续调用 `esp_log_va()` 保留原日志输出。独立
Task 读取 `network_manager` 在线状态，只在 `NETWORK_STATE_ONLINE` 时通过
`protocols/log_upload` 建立 session 和批量上传。Tool 不启停 Wi-Fi、不读取 Storage，服务
地址和设备 ID 由调用方复制配置。队列满时拒绝最新日志并通过状态快照累计丢弃数量。

**公共接口**（`tools/remote_log/include/remote_log.h`）：

| 接口 | 说明 |
|---|---|
| `remote_log_init()` / `remote_log_deinit()` | 启停 Log V2 捕获并管理缓存资源 |
| `remote_log_configure_copy()` | 复制服务地址和设备 ID |
| `remote_log_start()` / `remote_log_stop()` | 启动或同步停止上传 Task |
| `remote_log_get_status_copy()` | 复制上传状态、错误和日志统计 |

## 3. 目录结构

```
communication/
├── README.md               # 本文件
├── connect/                # Wi-Fi STA 与配网 Portal
│   ├── include/connect.h
│   └── src/
│       ├── connect.cpp
│       ├── connect_portal.c
│       ├── connect_portal_dns.c
│       ├── connect_portal_dns_task.c
│       ├── connect_portal_form.c
│       ├── connect_portal_qr.c
│       └── connect_portal_scan_task.c
├── network_manager/        # Wi-Fi 状态机与重连
├── tools/firmware_ota/     # 独立 Task 的可迁移应用固件 OTA 工具
│   ├── include/firmware_ota.h
│   └── src/
│       └── firmware_ota_task.c
├── tools/remote_log/       # ESP_LOG 缓存、在线检测与批量远端上传
│   ├── include/remote_log.h
│   └── src/
│       ├── remote_log.c
│       └── remote_log_task.c
├── transport/              # HTTP / WebSocket 传输
│   ├── include/
│   │   ├── transport.h           # 聚合导出
│   │   ├── transport_http.h
│   │   └── transport_websocket.h
│   └── src/
│       ├── transport_http.cpp
│       └── transport_websocket.cpp
└── protocols/              # 业务协议
    ├── include/
    │   ├── protocols.h           # 聚合导出
    │   ├── device_status_protocol.h
    │   ├── display_protocol.h
    │   ├── ota_manifest.h
    │   ├── voice_protocol.h
    │   └── log_upload.h
    └── src/
```

## 4. 依赖关系

```
protocols  ──REQUIRES──▶  transport  ──PRIV_REQUIRES──▶  esp_http_client, esp_websocket_client, freertos, heap
remote_log ──PRIV_REQUIRES──▶ network_manager, protocols, freertos, log
network_manager  ──PRIV_REQUIRES──▶  freertos, utils
network_manager  ──REQUIRES──▶  connect
connect  ──REQUIRES──▶  esp_wifi, esp_event, esp_netif, esp_http_server, lwip
connect  ──PRIV_REQUIRES──▶  utils
```

- `transport` 和 `connect` 分别是业务数据链与链路控制链的基础组件，可被符合全局分层规则的组件直接依赖。
- `network_manager` 被 Application 和具有明确协调职责的 Communication Tool 调用，不向
  `transport` 或 `protocols` 暴露。
- `remote_log` 是 Communication 内部 Tool，可读取 `network_manager` 状态并调用 `protocols`；
  它不控制网络生命周期，两个被依赖组件也不反向依赖本 Tool。
- `protocols` 是业务协议层，Application 和 Service 均可调用。
- “网络已可用”是调用 `transport`/`protocols` 的运行时前置条件，不构成它们对
  `network_manager`/`connect` 的编译依赖。

## 5. 设计约束

- **稳定 C ABI**：所有公共 API 和跨语言私有 API 均使用 `extern "C"`；是否由 `.c` 或 `.cpp`
  实现不影响调用方链接名称。
- **所有权语义**：普通同步 `const` 输入默认只在调用期间借用；`_copy` 表示跨调用边界复制或复制输出快照，`_take` 表示成功后转移所有权，长期注册的借用回调使用 `_borrow` 明确借用终点。
- **RAII 边界**：HTTP 临时资源和单个 WebSocket 对象使用 RAII；Network Manager 与 connect 的停止
  可能失败，必须由显式 API 驱动并保留失败状态，不在静态 Runtime 析构阶段隐式清理。
- **队列数据**：FreeRTOS 队列只复制 POD 命令或消息描述；带所有权的数据通过明确指针和释放 API
  交接，不把含构造/析构语义的 C++ 对象直接放入队列。
- **同步执行**：所有 HTTP 和 WebSocket 操作均为同步阻塞，调用方负责在合适的 Task 上下文中使用。
- **中文日志**：所有 `ESP_LOG*` 输出、断言说明和运行期提示使用中文。
- **配置持久化回调**：`network_manager_init_borrow()` 复制回调描述符，长期保存回调函数指针并
  借用 `ctx`；三类回调由 `network_manager_task` 同步调用，回调实现和 `ctx` 的有效期必须覆盖
  管理器的进程生命周期，且不得重入 Network Manager 控制 API。
- **回调规范**：链路和状态等异步事件回调必须快速返回，耗时处理应复制数据并投递到调用方
  队列；同步 HTTP 流式回调可以执行写入目标等有界工作，但不得无上限等待或重入所属组件的
  控制 API。具体执行上下文以公共头文件为准。
