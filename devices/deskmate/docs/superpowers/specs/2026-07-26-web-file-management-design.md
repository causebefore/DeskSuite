# DeskMate 网页文件管理设计

> 状态：已由用户确认，日期：2026-07-26。
>
> 本设计只覆盖局域网内按需启停的 SD 卡网页文件管理。配置编辑、WebDAV、WebSocket、
> 文件删除与目录修改不在本阶段范围内。

## 1. 目标与范围

DeskMate 在连接现有 Wi-Fi 后，允许用户从设备设置菜单手动开启网页文件管理。用户通过设备
屏幕显示的局域网地址和一次性 6 位访问码，在手机或电脑浏览器中访问完整 `/sdcard`，完成
目录浏览、文件下载和文件上传。

本阶段必须满足：

- 网页服务只由设备端用户手动开启，退出设备页面后关闭。
- 服务运行期间保持 Wi-Fi 在线并阻止轻睡眠。
- 网页逻辑根目录对应完整 `/sdcard`，所有路径必须限制在该挂载点内。
- 支持中文、空格和常见特殊字符文件名，FatFs API 使用 UTF-8 编码。
- 单个上传文件最大为 500 MiB。
- 文件全程流式传输，不把完整文件载入内存。
- 同时只允许一个已认证浏览器会话和一个文件传输。
- 同名文件必须由浏览器明确确认后才能覆盖。
- 传输失败、断网或设备重启不得破坏原有同名文件。

本阶段不包含：

- Wi-Fi、服务地址、Token、OTA 或其他配置编辑。
- 删除、移动、重命名、复制文件以及新建目录。
- WebDAV、WebSocket、ZIP 打包下载和浏览器端图片转换。
- SoftAP 热点模式、配网页面合并或常驻后台服务。
- 二维码入口和公网访问。

## 2. 现有基础与参考项目边界

DeskMate 当前已经具备：

- `device_storage` 提供与板型无关的 SD 块设备能力。
- `system_filesystem` 把 FAT 文件系统挂载到 `/sdcard`。
- `connect` 的配网 Portal 已使用 ESP-IDF `esp_http_server`。
- `app_network` 拥有联网时机、网络租约、Portal、Dashboard、OTA 与轻睡眠停网策略。
- UI 通过用户意图进入 Application，Presentation 只负责 View Model 和呈现事件。

参考项目 `C:\Users\lbq08\Desktop\crosslink` 使用 Arduino `WebServer`、`String`、`FsFile` 和
自有 `HalStorage`。这些后端类型与 DeskMate 的 ESP-IDF 架构不兼容，因此不直接复制
`CrossPointWebServer`、`WebDAVHandler`、`CrossPointWebServerActivity` 或 `HalStorage`。

允许直接迁移并改造的内容为：

- `src/network/html/FilesPage.html` 的页面布局、目录浏览和上传进度交互。
- `scripts/build_html.py` 的 HTML 压缩、gzip 和生成固件内嵌资源逻辑。
- 流式目录 JSON、分块写入和上传进度的纯算法思路。

迁移页面时删除参考项目中的设置、删除、移动、新建目录、图片转换、ZIP、WebDAV 和 WebSocket
代码，并增加 DeskMate 的登录、Bearer 会话令牌、原始 HTTP `PUT` 上传和中文界面。

Crosslink 使用 MIT License。凡复制或实质改造其代码和页面资源，必须在 DeskMate 中增加第三方
声明，保留原版权与 MIT 许可文本，并标注被修改文件的来源。DeskMate 当前没有根许可证文件，
该声明只用于履行上游许可义务，不自行决定 DeskMate 的整体发布许可证。

## 3. 架构与组件职责

整体数据流为：

```text
设备设置页用户意图
    → app_web_file
    → app_network 申请 APP_NETWORK_LEASE_WEB_FILE
    → web_file_service 启动 HTTPD
    → 浏览器认证并发起文件请求
    → web_file_service 通过 /sdcard VFS 串行执行文件事务
    → app_web_file / Presenter 更新设备端状态
```

### 3.1 `web_file_service`

新增独立 Service 组件：

