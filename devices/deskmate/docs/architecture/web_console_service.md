# Web Console 可移植组件契约

> 状态：已实现的共享组件与 DeskMate 装配契约，修订于 2026-08-08。
>
> 本文固定共享 `web_console_service` 与 DeskMate 产品装配必须保持的职责、依赖和裁剪边界；
> 后续功能不能反向扩张本文职责。

## 1. 目标

`web_console_service` 是可跨产品移植的本地认证管理控制台 Service。它通过 ESP-IDF HTTPD
提供 Web UI 与 HTTP API，统一拥有认证会话、URI 生命周期、活动 handler 记账和有界停止；
文件、用户设置、状态展示和非破坏性操作属于可裁剪模块。

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

Core 必须在没有 Files、Settings、Status、Actions 和 Network Provider 时仍能独立完成
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

### 4.2 Settings、Status 与 Actions

Settings/Status/Actions 模块只负责认证后的 HTTP 映射、字段元数据编码、Provider 发现和结果分发。
领域 Provider 继续拥有数据、校验、持久化、运行时应用和失败策略。

认证后的顶层只显示“文件管理”“设置”和“退出登录”；退出登录是会话动作，不是领域模块。
浏览器按相同 `section_id` 合并 Settings、Status 与 Actions 能力，在“设置”首页显示客户分组，
再进入一层详情。不同 Provider 类型允许复用同一 section ID，同一类型内部仍要求唯一。关闭
任一模块时，其 endpoint、文案、HTML/CSS/JS 标记和 Provider 存储均不得进入构建产物。

DeskMate 完整装配只形成三个客户分组：`hub`（Hub Settings + Hub Actions）、`pomodoro`
（番茄钟 Settings）和 `system`（“设备与系统” Status）。调试型 Network Manager Status
Provider 不参与产品装配，也不形成 Wi-Fi 分类。

Provider 集合只通过 `web_console_service_init_borrow()` 的初始化配置一次性装配，运行中不得
动态增加、替换或注销。Console 复制回调集合并长期借用 `ctx`，借用期在 `deinit` 完成时
结束。Provider 回调不得接收或保存 `httpd_req_t`，不得绕过 Core 的认证、handler 记账和
停止屏障。

Settings Provider 必须能够表达：

- 稳定 section/field ID、类型、长度、范围和枚举约束。
- section/field 的 UTF-8 标签与可选 `description`、`unit`、`summary`，以及 ASCII `format`。
- STRING 值统一使用有效 UTF-8，单值同时受字段上限与全局 127 bytes 上限约束，并拒绝
  embedded NUL；显示元数据按各自上限使用有效 UTF-8。
- 普通可读写字符串可选声明文件扩展名；该元数据只在 Files 同时启用时合法，浏览器通过
  认证后的目录接口选择逻辑路径，Provider 回调不得为此访问文件系统。
- `READ_ONLY`、`SECRET`、`WRITE_ONLY` 等访问属性。
- 立即生效、下一事务、重连、重启或仅空闲态等生效事实。
- 公开快照、完整候选或 patch 校验、异步应用请求及其最终结果。
- version，用于拒绝基于旧快照的覆盖提交。

Secret 只允许报告是否已配置，不得通过快照、日志或错误正文返回原值。Console 不认识 NVS
namespace/key、JSON 文件路径或产品持久化结构。

Actions 只表达非破坏性、异步管理操作，固定增加两条认证路由：

```text
POST /api/actions?section=<id>&action=<id>
GET  /api/actions/result?section=<id>&action=<id>&request=<uint64>
```

POST 只完成有界输入校验和快速复制/排队，GET 查询 `pending/succeeded/failed` 与稳定 reason。
Settings 与 Actions 共用 reason 集合；pending/succeeded 必须为 `none`，failed 必须为非
`none`。浏览器把已知 reason 呈现为确定终态，只有网络异常或轮询期限耗尽仍无终态时才报告
“结果未知”。
删除、覆盖、恢复出厂设置、OTA 和重启不得借 Actions 绕过单独的产品授权与安全设计。Actions
Provider 与 Settings/Status 一样在 `init_borrow()` 期间深复制元数据和回调，只长期借用
`context` 到 `deinit()` 成功。

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

DeskMate 当前 Hub URL 不是 Network Provider 配置项，而由 `app_network` 独占。候选只接受
ASCII `http://` authority：IPv4 或主机名、可选 `1..65535` 端口；scheme/host 规范化为小写，
移除唯一末尾 `/`，并拒绝用户信息、业务 path、query、fragment、空白、非 ASCII 和超过
127 bytes 的值。测试和保存共享一个 pending 槽，并与 Portal 的完整网络配置保存 reservation
互斥，防止两个入口交错覆盖 `network_cfg`。

