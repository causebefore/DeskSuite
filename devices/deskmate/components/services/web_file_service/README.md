# `web_file_service`

> `web_file_service` 是 Service 层的本地认证文件事务所有者，统一管理端口 80 HTTPD、单会话
> 凭据、URI 入口关闭、handler 排空和文件传输资源，不决定产品何时开放网页文件管理。

## 1. 定位

- 层级：Service。
- 进入理由：本组件独占 HTTPD、认证状态和文件传输共享资源，并为停止过程提供有界排空与失败
  终态。
- 触发方：由目标 Application 或 Composition Root 按产品时机调用生命周期 API。
- 主要输出：端口 80 的本地 HTTP 响应，以及可复制的服务运行摘要、会话和传输事实。

## 2. 职责边界

负责：

- 创建和停止 HTTPD，注册精确 URI，并为全部项目 handler 做进入/退出记账。
- 每次启动生成新的六位访问码，执行失败锁定、单会话 token 创建和空闲失效。
- 在停止时先拒绝新请求、清除秘密、关闭客户端，再由 HTTPD Task 注销 URI 并等待活动
  handler 排空，最后由一次性清理 Task 完成合法 HTTPD 销毁。
- 作为目录浏览、下载、事务式上传和短时文件变更 handler 的单传输状态及上传恢复 journal
  所有者。

不负责：

- 不启动 Wi-Fi、AP 或 Portal，不决定服务开放时机、产品降级、重试或重启策略。
- 不挂载或卸载 SD 卡，不调用下层存储组件的生命周期 API。
- 不把访问码嵌入 HTML 或写入日志；访问码如何在设备 UI 上呈现由上层决定。
- 不提供配置编辑、递归目录删除、目录移动/重命名、WebDAV、WebSocket、CORS、LRU 会话
  淘汰或长期后台轮询 Task。
- 不对目录项排序或在内存建立目录项数组；不为目录浏览分配 32 KiB 文件数据缓冲区。

## 3. 主要流程

端到端产品流程为：

```text
设备设置页选择“网页文件管理”
  → app_web_file 检查 /sdcard
  → app_network 授予 APP_NETWORK_LEASE_WEB_FILE
  → web_file_service 恢复事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → handler 串行浏览、下载、事务上传或执行单项文件变更
  → 设备返回时 Service 安全停止后释放网络租约
```

Service 只拥有从事务恢复、认证到 handler 排空的中间段；设备入口、链路变化、Presenter 和
退出门控都由 Application、Presentation 与 UI 各自负责。Application 只有在本组件确认 HTTPD、
handler、文件句柄和 PSRAM 缓冲区全部消失后才释放网络租约。

### 3.1 首页与认证

```text
GET /
    → handler 进入记账
    → 从 flash 发送 gzip 首页
    → handler 退出记账

POST /api/session（text/plain，正文恰好 6 字节）
    → handler 进入记账
    → 在认证状态副本上校验访问码
    → 校验成功后才生成 128 位随机 token
    → Service 锁内提交唯一会话
    → 返回 no-store JSON；响应失败时精确撤销本次会话
    → handler 退出记账
```

当前 HTTP 契约：

| 方法与 URI | 成功响应 | 安全约束 |
| --- | --- | --- |
| `GET /` | gzip 压缩的 `text/html; charset=utf-8` | `Cache-Control: no-store`、`X-Content-Type-Options: nosniff` |
| `POST /api/session` | `{"token":"<32 位小写十六进制>"}` | 仅接受以 `text/plain` 开头的 Content-Type 和 6 字节正文；响应 `no-store` |
| `GET /api/files?path=/...` | 目录容量和逐项 `entries` JSON | 精确 `Bearer ` token、严格单一 `path` query、双遍历后分块发送 |
| `GET /api/file?path=/...` | 常规文件下载 | 精确 `Bearer ` token、固定 `Content-Length` 原始分块、UTF-8 `filename*` |
| `PUT /api/file?path=/...` | `201 Created` 或覆盖时 `200 OK` | 原始请求体、500 MiB 上限、覆盖确认、1 MiB 空间余量和可恢复提交 |
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

### 3.2 文件数据流

文件 handler 的固定边界为：

```text
HTTP 请求
    → handler 记账和 Bearer token 授权
    → 原子取得唯一传输所有权并记录活动 socket
    → 私有路径内核完成一次百分号解码、UTF-8/保留路径校验和挂载点映射
    → 目录：首遍完整验证，再重开目录逐项流式发送 JSON
    → 下载：stat/fstat 复核后分配 32 KiB PSRAM，按固定长度响应头和原始正文 send-all
    → 上传：完整预检后创建 upload.part，接收、同步、复核并执行新建或覆盖提交
    → 文件变更：复核父目录、源类型、目标冲突或目录为空后执行单次 mkdir/rename/unlink/rmdir
    → HTTP 响应
    → 关闭文件或目录，释放 PSRAM 与单传输所有权并退出 handler 记账
```