```text
components/services/web_file_service/
├── CMakeLists.txt
├── README.md
├── include/
│   └── web_file_service.h
├── src/
│   ├── web_file_service.c
│   ├── web_file_service_auth.c
│   ├── web_file_service_path.c
│   ├── web_file_service_transfer.c
│   └── web_file_service_internal.h
├── web/
│   └── index.html
└── test/
    └── test_web_file_service.c
```

该 Service 负责：

- 独占 ESP-IDF HTTPD 句柄及其内部 Task 生命周期。
- 生成和校验一次性访问码、会话令牌与登录限流状态。
- 维护单会话、单传输和停止协作状态。
- 将逻辑网页路径安全映射到 `/sdcard`。
- 串行执行目录遍历、下载、上传、提交和残留事务恢复。
- 在 HTTP handler 上下文同步产生最终 HTTP 响应。

该 Service 不负责：

- 决定何时联网、何时开启页面或是否允许轻睡眠。
- 初始化、挂载或卸载 SD 卡文件系统。
- Dashboard、OTA、语音、Portal 或页面导航策略。
- 修改 DeskMate 产品配置。

该组件满足 Service 的进入条件：它拥有由 HTTPD 创建的持续执行资源、长期会话状态和跨网络与
文件系统的完整上传提交/回滚事务。组件必须创建 `README.md`，说明 Task、状态、依赖和失败终态。

### 3.2 `app_web_file`

在 `main/application/` 新增 `app_web_file.[ch]`，作为网页文件管理用例的唯一产品状态所有者。

它负责：

- 响应设备设置页的开启和关闭意图。
- 检查 `system_filesystem_is_mounted()`。
- 申请和释放 `APP_NETWORK_LEASE_WEB_FILE`。
- 读取当前局域网 IPv4 地址并组织访问 URL。
- 启动、停止 `web_file_service`。
- 把启动中、运行中、关闭中和错误状态交给 Presenter。
- 网络重新连接并获得新地址后刷新设备端 URL。

开启顺序：

```text
确认 SD 已挂载
    → 申请 Web 文件网络租约
    → 确认 STA 已获得 IPv4
    → 启动 web_file_service
    → 发布 URL、访问码和容量状态
```

关闭顺序：

```text
标记关闭中
    → 协作停止 HTTPD 与活动传输
    → 清理会话和临时文件
    → 释放 Web 文件网络租约
    → 发布已关闭状态
```

若 Service 未达到安全停止终态，Application 不得释放网络租约。

### 3.3 网络租约

`app_network_lease_type_t` 增加 `APP_NETWORK_LEASE_WEB_FILE`，并提供用途明确的 Web 文件租约
申请和释放 API。该租约期间：

- 保持 Network Manager 会话运行。
- 暂停 Dashboard 周期同步和自动 OTA。
- 拒绝进入配网 Portal。
- 拒绝开始实时语音租约或 OTA 事务。
- `app_network_suspend_for_power_save()` 返回冲突状态，从而阻止低功耗停网和轻睡眠。

租约仍由 `app_network` 的唯一 Task 串行修改。`app_web_file` 不直接操作 Wi-Fi Driver。

### 3.4 Presentation 与 UI

新增 `web_file_presenter.[ch]` 和有界 View Model，至少包含：

- `STOPPED`、`STARTING`、`RUNNING`、`STOPPING`、`ERROR` 状态。
- 当前访问 URL。
- 6 位访问码，仅在 `RUNNING` 状态有效。
- SD 卡总容量与可用容量。
- 最近一次错误的用户可读中文说明。

设置菜单增加“网页文件管理”入口。设备页面只读取 Presenter 快照并发出开启/关闭用户意图，
不得直接包含 Service、System 或 Application 头文件。用户从该页返回时发出关闭意图。

## 4. 生命周期与并发

`web_file_service` 使用以下显式生命周期：

```text
UNINITIALIZED → INITIALIZED → RUNNING → STOPPING → INITIALIZED
       ↑                                              │
       └──────────────── DEINIT ──────────────────────┘
```