上述 ASCII authority 是 Hub 产品字段的额外安全约束，由
`app_network_hub_url_parse_copy()` 和产品所有者校验保证；共享 Web Console 的通用 STRING
仍允许有效 UTF-8，例如番茄钟完成音乐逻辑路径 `/音乐/完成.mp3`。

测试只对候选的 `/healthz` 执行无凭据有界 GET，不写持久化；保存不能复用旧测试结果，而由
唯一 Network Task 对同一候选重测，然后读取完整 `network_cfg`、只替换 `service_url` 并一次性
提交单个 Blob。持久化成功后才发布新 URL/version，并立即使旧测试事实失效。随后远端日志按
stop/configure/start 最佳努力重配；失败只记录事实、不回滚地址。已经复制旧后端上下文的在途
事务继续完成，新 URL 只用于后续新事务。

番茄钟更新同样由领域所有者执行：Task 在锁内重检版本与空闲状态，锁外先持久化候选，成功后
才在短锁内发布新 settings/version；失败保留旧设置、版本、派生字段和原 `settings_saved`。

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

- Files 保持当前文件 URI、状态码、认证与事务语义；协议变化必须单独评审，不能借命名或移动
  代码静默改变客户端行为。
- 覆盖确认头使用产品无关的 `X-Web-Console-Overwrite`；HTML 标题、浏览器存储键和公共源码
  不得重新引入 DeskMate 产品名，产品差异只能通过初始化或构建配置显式注入。
- 当前 HTTPD 为局域网明文 HTTP。首版 Settings 不允许写入 Wi-Fi 密码、设备 Token 等 Secret；
  是否开放 Secret 写入必须单独确认传输安全策略。
- 所有认证后 API 默认 `Cache-Control: no-store`，不得记录访问码、Bearer token、密码或完整
  Authorization。
- Files、Settings、Status、Actions 和 Network Provider 的路由必须在启动前形成固定、有界、无冲突
  的表；停止时由 Core 对称注销。

## 7. 构建与裁剪验收

当前实现及后续维护必须验证以下构建组合：

| 组合 | 必须成立的事实 |
| --- | --- |
| Communication-only | 不发现或链接任何 Web Console 符号 |
| Console Core-only | 不依赖文件系统或 Communication，能够独立启停 |
| Core + Files | 保持现有文件 HTTP、安全和恢复契约 |
| Core + Settings | 使用内存 Provider 即可构建，不依赖产品存储 |
| Core + Actions | 只开放非破坏性异步操作，不携带 Files/Settings/Status 路由或网页片段 |
| Core + Network Provider | 只增加对 `network_manager` 的单向只读依赖 |
| DeskMate 完整组合 | 顶层“文件管理 / 设置 / 退出登录”；Application 继续拥有网络租约和启停时机 |

Python/Node/PowerShell 主机测试与静态检查只能证明源码、网页装配和 host helper 契约；它们
不等于固件编译、OTA 发布、设备安装、真实设备、真实 Hub 或浏览器交互验收。固件编译只能在
用户明确要求后通过 DeskSuite 根目录 `ds.ps1` 执行；编译通过也不代表 HTTP、SD、断网或停止
流程已经完成设备验收。

## 8. 当前维护边界

历史上的纯重命名、Core/Files 拆分、组件移动、Settings/Status Provider 和只读 Network
Provider 引入均已完成，不再构成当前操作步骤。后续维护遵守以下边界：

- `app_web_console` 与 `APP_NETWORK_LEASE_WEB_CONSOLE` 继续只属于 DeskMate 产品编排；共享
  Console 和 Provider 不得依赖这些产品符号，也不得重新引入 `web_file` 产品命名。
- DeskMate 因启用 Files 而保留 SD 前置检查；Settings、Status 或 Actions 的共享实现不得据此
  假设文件系统一定存在。
- 命名整理、文件移动、协议变化、行为重构和新增功能必须保持可审查边界；涉及公共 API、
  生命周期或跨模块术语时，先更新相应标准与契约，再按影响范围验证。
- 当前三客户分组、Hub/Pomodoro 所有权和“不含 Wi-Fi/OTA/重启”的产品边界只有在新的明确设计
  获批后才能改变，不能把历史迁移记录当作扩张授权。
