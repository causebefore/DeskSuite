# Web Console 可移植组件契约

> 状态：已确认的迁移目标契约，修订于 2026-07-30。
>
> 本文固定 `web_file_service` 向共享 `web_console_service` 迁移时必须保持的职责、依赖和裁剪
> 边界。迁移期间当前实现与本文存在的差异属于待迁移项，不能反向扩张本文职责。

## 1. 目标

`web_console_service` 是可跨产品移植的本地认证管理控制台 Service。它通过 ESP-IDF HTTPD
提供 Web UI 与 HTTP API，统一拥有认证会话、URI 生命周期、活动 handler 记账和有界停止；
文件、用户设置和状态展示属于可裁剪模块。

目标源码位置为：

```text
shared/components/services/web_console_service/
shared/components/services/web_console_network_provider/
```

可选的 Communication 适配源码独立形成 `web_console_network_provider`。产品 Application 或
Composition Root 负责选择模块、装配 Provider，并决定网络租约与 Service 启停时机。

本迁移不把 Portal HTTPD、WebDAV、WebSocket、递归文件操作、远程公网暴露或产品级维护策略
纳入目标。

## 2. 目标依赖

```text
Product Application / Composition Root
        ├──→ web_console_service
        ├──→ web_console_network_provider（可选）
        └──→ network_manager

web_console_network_provider ──→ web_console_service Provider 契约
web_console_network_provider ──→ network_manager
network_manager ──→ connect
```

必须满足：

- `web_console_service` Core 不依赖 `connect`、`network_manager`、产品 Application 或产品
  `System`。
- `web_console_network_provider` 只单向依赖 Console 与 `network_manager` 的稳定公共 API。
- Communication 不包含、不链接也不条件调用 Web Console；裁剪 Console 后 Communication
  必须保持独立编译与运行。
- `transport` 是出站 HTTP/WebSocket 客户端，不作为入站 HTTPD 的复用层。
- Portal 继续拥有独立 APSTA、DNS、HTTPD 和候选配置验证生命周期，不与 Console 共享 HTTPD
  句柄或 URI 表。

## 3. Core 职责

Core 负责：

- 创建、运行和有界停止自己的 HTTPD。
- 每次启动生成访问码，拥有登录锁定、唯一会话 token 与空闲失效状态。
- 统一注册和注销本次构建启用的精确 URI。
- 为所有项目 handler 建立入口关闭、活动计数和停止排空屏障。
- 统一设置认证、缓存、安全响应头和有界 JSON 错误响应。
- 复制有界运行摘要，不暴露内部 HTTPD、Task、锁或 Provider 指针。
- 只通过本地运行摘要复制访问码；调用方不得记录或远程转发，并在使用后覆盖副本。

Core 不负责：

- 启动、停止或重连 Wi-Fi，不申请或释放产品网络租约。
- 挂载文件系统、写入 NVS、读取产品设置结构或决定设置生效策略。
- 调用 Provider 所属组件的 `init/start/stop/deinit`。
- 决定产品级降级、重试、重启、页面导航或维护时机。

Core 必须在没有 Files、Settings 和 Network Provider 时仍能独立完成
`init/start/stop/deinit`。空控制台可以只提供认证入口与启用模块清单。

## 4. 可裁剪模块

### 4.1 Files

Files 模块复用当前已经验证的路径校验、单传输守卫、流式目录/下载、事务上传、单项文件变更
和恢复语义。模块通过初始化配置接收：

- 文件系统逻辑根目录。
- Service 自有事务工作目录名。
- 总容量与可用容量的查询回调。
- 上传大小、保留空间等技术配置。

Files 不挂载或卸载文件系统。文件系统根、事务目录和容量 Provider 的借用期必须覆盖
`init` 成功到 `deinit` 完成；调用方在此期间不得释放其上下文。关闭 Files 构建开关后，
Core 不编译文件 handler、事务恢复和文件传输源码，也不保留产品 `System` 依赖。

### 4.2 Settings 与 Status

Settings/Status 模块只负责认证后的 HTTP 映射、字段元数据编码、Provider 发现和结果分发。
领域 Provider 继续拥有数据、校验、持久化、运行时应用和失败策略。

Provider 集合只通过 `web_console_service_init_borrow()` 的初始化配置一次性装配，运行中不得
动态增加、替换或注销。Console 复制回调集合并长期借用 `ctx`，借用期在 `deinit` 完成时
结束。Provider 回调不得接收或保存 `httpd_req_t`，不得绕过 Core 的认证、handler 记账和
停止屏障。阶段 2 纯重命名暂时保留无参数 `init()`；引入 Files/Provider 注入时再独立迁移为
带 `_borrow` 的装配契约。

Settings Provider 必须能够表达：

- 稳定 section/field ID、类型、长度、范围和枚举约束。
- `READ_ONLY`、`SECRET`、`WRITE_ONLY` 等访问属性。
- 立即生效、下一事务、重连、重启或仅空闲态等生效事实。
- 公开快照、完整候选或 patch 校验、异步应用请求及其最终结果。
- version，用于拒绝基于旧快照的覆盖提交。

