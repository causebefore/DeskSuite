# `web_console_service`

> `web_console_service` 是 Service 层的可移植本地认证网页控制台。Console Core 始终提供
> HTTPD、单会话凭据、能力发现、URI 入口关闭与 handler 排空；Files、Settings、Status
> 是彼此可裁剪的可选模块。产品通过 Storage / Settings / Status Provider 注入能力，组件
> 不决定控制台何时开放，也不拥有产品配置、状态或持久化。

## 1. 定位

- 层级：Service。
- 进入理由：本组件独占 HTTPD、认证状态、路由集合和可选文件传输共享资源，并为停止过程提供
  有界排空与失败终态。
- 触发方：由目标 Application 或 Composition Root 按产品时机调用生命周期 API。
- 主要输出：配置端口上的本地 HTTP 响应，以及可复制的服务运行摘要。

## 2. 职责边界

负责：

- Console Core 创建和停止 HTTPD，聚合精确领域路由，并通过统一 dispatcher 为全部项目
  handler 做进入/退出记账。
- Console Core 每次启动生成新的六位访问码，执行失败锁定、单会话 token 创建和空闲失效。
- Console Core 只向认证会话公开本次构建且实际装配的模块、分区和字段元数据。
- Console Core 在停止时先拒绝新请求、清除秘密、关闭客户端，再由 HTTPD Task 逐路由注销
  URI 并等待活动 handler 排空，最后由一次性清理 Task 完成合法 HTTPD 销毁。
- 内部 Files 模块拥有目录浏览、下载、事务式上传和短时文件变更 handler，以及单传输状态、
  PSRAM 缓冲区和上传恢复 journal。
- 内部 Settings/Status HTTP 映射负责通用 JSON 编解码、字段类型/范围/访问属性校验、版本化
  异步更新协议和 Provider 输出契约校验；领域所有者继续负责语义校验、排队、持久化、生效和
  最终结果。

Core 与可选模块通过组件私有的领域路由描述协作。模块不注册或注销 `httpd_uri_t`，也不重复
做 handler 记账；Core 不直接持有文件传输状态或产品领域状态。构建脚本从唯一公共页面壳装配
已开启模块的 HTML/CSS/JS；关闭某模块时，其领域源码、路由和网页片段都不进入固件。

不负责：

- 不启动 Wi-Fi、AP 或 Portal，不决定服务开放时机、产品降级、重试或重启策略。
- 不挂载或卸载存储，不调用存储 Provider 的生命周期 API。
- 不把访问码嵌入 HTML 或写入日志；访问码如何在设备 UI 上呈现由上层决定。
- 不定义产品设置、不直接访问产品 NVS、不执行 Wi-Fi 重连/设备重启等领域动作，也不采集
  产品状态；这些事实全部来自注入 Provider。
- 不提供递归目录删除、目录移动/重命名、WebDAV、WebSocket、CORS、LRU 会话淘汰或长期
  后台轮询 Task。
- 不对目录项排序或在内存建立目录项数组；不为目录浏览分配 32 KiB 文件数据缓冲区。

## 3. 主要流程

端到端产品流程为：

```text
产品入口请求启动网页控制台
  → Application 检查可选存储并取得产品网络租约
  → Composition Root 用端口和本产品实际拥有的 Provider 初始化 Service
  → web_console_service 恢复可选 Files 事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → 浏览器读取 capabilities，只呈现实际装配模块
  → handler 读取状态、提交版本化设置更新，或执行可选文件操作
  → 设备返回时 Service 安全停止后释放网络租约
```

Service 只拥有从事务恢复、认证到 handler 排空的中间段；设备入口、链路变化、Presenter 和
退出门控都由 Application、Presentation 与 UI 各自负责。Application 只有在本组件确认 HTTPD、
handler、文件句柄和 PSRAM 缓冲区全部消失后才释放网络租约。

### 3.1 首页与认证

```text
GET /
    → Core dispatcher 进入记账
    → 从 flash 发送 gzip 首页
    → Core dispatcher 退出记账

POST /api/session（text/plain，正文恰好 6 字节）
    → Core dispatcher 进入记账
    → 在认证状态副本上校验访问码
    → 校验成功后才生成 128 位随机 token
    → Service 锁内提交唯一会话
    → 返回 no-store JSON；响应失败时精确撤销本次会话
    → Core dispatcher 退出记账

DELETE /api/session（Bearer token）
    → Core 锁内原子校验并清除当前 token
    → 保留本次运行访问码，返回 204，允许重新登录

GET /api/capabilities（Bearer token）
    → 返回本次构建且实际装配的模块、分区和字段元数据
```