- `init()` 创建状态锁和固定同步资源，不启动 HTTPD。
- `start()` 恢复残留事务、生成访问码并启动 HTTPD；成功后才进入 `RUNNING`。
- `stop(timeout_ms)` 先拒绝新请求，再取消活动 socket，等待 handler 退出并停止 HTTPD。
- `deinit()` 仅在 HTTPD、文件句柄、传输缓冲和会话均已释放时删除同步资源。
- 停止超时或清理失败时保留 `STOPPING/CLEANUP_FAILED` 等价状态，拒绝再次启动。

HTTPD handler 是文件访问的唯一执行上下文。组件不额外创建轮询 Task，也不允许两个文件请求
并发。状态锁只保护短时内存状态；不得持锁执行网络接收、SD I/O、等待或外部回调。

服务运行期间只有一个有效浏览器会话。目录浏览、下载和上传共享同一个传输占用标志；开始传输
后其他文件请求返回忙碌状态。第二个已认证浏览器会话被拒绝。

## 5. HTTP 接口

固件提供内嵌单页 HTML/CSS/JS，不依赖 CDN 或互联网资源。

| 方法与路径 | 认证 | 作用 |
| --- | --- | --- |
| `GET /` | 否 | 返回登录和文件管理页面 |
| `POST /api/session` | 访问码 | 创建唯一浏览器会话并返回 128 位随机令牌 |
| `GET /api/files?path=/...` | Bearer | 返回当前目录、容量和条目列表 |
| `GET /api/file?path=/...` | Bearer | 流式下载普通文件 |
| `PUT /api/file?path=/...` | Bearer | 将原始请求体流式上传到当前路径 |

`GET /api/files` 返回对象而不是裸数组：

```json
{
  "path": "/images",
  "totalBytes": 34359738368,
  "freeBytes": 21474836480,
  "entries": [
    {
      "name": "天气图标.png",
      "type": "file",
      "sizeBytes": 12345
    }
  ]
}
```

目录条目必须逐项转义并流式输出，禁止把整个目录列表一次性分配到内存。超出单条响应缓冲上限的
文件名应作为明确错误处理，不能静默跳过并产生不完整视图。

上传使用原始 HTTP `PUT`，不使用 `multipart/form-data`。浏览器使用 XHR 发送 `File` 对象，
以获得已发送字节数、百分比和速率。服务端必须要求有效 `Content-Length`，并在接收文件内容前
完成大小、空间、路径和覆盖许可检查。

同名目标存在时，首次请求返回 `409 Conflict`。浏览器弹出确认后，以明确覆盖请求头重新提交；
服务端不得仅依赖前端检查。

## 6. 认证与会话

Service 启动时使用硬件随机数生成：

- 带前导零的 6 位十进制访问码。
- 登录成功后使用的 128 位随机 Bearer 令牌。

访问码只显示在设备屏幕，不写入日志。Bearer 令牌由浏览器保存在当前标签页的
`sessionStorage`，所有文件 API 通过 `Authorization` 请求头传递。服务端不开放 CORS，降低其他
网页跨站调用本地设备接口的风险。

会话规则：

- 同一时间仅允许一个有效令牌。
- 无传输时连续 10 分钟没有认证请求，会话失效并允许重新登录。
- 正在上传或下载时不执行空闲失效。
- 连续 5 次访问码错误后，登录入口锁定 30 秒。
- 关闭 Service 后访问码和令牌立即失效。
- 访问码和令牌使用常量时间比较。

HTTP 明文无法防止同一局域网中的被动抓包。本功能依赖受信任的本地 Wi-Fi，并通过设备端按需
开启、短期访问码和单会话减少暴露面；本阶段不引入 TLS 证书管理。

## 7. 路径与 UTF-8 安全

网页逻辑根 `/` 映射为 `SYSTEM_FILESYSTEM_MOUNT_POINT`，即 `/sdcard`。路径处理必须：

1. 对 URL 参数只执行一次百分号解码。
2. 验证结果是合法 UTF-8。
3. 要求逻辑路径以 `/` 开头。
4. 拒绝空字节、控制字符、反斜杠、`.`、`..` 和超长分段。
5. 每个名称最多 255 字节，完整物理路径不得超过实现声明的固定上限。
6. 构造物理路径后再次验证其前缀边界为 `/sdcard/` 或恰好 `/sdcard`。
7. 下载目标必须是普通文件；上传目标的父路径必须是已存在目录。
8. 不跟随或创建符号链接；即使当前 FAT 不支持，也保留显式类型检查。