Secret 只允许报告是否已配置，不得通过快照、日志或错误正文返回原值。Console 不认识 NVS
namespace/key、JSON 文件路径或产品持久化结构。

### 4.3 Network Provider

`web_console_network_provider` 首个版本只提供 `network_manager` 已公开的状态和诊断事实，包括
Manager 状态、IPv4、网关、DNS、RSSI、AP 信息、已保存配置事实和 Portal 活动状态。

它是无 Task、Queue、Timer、可变状态和独立生命周期的叶子适配组件，只提供固定 Provider
描述符及回调；读取请求到达时调用 `network_manager_get_diagnostics_copy()`。不得为形式增加
空的 `init/start/stop/deinit`。

它不得：

- 直接调用 `connect`。
- 启停 Network Manager、发起重连、申请产品租约或直接切换 Portal。
- 直接读写网络 NVS 或取得 `network_manager` 配置存储回调。
- 注册并替换产品 Application 已持有的唯一网络通知回调。

按 HTTP 请求读取最新诊断即可满足首版需求。未来若增加网络配置修改，必须由 Application
先接受异步产品请求，在 HTTP 响应完成后停止 Console、释放租约，再由 Network Manager 验证
候选配置；HTTP handler 不得停止自身 Service。

## 5. 生命周期、并发与所有权

生命周期保持：

```text
UNINITIALIZED
    └─ init ─→ INITIALIZED
                   └─ start ─→ STARTING ─→ RUNNING
                                              └─ stop ─→ STOPPING ─→ INITIALIZED
INITIALIZED ── deinit ─→ UNINITIALIZED
STARTING / STOPPING ── 清理失败 ─→ CLEANUP_FAILED
CLEANUP_FAILED ── 后续 stop 成功 ─→ INITIALIZED
```

- `start()` 成功返回时 HTTPD、认证和全部启用模块 URI 已经运行。
- `stop(timeout_ms)` 成功返回时入口已关闭、handler 已排空、模块运行资源和 HTTPD 已释放。
- 停止超时或清理失败时保留资源所有权和显式失败状态，拒绝再次启动。
- Core 状态锁只保护短时内存事实；文件、网络、持久化 I/O 和外部回调都在锁外执行。
- Provider 回调必须声明执行上下文。可能阻塞或改变产品状态的操作必须转为异步请求，不能在
  HTTPD Task 内直接执行长期事务。

## 6. HTTP 与安全边界

- 迁移阶段保持现有文件 URI、状态码、认证与事务语义，命名和移动提交不得混入协议变化。
- 纯重命名阶段暂时保留 `X-DeskMate-Overwrite`、HTML 产品标题和浏览器存储键；移入
  `shared` 前必须改为产品无关协议名称或通过初始化/构建配置注入，公共源码不得残留产品名。
- 当前 HTTPD 为局域网明文 HTTP。首版 Settings 不允许写入 Wi-Fi 密码、设备 Token 等 Secret；
  是否开放 Secret 写入必须单独确认传输安全策略。
- 所有认证后 API 默认 `Cache-Control: no-store`，不得记录访问码、Bearer token、密码或完整
  Authorization。
- Files、Settings、Status 和 Network Provider 的路由必须在启动前形成固定、有界、无冲突
  的表；停止时由 Core 对称注销。

## 7. 构建与裁剪验收

迁移完成必须验证以下构建组合：

| 组合 | 必须成立的事实 |
| --- | --- |
| Communication-only | 不发现或链接任何 Web Console 符号 |
| Console Core-only | 不依赖文件系统或 Communication，能够独立启停 |
| Core + Files | 保持现有文件 HTTP、安全和恢复契约 |
| Core + Settings | 使用内存 Provider 即可构建，不依赖产品存储 |
| Core + Network Provider | 只增加对 `network_manager` 的单向只读依赖 |
| DeskMate 完整组合 | Application 继续拥有网络租约和启停时机 |

静态检查、固件编译和真实设备验收是三个独立等级。固件编译只能通过 DeskSuite 根目录
`ds.ps1` 执行；编译通过不代表 HTTP、SD、断网或停止流程已经完成设备验收。

## 8. 迁移提交顺序

迁移按以下可独立回滚的阶段执行：

1. 确认本文契约与受控术语。
2. 在原位置纯重命名为 `web_console_service`，不改变运行行为。
3. 拆出可独立运行的 Core。
4. 将 Files 改为可裁剪模块并注入文件系统能力。
5. 将已去产品依赖的组件移动到 `shared/components/services/`。
6. 增加通用 Settings/Status Provider 框架。
7. 增加可选只读 `web_console_network_provider`。

每一阶段必须独立编译并提交；命名、移动、行为重构和新增功能不得合并成一个提交。

`app_web_console` 与 `APP_NETWORK_LEASE_WEB_CONSOLE` 是 DeskMate 产品编排符号；当前入口仍因
启用 Files 能力而执行 SD 前置检查，但后续开放 Settings、Status 或维护入口时不得重新引入
`web_file` 产品命名。共享 Console 和 Provider 不得依赖这些产品符号。