当前 HTTP 契约：

| 方法与 URI | 成功响应 | 安全约束 |
| --- | --- | --- |
| `GET /` | gzip 压缩的 `text/html; charset=utf-8` | `Cache-Control: no-store`、`X-Content-Type-Options: nosniff` |
| `POST /api/session` | `{"token":"<32 位小写十六进制>"}` | 仅接受以 `text/plain` 开头的 Content-Type 和 6 字节正文；响应 `no-store` |
| `DELETE /api/session` | `204 No Content` | 精确 Bearer token；在 Core 锁内原子关闭当前会话 |
| `GET /api/capabilities` | 构建模块及实际 Provider 元数据 | 精确 Bearer token；秘密字段只公开访问属性 |
| `GET /api/settings?section=...` | 分区版本和完整公开值 | Provider 回调在 Core 锁外；版本编码为十进制字符串 |
| `PATCH /api/settings?section=...` | `202 Accepted` 及请求 ID | 2 KiB JSON 上限；通用校验后由所有者复制并异步受理 |
| `GET /api/settings/result?section=...&request=...` | `202` pending 或 `200` 终态 | 请求 ID 与版本均使用无精度损失的十进制字符串 |
| `GET /api/status?section=...` | 一个只读分区摘要 | Provider 回调只能做有界事实读取和短时所有者加锁 |
| `GET /api/files?path=/...` | 目录容量和逐项 `entries` JSON | 精确 `Bearer ` token、严格单一 `path` query、双遍历后分块发送 |
| `GET /api/file?path=/...` | 常规文件下载 | 精确 `Bearer ` token、固定 `Content-Length` 原始分块、UTF-8 `filename*` |
| `PUT /api/file?path=/...` | `201 Created` 或覆盖时 `200 OK` | 原始请求体、配置上限、覆盖确认、配置空间余量和可恢复提交 |
| `PUT /api/directory?path=/...` | `201 Created` | 目标必须不是根目录、不能已存在，父目录必须真实存在 |
| `PATCH /api/file?path=/...&destination=/...` | `200 OK` | 仅移动或重命名常规文件；目标不得存在且父目录必须存在 |
| `DELETE /api/file?path=/...` | `200 OK` | 只删除常规文件或空目录；禁止根目录、保留目录和递归删除 |

`path` query 只允许携带一个百分号编码的逻辑路径。路径内核仅解码一次，并要求解码结果是以
`/` 开头的绝对逻辑路径；因此分隔符既可原样传输，也可由浏览器编码为 `%2F`。
`PATCH` 是唯一例外，其 query 必须严格按 `path=<源>&destination=<目标>` 顺序携带两个各自
百分号编码的逻辑路径，字段值中的 `&` 必须编码为 `%26`。

会话创建把格式错误映射为 `400`、错误访问码映射为 `401`、已有未过期会话映射为 `409`、登录
锁定映射为 `423`。响应不添加 CORS 头。十分钟空闲从最近一次成功授权或完整传输结束时重新
起算；已有传输期间到达的并发请求不会因起始授权时间已超过十分钟而清除同一会话，而是先完成
token 校验再返回单传输忙。停止流程已清空的会话不会被传输释放路径重新创建或恢复。

### 3.2 Settings 与 Status 数据流

```text
初始化
  → 校验分区/字段 ID、UTF-8 标签、类型、范围、访问属性和回调
  → 按实际装配量深复制字符串、字段和枚举元数据
  → 仅长期借用 Provider context

读取
  → Core 鉴权并释放 Core 锁
  → 调用所有者 get_*_copy 写入 Console 提供的有界数组
  → 校验 Provider 未替换缓冲区、字段数/类型/范围/UTF-8/秘密脱敏完整
  → JSON 编码并发送

更新
  → 严格解析 expectedVersion + changes
  → 通用类型、范围、步长、枚举、只读和秘密约束校验
  → 所有者 validate_update 做无副作用早期校验
  → 所有者 request_update_copy 复制请求并重新检查版本或在执行点原子重检
  → 返回非零单调 requestId，浏览器查询 pending / succeeded / failed
```