用户要求暴露完整 `/sdcard`，因此普通隐藏文件和目录也会显示。唯一例外是 Service 自己保留的
`/sdcard/.deskmate-web` 事务目录，该目录不出现在网页列表，也不允许通过文件 API 访问。

FatFs 配置调整为 UTF-8 API 编码，并保持长文件名使用堆分配。中文路径、空格、`%`、`#` 和多
字节边界必须进入测试。

## 8. 上传、覆盖与恢复事务

### 8.1 资源限制

- 单文件 `Content-Length` 最大为 500 MiB。
- 上传前查询文件系统可用空间。
- 暂存文件必须有完整可用空间；覆盖现有文件时不能先删除旧文件来释放空间。
- Service 为元数据和文件系统操作保留固定安全余量，空间不足返回 `507 Insufficient Storage`。
- 上传和下载共享一个 32 KiB 缓冲区。

32 KiB 缓冲区必须显式从 PSRAM 分配：

```c
heap_caps_malloc(32U * 1024U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

缓冲区不放入 HTTPD Task 栈。分配失败时拒绝传输并返回内存错误，不退化为大型栈缓冲。路径、
认证和小型响应字段继续使用有界内部 RAM 缓冲。

### 8.2 新文件上传

```text
校验请求
    → 创建 /sdcard/.deskmate-web/upload.part
    → 循环接收并写入 32 KiB 数据块
    → fsync
    → 关闭并核对实际长度
    → 重命名到最终目标
    → 清理事务元数据
```

任何接收、写入、同步、长度核对或重命名失败都会关闭句柄并删除 `.part`。

### 8.3 覆盖上传

覆盖提交必须保留旧文件，采用单事务记录：

```text
完整写入 upload.part
    → 写入并同步事务记录
    → 旧目标重命名为备份
    → upload.part 重命名为目标
    → 删除备份
    → 删除事务记录
```

如果新文件提交失败，立即把备份恢复为原目标。启动 Service 前检查事务目录：

- 只有 `.part`：删除未完成上传。
- 目标缺失且备份存在：恢复备份。
- 目标存在且备份存在：视事务阶段清理已经提交的备份。
- 状态无法唯一判断：停止启动并向 Application 报告恢复错误，不猜测或删除用户文件。

事务记录只保存恢复所需的有界目标路径、预期长度和阶段，不保存文件内容、访问码或令牌。

### 8.4 下载

下载前固定文件长度和 MIME 类型，使用 32 KiB PSRAM 缓冲区循环读取并发送。响应包含
`Content-Length` 和支持 UTF-8 文件名的 `Content-Disposition`。客户端断开时立即关闭文件并
释放传输状态。

## 9. 停止、断网和故障处理

停止流程先设置拒绝新请求的状态，再关闭活动 session/socket，使阻塞的接收或发送尽快返回。
handler 在每个网络与文件块边界检查取消状态。`stop(timeout_ms)` 必须有有界等待，只有以下
资源均已释放才返回 `ESP_OK`：

- HTTPD 及其 Task。
- 活动 socket 和会话。
- `FILE`、`DIR` 等 VFS 句柄。
- 32 KiB PSRAM 缓冲区。
- 未提交的临时文件。

错误映射：

| 场景 | HTTP 状态 |
| --- | --- |
| 路径、编码或请求参数非法 | `400 Bad Request` |
| 未认证或令牌失效 | `401 Unauthorized` |
| 已有其他会话或传输 | `423 Locked` |
| 文件或目录不存在 | `404 Not Found` |
| 同名文件未确认覆盖 | `409 Conflict` |
| 文件超过 500 MiB | `413 Content Too Large` |
| SD 卡空间不足 | `507 Insufficient Storage` |
| SD 卡、同步、提交或内部资源失败 | `500 Internal Server Error` |

HTTP JSON 错误同时提供稳定短错误码和中文说明。项目日志和设备 UI 使用中文，且不得包含访问码、
令牌、文件内容或其他凭据。

Wi-Fi 临时断开时，当前传输失败并按上传/下载失败路径收敛。Network Manager 继续执行自身技术
重连；重新获得 IPv4 后 HTTPD 继续服务，`app_web_file` 更新设备页面显示的新 URL。

## 10. 页面交互

设备端页面：

- 进入时依次显示检查 SD 卡、申请网络和启动服务状态。
- 运行时显示 `http://<IPv4>/`、6 位访问码、总容量和剩余容量。
- 用户返回时显示关闭中状态，完成后返回设置菜单。
- 错误状态显示可诊断中文说明，不自动改为 Portal 或热点模式。