Service 锁只保护认证、生命周期、活动 handler 数和传输所有权等短时内存状态。网络收发、文件
I/O、目录遍历、等待以及 HTTPD API 调用都在锁外执行。

目录浏览在任何成功响应头发送前验证全部可见项，包括名称 UTF-8、控制字符、文件
类型、`stat` 和单项 JSON 长度。第二遍不保存也不排序目录项；目录在两遍之间发生变化时，
第二遍仍会重复安全校验，失败即中止连接。逻辑根目录不暴露大小写变体的 `.deskmate-web`。

文件下载只接受常规文件。路径 `stat` 通过后，打开的流必须再次通过 `fstat(fileno(file))`
确认仍为常规文件且长度与预检一致，之后才发送响应。下载使用有界扩展名表选择 MIME，并构造
包含原始 `Content-Length` 和
`attachment; filename="download"; filename*=UTF-8''...` 的完整有界 HTTP/1.1 响应头。
响应头和每个 32 KiB 原始正文块都使用公开 `httpd_send()` 的 send-all 循环，处理部分发送，
不调用会自动添加 `Transfer-Encoding: chunked` 的 response chunk API；目录 JSON 仍使用
chunked。短读、客户端断开、发送失败或 Service 停止都会立即关闭文件，并由统一清理路径归还
传输缓冲区和活动 socket。

上传先完成 Bearer 授权与单传输占用，再严格读取 `Content-Length`、唯一 `path` query 和
大小写敏感的 `X-DeskMate-Overwrite: confirm`。目标父目录必须真实存在且可打开，目标只允许
不存在或常规文件；同名文件未确认时在接收正文前返回 `409`。空间检查使用
`system_filesystem_get_info_copy()`，要求可用字节至少为正文长度加 1 MiB；ESP-IDF v6.0.1
的 `statvfs()` 是固定返回 `ENOSYS` 的占位实现，因此不作为容量来源。全部预检通过后才创建
事务目录并分配共享 32 KiB PSRAM 缓冲区。

正文独占写入 `/sdcard/.deskmate-web/upload.part`，每块 `fwrite()` 必须完整消费；完成后依次
执行 `fflush()`、`fsync()`、关闭和精确长度复核。新文件直接把 `.part` 重命名到目标。覆盖
文件使用四行 UTF-8 journal，按 `PREPARED → BACKUP_MOVED → TARGET_COMMITTED` 推进：

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

目录创建、常规文件重命名/移动、常规文件删除和空目录删除与上传共用认证和唯一传输守卫，但
不分配 32 KiB PSRAM。每个请求只提交一个 FatFs 目录项变更：创建前复核父目录和目标不存在；
移动前复核源是常规文件、目标父目录存在且目标不存在；删除目录前完整遍历并只允许空目录。
根目录和路径内核保留的 `.deskmate-web` 始终拒绝修改。浏览器多选移动或删除通过顺序调用单项
接口实现，因此可能部分成功；失败项目会逐项报告，不声明跨多个文件的原子事务。

本组件的路径安全边界仅适用于固定 `SYSTEM_FILESYSTEM_MOUNT_POINT`（`/sdcard`）上的
ESP-IDF FatFs；FatFs 不支持符号链接，其 VFS `stat` 只将目录和常规文件报告为 `S_IFDIR`
或 `S_IFREG`。不得把本实现原样复用于支持符号链接的挂载点，因为当前 SDK 没有在本数据流中使用
`openat(O_NOFOLLOW)` / `fstatat()` / `fdopendir()` 建立逐段无跟随解析。

## 4. 依赖关系

| 方向 | 组件或运行时 | 用途 |
| --- | --- | --- |
| 公开依赖 | `esp_common` | 公共 API 的 `esp_err_t` |
| 私有调用 | `esp_http_server` | HTTPD、URI handler、客户端关闭和同步停止 |
| 私有调用 | `esp_system` | `esp_fill_random()` 生成访问码和 token |
| 私有调用 | `esp_timer` | 登录锁定、会话空闲失效和停止期限的单调时间 |
| 私有调用 | `freertos` | Service 状态锁、完成信号量和一次性 HTTPD 清理 Task |
| 私有调用 | `heap` | 分配内部 RAM 请求工作区和共享 32 KiB PSRAM 传输缓冲区 |
| 私有调用 | `sys` | 映射固定 SD 挂载点并查询文件系统总容量和可用容量 |
| 被调用 | 目标 Application / Composition Root | 按产品时机装配生命周期并读取运行摘要 |