Settings Provider 必须拥有独立、严格递增且不回绕的设置版本。`validate_update` 不是提交保证；
真正修改前必须再次比较 `expected_version`，冲突不得覆盖新设置。请求 ID 在同一 Provider
context 生命周期内不得复用；最终结果至少保留到下一请求被接受。Secret 首版 HTTP 只允许
报告是否已配置，不读取也不写入秘密原值。

Provider 回调运行在 HTTPD 普通 Task、Core 锁外，只能做有界内存读取、短时所有者加锁或快速
排队；不得执行网络、文件、NVS 等长 I/O，不得长期等待，也不得回调 Console。Console 不创建
轮询 Task，异步状态查询由已登录浏览器发起。

### 3.3 文件数据流

文件 handler 的固定边界为：

```text
HTTP 请求
    → Core dispatcher 记账
    → Files 完成 Bearer token 授权
    → 原子取得唯一传输所有权并记录活动 socket
    → 私有路径内核完成一次百分号解码、UTF-8/保留路径校验和挂载点映射
    → 目录：首遍完整验证，再重开目录逐项流式发送 JSON
    → 下载：stat/fstat 复核后分配 32 KiB PSRAM，按固定长度响应头和原始正文 send-all
    → 上传：完整预检后创建 upload.part，接收、同步、复核并执行新建或覆盖提交
    → 文件变更：复核父目录、源类型、目标冲突或目录为空后执行单次 mkdir/rename/unlink/rmdir
    → HTTP 响应
    → 关闭文件或目录，释放 PSRAM 与单传输所有权
    → Core dispatcher 退出 handler 记账
```

Service 锁只保护认证、生命周期、活动 handler 数和传输所有权等短时内存状态。网络收发、文件
I/O、目录遍历、等待以及 HTTPD API 调用都在锁外执行。

目录浏览在任何成功响应头发送前验证全部可见项，包括名称 UTF-8、控制字符、文件
类型、`stat` 和单项 JSON 长度。第二遍不保存也不排序目录项；目录在两遍之间发生变化时，
第二遍仍会重复安全校验，失败即中止连接。逻辑根目录不暴露大小写变体的配置工作区名称。

文件下载只接受常规文件。路径 `stat` 通过后，打开的流必须再次通过 `fstat(fileno(file))`
确认仍为常规文件且长度与预检一致，之后才发送响应。下载使用有界扩展名表选择 MIME，并构造
包含原始 `Content-Length` 和
`attachment; filename="download"; filename*=UTF-8''...` 的完整有界 HTTP/1.1 响应头。
响应头和每个 32 KiB 原始正文块都使用公开 `httpd_send()` 的 send-all 循环，处理部分发送，
不调用会自动添加 `Transfer-Encoding: chunked` 的 response chunk API；目录 JSON 仍使用
chunked。短读、客户端断开、发送失败或 Service 停止都会立即关闭文件，并由统一清理路径归还
传输缓冲区和活动 socket。

上传先完成 Bearer 授权与单传输占用，再严格读取 `Content-Length`、唯一 `path` query 和
大小写敏感的 `X-Web-Console-Overwrite: confirm`。目标父目录必须真实存在且可打开，目标只
允许不存在或常规文件；同名文件未确认时在接收正文前返回 `409`。空间检查在 Service 锁外
调用 Storage Provider 的容量快照回调，要求可用字节至少为正文长度加配置保留量。全部预检
通过后才创建事务目录并分配共享 32 KiB PSRAM 缓冲区。

正文独占写入“配置挂载根/配置工作区/upload.part”，每块 `fwrite()` 必须完整消费；完成后
依次执行 `fflush()`、`fsync()`、关闭和精确长度复核。新文件直接把 `.part` 重命名到目标。
覆盖文件使用四行 UTF-8 journal，按 `PREPARED → BACKUP_MOVED → TARGET_COMMITTED` 推进：

```text
同步 upload.part
    → 同步 PREPARED journal
    → 目标重命名为 upload.bak
    → 同步 BACKUP_MOVED journal
    → upload.part 重命名为目标并复核长度
    → 同步 TARGET_COMMITTED journal
    → 删除 upload.bak 和 journal
```