浏览器页面：

- 首屏只显示访问码登录。
- 登录后显示路径面包屑、容量、名称、类型和大小。
- 点击目录进入，点击文件下载。
- 上传目标为当前目录。
- 同名文件弹出覆盖确认。
- 上传期间显示已发送字节数、百分比和速率，并禁用其他文件操作。
- 页面使用简体中文并适配桌面与手机宽度。

## 11. 构建与配置

- `web_file_service` 依赖 `esp_http_server`、`esp_system`、`freertos` 和 `sys`，具体公开与私有
  依赖按头文件实际使用区分。
- 网页压缩脚本接入组件 CMake，在统一 `dm.ps1 build` 流程中生成内嵌资源，不要求开发者手工
  运行脚本。
- 不新增 Arduino、WebSocket 或 WebDAV 依赖。
- ESP-IDF HTTPD WebSocket 支持保持关闭。
- FatFs API 编码切换为 UTF-8，并核对 `sdkconfig.defaults`、`sdkconfig.ci` 与当前配置的一致性。
- HTTPD 接收和发送等待时间必须有界，以支持协作停止。

## 12. 验证

### 12.1 纯逻辑与组件测试

- 路径：中文、空格、`%`、`#`、控制字符、无效 UTF-8、重复编码、`.`、`..`、超长分段和根
  目录边界。
- 认证：正确/错误访问码、5 次错误锁定、30 秒恢复、令牌比较、10 分钟空闲失效、关闭失效和
  第二客户端拒绝。
- 上传：0 字节、32 KiB 边界、非整块结尾、500 MiB 上限、超过上限、空间不足和短写。
- 事务：新文件取消、覆盖取消、断网、`fsync` 失败、重命名失败和各事务阶段重启恢复。
- HTTP：所有错误状态和 JSON 结构稳定，未认证请求不能读取任何文件元数据。

### 12.2 实机验证

- 中文目录与文件能够浏览、上传和下载。
- 下载后文件大小和哈希与源文件一致。
- 500 MiB 文件完整上传，记录实际速率、内部 RAM、PSRAM 和历史最小剩余内存。
- 上传期间设备 UI 仍可响应返回操作且不触发看门狗。
- 服务运行期间轻睡眠被阻止；关闭后网络租约可正常释放。
- 第二浏览器被拒绝，空闲超时后可以重新登录。
- 断网重连后 URL 更新，失败传输没有残留 `.part`。
- 停止后 HTTPD Task、socket、文件句柄、PSRAM 缓冲和网络租约全部释放。

仓库规则禁止 Agent 默认主动编译。实现完成后的编译只有在用户明确要求时，才通过仓库根目录的
统一命令执行：

```powershell
& .\dm.ps1 build
```

不得直接调用 `idf.py`、`cmake` 或 `ninja`。

## 13. 完成判定

满足以下条件才视为本阶段完成：

- 用户能从设备设置菜单手动开启和关闭网页文件管理。
- 受认证浏览器能访问完整 `/sdcard` 并浏览、下载和上传文件。
- 中文路径和 500 MiB 文件满足已定义测试。
- 单会话、路径边界、覆盖确认和恢复事务均生效。
- 服务运行阻止轻睡眠，安全停止后释放网络租约和全部资源。
- 没有配置编辑、WebDAV、WebSocket 或其他范围外功能。
- 迁移的 Crosslink 代码和资源包含完整 MIT 第三方声明。
