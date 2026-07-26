# `remote_log`

> 在 Communication 内部捕获 ESP-IDF 日志，并在 Network Manager 在线时通过
> `protocols/log_upload` 批量上传。

## 1. 定位

- 层级：`components/communication/tools` 下的远端诊断工具。
- 触发方：由目标项目的组合根或 Application 完成配置并启动。
- 主要输出：服务端日志 session、批量日志事件和本地运行状态快照。

## 2. 职责边界

负责：

- 通过链接器包装 Log V2 的 `esp_log()` 入口，同时保留原日志输出。
- 以固定容量队列缓存完整日志，直接记录级别、Tag、正文、序号和运行时间。
- 在独立 Task 中读取 Network Manager 状态、创建远端日志 session、聚合批次并失败重试。
- 通过 `remote_log_get_status_copy()` 报告排队、上传、丢弃和失败事实。

不负责：

- 启动、停止或等待 Network Manager 完成具体产品网络会话。
- 读取 Storage、决定服务端配置来源或保存设备身份。
- 延长在线窗口、阻止休眠、决定故障是否致命或执行重启。
- 修改 `protocols/log_upload` 的服务端协议、认证和持久化语义。

## 3. 主要流程

```text
ESP_LOG 输出
    ↓ Log V2 esp_log 包装（esp_log_va 继续执行原输出）
固定容量日志队列
    ↓
remote_log_task
    ├─ Network Manager 非 ONLINE → 保留当前批次并退避
    └─ Network Manager ONLINE
           ↓
       log_upload_start → log_upload_batch → transport
```

`remote_log_init()` 启用已经由链接器接入的捕获入口并立即缓存后续启动日志。上传 Task 自己产生
的日志不会重新进入队列，避免协议和 HTTP 诊断日志形成反馈循环。

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `network_manager` | 读取 `NETWORK_STATE_ONLINE` 事实，不控制网络生命周期 |
| 调用 | `protocols` | 使用 `log_upload_start()` 和 `log_upload_batch()` |
| 私有依赖 | ESP-IDF `log` / `freertos` / `esp_app_format` / `esp_system` | Log V2 包装、Task/Queue、固件版本和复位原因 |
| 被调用 | 目标组合根或 Application | 配置服务地址、设备 ID 并管理 Tool 生命周期 |

虽然位于 Communication 内部，`remote_log` 仍保持单向依赖：`network_manager` 和 `protocols`
都不反向依赖本 Tool，因此不会形成组件环。

## 5. 公共接口

公共头文件：[`include/remote_log.h`](include/remote_log.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `remote_log_config_set_defaults()` | 同步 | 写入推荐的容量、重试和 Task 默认值 |
| `remote_log_init()` | 同步 | 创建缓存资源并启用 Log V2 捕获，不创建 Task |
| `remote_log_configure_copy()` | 同步 | 复制服务端 URL、产品 ID 和设备 ID，清空旧 session |
| `remote_log_start()` | 异步启动 | 创建上传 Task；网络未在线时在后台等待 |
| `remote_log_stop()` | 同步 | 请求停止并在调用方超时内等待 Task 到达安全停止点 |
| `remote_log_deinit()` | 同步 | 停用捕获并释放资源 |
| `remote_log_get_status_copy()` | 同步 | 原子复制运行状态和统计信息 |

## 6. 状态、生命周期与并发

```text
init → configure_copy → start → stop → deinit
                         ↑       │
                         └───────┘
```

- Log V2 包装入口可以被多个业务 Task 并发调用，只做有界格式化与非阻塞入队。
- 上传 Task 是 session、当前批次和重试流程的唯一执行者。
- 初始化、配置、启动、停止和反初始化必须由同一编排上下文串行调用；状态快照可并发读取。
- 队列满时拒绝最新日志并累计 `dropped_lines`，不阻塞业务日志调用。
- 停止请求不取消正在执行的同步 HTTP；超时后保留 `STOPPING`，允许再次等待。
- `stop()` 不清空仍在队列中的日志；已经取出的当前批次在停止时计为丢弃。
- `deinit()` 先停用捕获并等待已经进入钩子的调用退出，再释放队列与同步资源。
- 网络会话所有者必须在 Network Manager 已经 ONLINE 后启动上传，并在调用
  `network_manager_stop()` 前同步完成 `remote_log_stop()`；否则同步日志 HTTP 可能与 Wi-Fi
  Driver 释放发生竞态。
- 链接器包装始终继续调用 `esp_log_va()`；`init()` / `deinit()` 只启停远端捕获，不修改
  `esp_log_set_vprintf()`。

## 7. 故障与恢复

- Network Manager 未在线时不调用协议层，按 `retry_interval_ms` 重新检查。
- `log_upload_start()` 或 `log_upload_batch()` 失败时保留当前会话或批次并退避重试。
- 上传成功后若服务端返回新的 `session_id`，后续批次改用新会话。
- 是否因长期上传失败而停止网络会话、降级或重启由 Application 决定。

## 8. 配置与限制

- 默认队列 64 条、每批 8 条、聚合 100 ms、失败重试 3 秒、HTTP 超时 3 秒。
- 组件要求启用 `CONFIG_LOG_VERSION_2` 和 `CONFIG_LOG_MODE_TEXT`，并通过 `--wrap=esp_log` 接入
  普通 `ESP_LOGx`。
- `base_url`、正整数 `product_id` 与 `device_id` 由调用方复制配置；Tool 不读取项目持久化配置。
- 当前 `log_upload` 不接收设备 Token，因此本 Tool 暂不处理认证。
- Network Manager 当前未公开 STA IP 快照，boot 事件中的 `ip` 暂时为空字符串。
- `ESP_EARLY_LOGx`、`ESP_DRAM_LOGx`、ROM 与 panic 的直接输出不经过普通 `esp_log()`，不在本
  Tool 的捕获范围内。
- 单条日志受 `log_upload_line_t` 固定字段容量限制；过长内容会被截断。

## 9. 验证

- 静态检查：公共 C ABI、中文 Doxygen、Task 文件命名、Log V2 链接包装、队列满策略和单向
  组件依赖。
- 实机接入后：验证一条 `ESP_LOGx` 只生成一条远端记录、原串口输出不变、早期日志缓存、离线
  重试、session 创建、批次顺序、停止超时和丢弃统计。