journal 只包含版本、阶段、预期长度和规范逻辑目标路径。每次更新先完整同步
`transaction.new`；由于 FatFs 的 rename 不覆盖现有名称，再移除旧 journal 并切换新记录。
持久切换和恢复清理存在五个明确掉电窗口：

1. 写入或同步 `transaction.new` 期间：旧 `transaction` 保持不变；若新记录不完整或与旧记录
   冲突，恢复拒绝启动。
2. 新记录已同步、旧记录尚未移除：两份记录必须具有相同目标和长度，且阶段相同或只前进一级；
   `PREPARED → BACKUP_MOVED` 使用旧记录做保守回滚，`BACKUP_MOVED → TARGET_COMMITTED`
   使用已验证的后继记录，相同阶段可使用任一记录。
3. 旧记录已移除、新记录尚未重命名：仅存在的 `transaction.new` 必须完整通过解析，并作为
   当前阶段恢复。
4. 新记录已重命名：仅使用新的 `transaction` 恢复。
5. 双记录恢复已接受 `TARGET_COMMITTED` 并删除 `.bak`，但尚未清理 journal 时再次掉电：
   下次启动仍选择后继 `TARGET_COMMITTED`；只要目标长度精确匹配且 `.part` 缺失，无论
   `.bak` 已删除或仍存在都可幂等接受提交。

双记录的格式、目标、长度或阶段关系任一不一致都会拒绝启动。接收失败、短写、同步失败和提交
前取消只清理 Service 自有 `.part`；覆盖提交失败优先恢复 `.bak`，绝不先删除请求目标来腾出
空间。

挂载文件系统启用打开文件锁时，删除、移动或覆盖正被其他组件打开的目标会把 `EBUSY` 映射为
`409 Conflict` 与 `file_in_use`（“文件正在使用”）。覆盖路径只有在恢复内核已经删除 `.part`
和 journal、并确认原文件仍保留后才返回该业务冲突；恢复失败仍按文件系统错误处理并保留可
恢复现场。

目录创建、常规文件重命名/移动、常规文件删除和空目录删除与上传共用认证和唯一传输守卫，但
不分配 32 KiB PSRAM。每个请求只提交一个 VFS 目录项变更：创建前复核父目录和目标不存在；
移动前复核源是常规文件、目标父目录存在且目标不存在；删除目录前完整遍历并只允许空目录。
根目录和路径内核保留的配置工作区始终拒绝修改。浏览器多选移动或删除通过顺序调用单项接口
实现，因此可能部分成功；失败项目会逐项报告，不声明跨多个文件的原子事务。

调用方必须注入绝对、规范且不带结尾斜杠的 VFS 挂载根，并保证该文件系统不支持符号链接，
或以其他方式提供等价的无跟随保证。当前路径内核没有使用 `openat(O_NOFOLLOW)` /
`fstatat()` / `fdopendir()` 建立逐段无跟随解析，因此不得把本实现直接接到可创建符号链接的
挂载点。挂载根还必须在 Service 完成反初始化前保持稳定；调用方必须串行化 Console 外部
写入，防止新建或移动操作在“确认目标不存在”与 `rename()` 之间被其他写入者抢占。Files
依赖这一独占写入契约实现 no-replace 语义，不能仅凭某个 VFS 的 `rename()` 默认行为保证。

## 4. 依赖关系

| 方向 | 组件或运行时 | 用途 |
| --- | --- | --- |
| 公开依赖 | `esp_common` | 公共 API 的 `esp_err_t` |
| 私有调用 | `esp_http_server` | HTTPD、URI handler、客户端关闭和同步停止 |
| 私有调用 | `esp_system` | `esp_fill_random()` 生成访问码和 token |
| 私有调用 | `esp_timer` | 登录锁定、会话空闲失效和停止期限的单调时间 |
| 私有调用 | `freertos` | Service 状态锁、完成信号量和一次性 HTTPD 清理 Task |
| 可选私有调用 | `heap` | Files 开启时分配内部 RAM 请求工作区和共享 32 KiB PSRAM 传输缓冲区 |
| 组件清单依赖 | `espressif__cjson` | Settings/Status 开启时才编译并链接，用于严格解析和有界编码 JSON |
| 可选调用方 | [`web_console_network_provider`](../web_console_network_provider/README.md) | 通过公共 Status 契约提供 Network Manager 只读诊断；Core 不反向依赖 |
| 被调用 | 目标 Application / Composition Root | 按产品时机装配生命周期并读取运行摘要 |