本组件不初始化这些依赖，也不拥有 SD 卡或网络链路的生命周期。

## 5. 公共接口

公共头文件：[`include/web_file_service.h`](include/web_file_service.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `web_file_service_init()` | 同步 | 创建固定锁与三个完成信号量，不访问 SD 卡、不启动 HTTPD |
| `web_file_service_start()` | 同步 | 先恢复残留上传事务，再生成新访问码并在全部 handler 注册成功后进入 `RUNNING` |
| `web_file_service_stop(timeout_ms)` | 同步有界等待 | 三阶段共用总期限；成功时 HTTPD、清理 Task 和运行期资源已释放 |
| `web_file_service_get_status_copy()` | 同步 | 在短锁内复制完整有界快照，不返回内部指针 |
| `web_file_service_deinit()` | 同步 | 仅在 HTTPD、handler、传输和缓冲区全部消失后释放固定同步资源 |

`web_file_service_status_t.access_code` 是为上层本地呈现而提供的秘密副本，仅在运行态非空；调用方
不得记录或远程转发它。

## 6. 状态、生命周期与并发

```text
UNINITIALIZED
    └─ init ─→ INITIALIZED
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
- 每个 URI handler 无论成功、拒绝还是发送失败，都配对调用
  `web_file_handler_enter()` / `web_file_handler_leave()`；停止期拒绝路径不再发送响应。
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

- 固定 HTTP 端口：`80`。
- `max_uri_handlers = 8`，当前注册 `GET /`、`POST /api/session`、`GET /api/files`、
  `GET/PUT/PATCH/DELETE /api/file` 和 `PUT /api/directory`。
- `max_open_sockets = 1`；HTTPD 另行占用 listen 和两个控制 socket。
- recv/send timeout：各 5 秒；LRU purge 关闭；不注册 WebSocket。
- 构建配置：无 Kconfig 开关。
- [`src/web_file_service.cpp`](src/web_file_service.cpp)：生命周期、HTTPD 句柄和停止资源所有权。
- [`src/web_file_service_http.cpp`](src/web_file_service_http.cpp)：首页、认证会话、URI 注册与入口关闭。
- [`src/web_file_service_stop_task.cpp`](src/web_file_service_stop_task.cpp)：一次性 HTTPD 销毁
  Task 及无界 SDK 调用隔离。
- [`src/web_file_service_auth.cpp`](src/web_file_service_auth.cpp)：访问码锁定、单会话和 token 内核。
- [`src/web_file_service_path.cpp`](src/web_file_service_path.cpp)：路径、JSON 和响应头编码安全内核。
- [`src/web_file_service_transfer.cpp`](src/web_file_service_transfer.cpp)：共享鉴权传输守卫与原始
  PUT 接收。
- [`src/web_file_service_read.cpp`](src/web_file_service_read.cpp)：目录双遍历 JSON 与 32 KiB
  PSRAM 流式下载。
- [`src/web_file_service_mutation.cpp`](src/web_file_service_mutation.cpp)：目录创建、常规文件移动和删除。
- [`src/web_file_service_transaction.cpp`](src/web_file_service_transaction.cpp)：固定上传产物、
  journal 持久化、提交顺序与启动恢复矩阵。
- [`src/web_file_service_internal.hpp`](src/web_file_service_internal.hpp) 和
  [`src/web_file_service_transfer.hpp`](src/web_file_service_transfer.hpp)：组件私有 C++ 状态与协作接口。
- [`src/web_file_service_web.h`](src/web_file_service_web.h)：生成的 gzip 首页符号声明。

Service 手写实现均以 C++ 编译；构建期生成的 `web_file_index.generated.c` 只承载只读 gzip
字节资源，并通过带 `extern "C"` 的符号声明与 C++ 实现连接。

## 9. 验证

README 不记录某次任务的构建结果、固件大小或尚未执行的临时状态。变更应按影响范围完成以下
核查：

- 静态与主机侧：覆盖路径解码、认证、单传输守卫、上传事务恢复、创建/移动/删除约束、生成网页
  语法和 C/C++ ABI 边界。
- 固件：仅在用户明确要求时，从 DeskSuite 根目录执行统一命令
  `& .\ds.ps1 build deskmate`；不得绕过脚本调用下层构建工具。
- 实机：覆盖登录互斥、中英文和特殊名称、下载/上传完整性与边界大小、空间不足、覆盖恢复、
  创建/移动/删除约束、断网/掉电、安全停止和 STA 重连。
- 资源：长传输期间记录内部堆与 PSRAM 的当前/历史最低空闲量；离开页面后确认 HTTPD Task、
  socket、文件句柄、PSRAM、秘密和网络租约均已释放。

静态核查或编译成功都不等同于目标设备验收通过。