本组件不初始化这些依赖，也不拥有存储 Provider 或网络链路的生命周期。Files 只使用 C/POSIX
文件 API 和注入的容量快照，不依赖产品 `sys` 组件。

ESP-IDF Component Manager 在 Kconfig 求值前解析组件清单，因此可移植包始终声明并解析
`espressif/cjson`；关闭 Settings/Status 会裁掉其源码、头文件路径和链接依赖，但不承诺省略
依赖解析/下载。若宿主要求连依赖解析也裁掉，应把 Provider HTTP 映射拆成单独发现的组件，
而不是让 Core 静默依赖宿主已有的 JSON 组件。

## 5. 公共接口

公共头文件：[`include/web_console_service.h`](include/web_console_service.h)、
[`include/web_console_provider.h`](include/web_console_provider.h)、
[`include/web_console_files.h`](include/web_console_files.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `web_console_service_init_borrow(config)` | 同步 | 创建固定同步资源，按实际数量深复制 Provider 元数据并借用 context |
| `web_console_service_start()` | 同步 | 先恢复可选 Files 残留事务，再生成新访问码并在全部 handler 注册成功后进入 `RUNNING` |
| `web_console_service_stop(timeout_ms)` | 同步有界等待 | 三阶段共用总期限；成功时 HTTPD、清理 Task 和运行期资源已释放 |
| `web_console_service_get_status_copy()` | 同步 | 在短锁内复制完整有界快照，不返回内部指针 |
| `web_console_service_deinit()` | 同步 | 仅在 HTTPD、handler、传输和缓冲区全部消失后释放固定同步资源 |

`web_console_service_status_t.access_code` 是为上层本地呈现而提供的秘密副本，仅在运行态非空；调用方
不得记录或远程转发它。配置中的字符串及回调函数指针在初始化期间复制；Provider `context`
由调用方持有并必须保持有效，直到 `web_console_service_deinit()` 成功。

## 6. 状态、生命周期与并发

```text
UNINITIALIZED
    └─ init_borrow ─→ INITIALIZED
                   └─ start ─→ STARTING ─→ RUNNING
                                              └─ stop ─→ STOPPING ─→ INITIALIZED
INITIALIZED ── deinit ─→ UNINITIALIZED
STARTING / STOPPING ── URI 或 HTTPD 清理失败 ─→ CLEANUP_FAILED
CLEANUP_FAILED ── 后续 stop 成功 ─→ INITIALIZED
```

- 普通启动失败在资源完整回滚后回到 `INITIALIZED`。
- 入口关闭、handler 排空或 HTTPD 清理 Task 等待超时保留 `STOPPING`；启动失败回滚超时以及
  工作排队、URI 注销、Task 创建或 HTTPD 清理失败保留 `CLEANUP_FAILED`。两者都拒绝
  `start()`，但允许后续 `stop()` 继续等待同一清理所有者或重试。
- `lifecycle_active` 防止两个生命周期操作同时使用同一 HTTPD 句柄。
- 全部 URI 由 Core 的统一 dispatcher 完成准入检查和 handler 记账；无论领域 handler 成功、
  拒绝还是发送失败，RAII 记账范围都会配对退出，停止期拒绝路径不再发送响应。
- Provider 注册表只在 `init_borrow()` 配置、在 handler 全部排空后的 `deinit()` 清理；HTTP
  handler 期间元数据地址稳定，所有 Provider 回调均在 Core 锁外。
- HTTPD 配置只允许一个客户端会话，认证内核只允许一个未过期会话，文件层只允许一个活动
  传输。
- `GET /api/files`、`GET/PUT/PATCH/DELETE /api/file` 和 `PUT /api/directory` 共用同一传输
  守卫；鉴权先于 query 和文件系统访问，未认证请求无法借错误差异推断路径是否存在。
- 活动传输期间的鉴权把单传输事实传给认证内核，因此长传输不会被并发请求误判为空闲过期；
  传输释放在 Service 锁内以完成时刻刷新仍存在的会话，只单调推进活动时间，停止已清空会话时
  保持清空。
- 下载和上传缓冲区只以 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` 精确分配 32 KiB，不进入 HTTPD
  Task 栈或普通内部 RAM；目录浏览和短时文件变更只使用有界内部 RAM 工作区。
- HTTPD 自己拥有执行 URI handler 的 Task。本组件只在销毁阶段创建一个栈 3072 字节、
  优先级 4 的一次性清理 Task，用于隔离 ESP-IDF `httpd_stop()` 的无超时等待；Task 发布结果
  后挂起，由当前或后续生命周期调用显式删除。本组件不创建 Timer、Queue 或周期轮询 Task。

## 7. 秘密、停止与故障恢复

- 每次 `start()` 重新生成访问码；只有服务进入 `RUNNING` 后才发布到运行摘要。
- token 只在访问码验证成功后生成，通过 `no-store` JSON 返回，并以二进制形式保存在认证状态；
  若响应构造或发送失败，仅在当前 token 仍匹配时撤销本次会话。
- `stop()` 进入 `STOPPING` 的同一锁范围内立即清空访问码、token、锁定计数和会话时间。
- `stop(timeout_ms)` 从入口计算总期限，依次约束 HTTPD Task 注销项目 URI、活动 handler
  排空和一次性清理 Task 完成；HTTPD 的 recv/send 等待各限制为 5 秒。停止方先建立不会再
  进入项目 handler 的稳定准入屏障，屏障或 handler 未排空时不启动销毁，也不释放仍可能使用
  的缓冲区。
- 入口关闭且 handler 排空后先释放传输缓冲区，再由一次性 Task 独占服务器句柄并调用
  `httpd_stop()`。ESP-IDF 内部等待无上限，但公共 `stop()` 只等待总期限；超时不会强杀 Task、
  删除完成信号或重用 HTTPD 句柄。后续 `stop()` 等待同一 Task，收取结果并显式回收 Task。
- 启动失败回滚复用同一机制并共享固定六秒总期限。超时或工作排队、URI 注销、Task 创建、
  HTTPD 清理失败时保留可恢复所有权，上层可以决定何时重试 `stop()` 或请求重启。
- `start()` 在生成访问码和启动 HTTPD 前扫描固定事务目录：唯一 `.part` 或唯一
  `transaction.new` 只有经 `stat` 确认为常规文件后才清理，目录和其他非文件类型会阻止
  启动；`PREPARED` 保留原目标并丢弃未提交暂存；`BACKUP_MOVED` 在目标缺失且 `.bak`
  存在时恢复备份，目标存在时只有 `.bak` 仍存在、`.part` 缺失且长度精确匹配才接受提交；
  `.bak` 缺失属于协议不能证明安全的状态并拒绝启动；`TARGET_COMMITTED` 同样只接受目标
  长度匹配且 `.part` 缺失，目标缺失且 `.bak` 存在时恢复备份。长度不匹配、malformed
  journal、未知目录项、非法目标类型
  或互相冲突的产物集合会阻止启动，不猜测或删除用户文件。
- 上传在每个接收块边界和每个 durable 重命名阶段间检查停止状态。提交前取消会清理 `.part`；
  目标已经换入后会先完成 `TARGET_COMMITTED` 的可恢复阶段和元数据清理，再向停止流程报告
  取消。
- 产品级是否降级、重试、隐藏入口或请求重启仍由 Application 决定。

## 8. 配置与文件

- HTTP 端口：由 `web_console_service_config_t.server_port` 注入。
- 固定 Core 路由 4 条；Files 6 条；Settings 3 条；Status 1 条。完整构建共 14 条，关闭模块
  时 `max_uri_handlers` 与实际路由数同步缩减。
- `max_open_sockets = 1`；HTTPD 另行占用 listen 和两个控制 socket。
- recv/send timeout：各 5 秒；LRU purge 关闭；不注册 WebSocket。
- 构建配置：`CONFIG_WEB_CONSOLE_FILES` 默认开启；`CONFIG_WEB_CONSOLE_SETTINGS` 与
  `CONFIG_WEB_CONSOLE_STATUS` 默认关闭。构建始终使用 `web/index.html` 唯一公共壳，只把
  已开启模块的 `web/modules/` 片段组装到 gzip 资源中。
- [`src/core/web_console_service.cpp`](src/core/web_console_service.cpp)：生命周期、HTTPD 句柄和停止资源所有权。
- [`src/core/web_console_service_http.cpp`](src/core/web_console_service_http.cpp)：首页、认证会话、静态
  路由槽、统一 dispatcher 与入口关闭。
- [`src/core/web_console_http_common.cpp`](src/core/web_console_http_common.cpp)：认证后路由共享的
  Bearer 校验、会话关闭和 no-store/nosniff JSON 响应。
- [`src/core/web_console_service_stop_task.cpp`](src/core/web_console_service_stop_task.cpp)：一次性 HTTPD 销毁
  Task 及无界 SDK 调用隔离。
- [`src/core/web_console_service_auth.cpp`](src/core/web_console_service_auth.cpp)：访问码锁定、单会话和 token 内核。
- [`src/files/web_console_files.cpp`](src/files/web_console_files.cpp)：Files 路由、传输上下文与启动/清理钩子。
- [`src/files/web_console_service_path.cpp`](src/files/web_console_service_path.cpp)：路径、JSON 和响应头编码安全内核。
- [`src/files/web_console_service_transfer.cpp`](src/files/web_console_service_transfer.cpp)：共享鉴权传输守卫与原始
  PUT 接收。
- [`src/files/web_console_service_read.cpp`](src/files/web_console_service_read.cpp)：目录双遍历 JSON 与 32 KiB
  PSRAM 流式下载。
- [`src/files/web_console_service_mutation.cpp`](src/files/web_console_service_mutation.cpp)：目录创建、常规文件移动和删除。
- [`src/files/web_console_service_transaction.cpp`](src/files/web_console_service_transaction.cpp)：固定上传产物、
  journal 持久化、提交顺序与启动恢复矩阵。
- [`src/providers/web_console_provider_registry.cpp`](src/providers/web_console_provider_registry.cpp)：
  Provider 元数据校验、按量深复制、发现与释放。
- [`src/providers/web_console_provider_http.cpp`](src/providers/web_console_provider_http.cpp)：Capabilities、
  Settings/Status 的认证 HTTP/JSON 映射和 Provider 输出契约检查。
- [`scripts/build_html.py`](scripts/build_html.py) 与 [`scripts/test_build_html.py`](scripts/test_build_html.py)：
  确定性模块装配、gzip 生成及全部八种裁剪组合测试。
- [`src/core/web_console_service_internal.hpp`](src/core/web_console_service_internal.hpp) 和
  [`src/files/web_console_files_internal.hpp`](src/files/web_console_files_internal.hpp)：Core/Files
  私有状态、领域路由与生命周期协作接口。
- [`src/files/web_console_service_transfer.hpp`](src/files/web_console_service_transfer.hpp)：Files 传输私有类型。
- [`src/core/web_console_service_web.h`](src/core/web_console_service_web.h)：生成的 gzip 首页符号声明。
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)：随组件移植的 Crosslink 页面许可声明。

Service 手写实现均以 C++ 编译；构建期生成的 `web_console_index.generated.c` 只承载只读 gzip
字节资源，并通过带 `extern "C"` 的符号声明与 C++ 实现连接。

## 9. 验证

README 不记录某次任务的构建结果、固件大小或尚未执行的临时状态。变更应按影响范围完成以下
核查：

- 静态与主机侧：覆盖路径解码、认证、单传输守卫、上传事务恢复、创建/移动/删除约束、八种
  模块网页组合、Provider 元数据/输出/JSON 边界和 C/C++ ABI。
- 固件：仅在用户明确要求时，从 DeskSuite 根目录执行统一命令
  `& .\ds.ps1 build deskmate`；不得绕过脚本调用下层构建工具。
- 实机：覆盖登录/退出/重新登录、Capabilities 裁剪、设置版本冲突和异步终态、状态读取、
  中英文和特殊名称、下载/上传完整性与边界大小、空间不足、覆盖恢复、创建/移动/删除约束、
  断网/掉电、安全停止和 STA 重连。
- 资源：长传输期间记录内部堆与 PSRAM 的当前/历史最低空闲量；离开页面后确认 HTTPD Task、
  socket、文件句柄、PSRAM、秘密和网络租约均已释放。

静态核查或编译成功都不等同于目标设备验收通过。
