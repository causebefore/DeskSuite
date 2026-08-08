# PhotoPainter Web Console 文件管理移植设计

> 状态：交互与技术方向已确认，等待书面规格评审；尚未进入实施。
>
> 日期：2026-08-08

## 1. 背景与结论

PhotoPainter 当前没有 Web Console 文件管理入口。DeskMate 已有的共享
`web_console_service` 可以按 `Core + Files` 裁剪复用，但不能把 DeskMate 的 `/sdcard`
产品装配原样复制过来：PhotoPainter 的 `/sdcard/display` 保存活动集合、上一集合、PPF2 页面、
A/B 状态槽和事务临时文件，属于 `display_collection_service` 的内部持久化数据。对其进行网页
删除、移动或覆盖会破坏集合一致性。

第一版采用以下结论：

- 复用共享 Web Console 的认证、HTTP 生命周期、文件操作和浏览器 UI。
- PhotoPainter 只启用 `Core + Files`，不移植 DeskMate 的 Settings、Status 或产品 Provider。
- 浏览器逻辑根固定映射到新目录 `/sdcard/user`；`/sdcard/display`、Flash、NVS 和原始分区均
  不可见、不可寻址。
- 用户上传的图片只是普通文件，不自动转换、不更新集合，也不进入照片播放。
- 右键长按三秒请求进入文件管理维护会话；左键退出，运行十分钟后自动退出。
- `power_management_app` 是维护会话、网络、休眠和恢复顺序的唯一所有者。
- 文件服务运行时取得 SD 文件系统租约，并与内容刷新、OTA、深睡和 SD 自动卸载互斥。

## 2. 用户体验

### 2.1 进入

用户在 PhotoPainter 正常照片页长按右键三秒。按键回调只提交异步请求，不直接启动网络或 HTTP
服务。设备等待当前内容刷新、显示提交和网络清理收敛后进入维护会话；等待期间显示简短的
“正在启动文件管理”模态页。

启动成功后墨水屏显示：

```text
FILE MANAGER
IP 192.168.1.23
CODE 123456
LEFT: EXIT
```

屏幕最多四行、每行不超过现有 ASCII 模态页容量。访问码只在本地呈现，使用完的内存副本立即
覆盖，不写日志、不远传 Hub，也不持久化。

### 2.2 浏览器

用户在同一局域网内打开设备 IP，输入本次六位访问码后进入 Files-only 页面。页面只显示
“文件管理”，不出现设置、状态、Wi-Fi、服务器、OTA 或重启入口。

第一版提供：

- 浏览目录；
- 上传与下载；
- 新建文件夹；
- 重命名与同一用户根内移动；
- 删除文件或空目录；
- 覆盖前明确确认；
- 中文及其他有效 UTF-8 文件名。

文件类型不限制。JPEG、PNG、PPF 或其他文件上传后都只保存在用户区，不触发内容刷新或播放。
共享页面中写死的“SD 卡”客户文案改为“设备存储”，DeskMate 与 PhotoPainter 继续使用同一份
通用页面。

### 2.3 退出

- 左键单击提交退出请求。
- 会话从 Web Console 成功进入 `RUNNING` 起最多运行十分钟；到期自动退出。
- 退出时先关闭 HTTP 入口并排空文件操作，再释放 SD 和网络，最后恢复进入前的照片。
- 手动退出后重新开启正常 180 秒清醒窗口；超时退出则恢复原绝对唤醒计划并进入既有休眠判定。
- 普通退出不主动发起新一轮内容刷新；用户仍可通过既有确认键长按操作刷新。

## 3. 范围

### 3.1 第一版包含

1. 将共享 `web_console_service` 以 Files-only 方式链接进 PhotoPainter。
2. 新增 PhotoPainter 产品装配：端口、用户根、事务目录、容量回调、上传上限和预留空间。
3. 右键长按三秒的维护请求与模态页交互。
4. 在 `power_management_app` 现有唯一 Task 内增加 Web Console 维护阶段和启停编排。
5. 增加 SD 文件系统租约、拔卡事实转发和容量快照能力。
6. 将现有临时模态页恢复标记从 OTA 专用语义泛化，使异常复位后不会长期显示已失效验证码。
7. 扩展共享网页构建测试和 PhotoPainter 产品契约检查。
8. 在用户已明确授权的前提下，通过仓库根 `ds.ps1` 编译 PhotoPainter 固件。

### 3.2 第一版不包含

- 上传图片后自动加入播放集合。
- JPEG/PNG 到 PPF2 的本地转换。
- 浏览、下载或修改 `/sdcard/display`。
- Flash、NVS、LittleFS、字体、模型、OTA、coredump 或原始分区访问。
- Web Console 内的 Wi-Fi、Hub、声音、系统设置或设备状态页面。
- 配网 Portal 与 Web Console 的同时运行或页面合并。
- 公网访问、WebDAV、账户体系、多用户会话或远程 Hub 代理。
- SD 卡格式化、分区、修复或跨卡迁移。
- OTA 发布、设备安装或实体设备验收；这些动作需要分别报告，固件构建成功不能替代它们。

## 4. 组件职责

### 4.1 `photo_playback_app`

- 在现有按键扫描中识别右键三秒长按结束事实。
- 通过新的 Web Console 请求回调把意图提交给协调方，完成语义与现有刷新、固件检查请求一致。
- 不直接依赖网络、SD 或共享 Web Console Service，避免 Application 反向依赖和组件环。
- 维护会话中复用现有模态按键捕获：左键退出，确认键和右键不触发翻页、刷新或 OTA。
- 复用现有多行 ASCII 模态页呈现启动状态、IP、验证码和错误。

公共接口应使用已登记的 `request`、`callback`、`borrow` 和 `web_console` 术语，不新增
`manager`、`worker`、`maintenance` 等平行架构名词。

### 4.2 `power_management_app`

`power_management_app` 的唯一 Task 增加 Web Console 的待处理、启动、运行、停止和照片恢复阶段。
这些阶段并入现有电源状态机，不创建第二个调度 Task，也不通过停止/重启整个 Power Application
来阻止休眠。

它负责：

- 接受或拒绝按键请求；
- 等待内容、显示和网络达到安全边界；
- 停止空闲的 `content_refresh_app`；
- 排斥 OTA、手动休眠和新的内容刷新；
- 取得并释放 SD 文件系统租约；
- 独占启动和停止 `network_manager`；
- 初始化、启动、停止和反初始化共享 Web Console；
- 读取并本地呈现访问码；
- 处理左键、超时、断网和拔卡；
- 恢复照片、原绝对唤醒目标和正常按键语义。

### 4.3 `sd_card_service` 与 `device_sd`

`sd_card_service` 仍是插拔监测和自动挂载/卸载所有者。新增严格配对、带代次的文件系统租约：

- 只有卡已插入且文件系统已挂载时才能取得；同一时刻只允许一个持有者。
- 租约期间 SD Service 继续监测物理插拔，但不得在共享 Files 仍持有 VFS 句柄时直接卸载。
- 检测到拔卡后发布不可变事实，`power_management_app` 立即进入 Console 停止流程。
- Console 完全停止和反初始化后释放租约；释放时由 SD Service 重新收敛挂载事实。
- 代次防止旧停止路径误释放新的租约。

租约只保证挂载生命周期稳定。文件写入排斥由产品状态机保证：维护会话开始前停止内容刷新，并让
照片播放停留在不读取集合文件的模态状态，因此共享 Files 是用户区唯一文件操作方。

`device_sd` 增加有界容量快照读取，返回总容量和可用字节数；它只负责介质机制，不包含 500 MiB
或 8 MiB 等产品策略。

### 4.4 共享 `web_console_service`

共享 Service 保持产品无关：

- 继续拥有访问码、Bearer 会话、登录失败锁定、HTTP 路由和 handler 排空。
- Files 根、事务目录、上传上限、预留空间和容量回调全部由 PhotoPainter 装配注入。
- 不依赖 `device_sd`、`sd_card_service`、`power_management_app` 或 PhotoPainter 产品头文件。
- 不增加 PhotoPainter 产品条件分支。
- 除把“SD 卡”改为“设备存储”外，不重做 DeskMate Files 的交互和视觉结构。

## 5. 生命周期与数据流

```text
右键长按三秒
    → photo_playback_app 提交 Web Console 请求
    → power_management_app 记录 PENDING
    → 等待本轮内容与显示收敛、网络清理成功
    → 停止空闲 content_refresh_app
    → 开始模态按键捕获并设置持久化照片恢复标记
    → 取得 SD 文件系统租约
    → 确认 /sdcard/user 存在并读取容量
    → 启动 network_manager 并等待 ONLINE
    → web_console_service_init_borrow()
    → web_console_service_start()
    → 分别读取 network_manager IP 与 Web Console 访问码摘要
    → 本地显示 IP、CODE、LEFT: EXIT
    → RUNNING
```

启动前必须同时满足：

1. 最近内容刷新已经形成终态，且网络清理成功。
2. 目标集合显示已经收敛，照片 Application 可以进入模态状态。
3. `content_refresh_app` 处于 `IDLE`、`BACKOFF` 或 `STOPPED`。
4. `network_manager` 处于 `STOPPED`。
5. 没有 OTA、休眠准备、恢复或其他清理事务。
6. SD 卡存在、已挂载且不存在活动文件系统租约。

请求可在等待刷新、等待显示或正常清醒窗口中先记为待处理；OTA、清理失败、休眠提交后或已在
Console 会话中时拒绝重复请求。

停止顺序固定为：

```text
关闭新 HTTP 请求
    → web_console_service_stop(timeout_ms)
    → 若超时或 CLEANUP_FAILED，保留全部所有权并有界重试
    → web_console_service_deinit()
    → 释放 SD 文件系统租约并收敛插拔事实
    → network_manager_stop()
    → 必要时恢复 display_collection_service
    → 恢复原照片
    → 清除持久化照片恢复标记
    → 结束模态按键捕获
    → 恢复清醒窗口或原休眠计划
```

`stop()` 或 `deinit()` 未成功时，禁止提前释放 SD、关闭其所有者、进入深睡或再次启动 Console。

## 6. 文件系统边界

### 6.1 可见根

```text
浏览器逻辑根 “设备存储”
└─ /sdcard/user
   ├─ 用户文件和文件夹
   └─ .photopainter-web     隐藏且保留的上传事务目录
```

PhotoPainter 装配参数：

| 参数 | 第一版值 |
| --- | --- |
| 物理用户根 | `/sdcard/user` |
| 事务工作区 | `.photopainter-web` |
| 单文件上传上限 | 500 MiB |
| 上传后最小剩余空间 | 8 MiB |
| HTTP 端口 | 80 |

8 MiB 预留高于当前活动集合、上一集合与一次全新 16 页集合同时存在时约 4.4 MiB 的页面数据需求，
为 Manifest、状态槽、临时文件和 FAT 开销留下余量。它是上传准入下限，不是 SD 分区或目录配额。

### 6.2 隐藏与禁止范围

- `/sdcard/display/**`：集合 Service 的内部事务数据，完全不在挂载根之下。
- `/sdcard/display/state_a.json`、`state_b.json`：A/B 状态槽。
- `/sdcard/display/collections/**`：集合 Manifest。
- `/sdcard/display/pages/**`：不可变 PPF2 页面。
- `/sdcard/display/tmp/**`：集合事务临时文件。
- `/sdcard` 根下除 `user` 外的所有路径。
- Flash `/`、NVS、LittleFS 和原始分区。

服务端必须基于注入根重新解析所有相对路径，拒绝绝对路径、`..`、越界规范化、事务目录访问和
跨根移动。FAT 大小写语义下也要保护事务目录名称，不能只依赖浏览器隐藏。

### 6.3 写入与恢复

- 上传先写事务目录中的临时文件，关闭并确认传输完成后再原子发布到目标目录。
- 覆盖失败、断网、复位或超时不能破坏原正式文件。
- 下一次 Service 启动时复用共享恢复规则，恢复或清理未完成事务。
- 删除沿用当前共享 Files 的非递归语义；非空目录不得伪装成删除成功。
- 移动和重命名只允许发生在用户根内部。
- 容量在上传准入和提交前都重新读取；空间不足时保留原文件。

## 7. 网络、认证与安全

- Web Console 仅在 PhotoPainter 已保存 Wi-Fi 配置且能够加入局域网时启动。
- 不启动 Portal，也不允许 Portal 和 Web Console 同时占用端口 80。
- `network_manager` 当前没有租约和多回调能力，因此维护会话复用现有 Power Application 的独占
  网络编排，不新增引用计数假装支持并发所有者。
- 每次 `start()` 生成新的六位访问码；停止开始时访问码和会话 token 立即失效。
- 继续沿用单一会话、128 位 token、十分钟 token 空闲失效、连续五次错误锁定三十秒。
- Application 的十分钟运行期限独立于认证 token；期限到达后即使浏览器仍打开也执行安全停止。
- 所有受保护响应继续使用 `Cache-Control: no-store`。
- 文件名可以进入页面和必要的本地操作结果，但不进入普通运行日志；验证码、token 和网络凭据
  从不进入日志、NVS 新字段或 Hub 请求。

## 8. 错误与恢复策略

| 事实 | 设备行为 | 浏览器或屏幕文案 |
| --- | --- | --- |
| 未插 SD 卡 | 不取得租约，不启动网络和 HTTP | “请插入 SD 卡后重试” |
| SD 挂载失败 | 保留原产品状态，结束请求 | “SD 卡暂时不可用” |
| 创建用户目录失败 | 不启动 Files | “无法准备用户文件区” |
| Wi-Fi 连接失败 | 释放 SD 租约并恢复照片 | “无法连接已保存的 Wi-Fi” |
| HTTP 启动失败 | 按 Service 契约回滚或继续清理 | “文件管理启动失败” |
| 空间不足 | 拒绝上传或提交，保留原文件 | “设备存储空间不足” |
| 网络运行中断开 | 停止 Console 并恢复产品状态 | “网络连接已断开” |
| SD 运行中拔出 | 关闭入口、排空操作、释放租约后卸载 | “SD 卡已拔出” |
| 停止超时 | 保持唤醒和全部所有权，有界重试 | “正在安全关闭文件管理” |
| 恢复照片失败 | 不清除恢复标记，进入现有受控错误路径 | “照片恢复失败” |

墨水屏断电后仍保留图像。进入访问码页面前必须持久化“临时模态页待恢复”事实；异常复位后
`bootstart_app` 先恢复活动照片，成功后才清除标记。不能继续使用仅描述 OTA 的旧命名，也不能
让失效访问码长期停留在屏幕上。

若维护期间发生过拔卡，即使卡随后重新插入，也不能直接复用内存中的旧集合快照。停止 Files、
释放租约并重新收敛 SD 后，必须显式恢复 `display_collection_service`；恢复失败时不尝试按旧路径
播放。第一版不宣称支持无缝换卡。

## 9. 构建装配

PhotoPainter 根构建加入共享组件目录，并建立产品 Application 到 `web_console_service` 的真实
组件依赖。`sdkconfig.defaults` 显式设置：

```text
CONFIG_WEB_CONSOLE_FILES=y
CONFIG_WEB_CONSOLE_SETTINGS=n
CONFIG_WEB_CONSOLE_STATUS=n
CONFIG_HTTPD_MAX_URI_LEN=2048
```

现有 PSRAM、FATFS UTF-8 长文件名和文件锁配置继续复用。Files 每次活动上传或下载使用共享组件
既有的 32 KiB PSRAM 缓冲，不增加与文件大小等比例的 RAM 分配。

构建产物必须证明：

- Core 和 Files 路由、网页片段及实现进入固件；
- Settings、Status 和 DeskMate Provider 未进入固件；
- Capabilities 只报告 Files；
- 端口、URI 长度和 FATFS 配置符合产品默认值；
- map 中没有意外链接 DeskMate 产品符号。

## 10. 自动化与分级验证

### 10.1 共享组件测试

- 扩展 `scripts/test_build_html.py`，保持所有 Files/Settings/Status 裁剪组合可生成。
- 验证 Files-only 页面只包含文件能力，不包含设置或状态入口。
- 验证客户文案使用“设备存储”，不再把共享组件限定为 DeskMate 或单一产品。
- 保持确定性构建、gzip 结果和模块标记断言。
- 对共享 Files 既有路径、认证、事务恢复和停止契约做回归；无法在主机真实执行的 ESP-IDF 行为
  明确留给构建和实机验收，不用静态字符串断言冒充运行测试。

### 10.2 PhotoPainter 产品检查

- 右键长按只有达到三秒并结束后才提交一次请求；短按仍只翻到下一张。
- 按键回调不直接调用网络、SD 或 HTTP 生命周期 API。
- Power Application 的进入门控、互斥状态和停止顺序有可审查的静态契约。
- 文件系统租约成对、带代次，拔卡期间不提前卸载。
- 用户根、事务目录、500 MiB 上限、8 MiB 预留和容量回调均由产品装配注入。
- 临时模态页恢复标记在正常退出和异常复位路径中都有对应处理。
- 运行格式检查和 `git diff --check`，不混入现有 Hub 工作区修改。

### 10.3 固件构建

用户已明确授权本任务进行 PhotoPainter 固件构建。实现完成后只能从 DeskSuite 根目录执行：

```powershell
& .\ds.ps1 build photopainter
```

同时核对生成 `sdkconfig`、`compile_commands.json` 和链接 map，并记录固件体积、RAM/PSRAM 增量。
本授权只包含构建，不自动包含 OTA 发布、烧录或设备安装。

### 10.4 实体设备验收

实体设备阶段至少覆盖：

1. 右键短按仍翻页，右键长按进入文件管理。
2. 屏幕显示正确 IP、一次性验证码和退出提示。
3. 正确登录、五次错误锁定、停止后旧验证码和 token 失效。
4. 中文文件名的上传、下载、新建目录、重命名、移动和删除。
5. 构造路径访问 `/sdcard/display`、事务目录或上级路径均失败。
6. 覆盖上传中断、空间不足和异常复位后原文件仍完整。
7. 上传期间断网、拔卡和十分钟到期能够安全停止。
8. 退出后 HTTPD Task、socket、文件句柄、32 KiB 缓冲及网络/SD 所有权全部释放。
9. 原照片恢复，后续翻页、内容刷新、OTA 和深睡没有回归。

实体卡是否存在、卡容量、真实 Wi-Fi、浏览器访问、热拔卡和墨水屏物理恢复均不能由源码审查或
固件构建证明，必须单独报告为实机已验证或未验证。

## 11. 验收条件

1. PhotoPainter 固件只装配共享 `Core + Files`，页面没有 Settings 或 Status。
2. 右键短按行为不变，右键长按三秒请求维护会话。
3. 网络、Console、SD 和休眠只由 `power_management_app` 唯一 Task 编排。
4. 浏览器根固定为 `/sdcard/user`，无法通过路径、大小写或移动操作触达 `/sdcard/display`。
5. 用户可以完成既定文件操作，上传内容不会进入播放集合。
6. 单文件上限为 500 MiB，提交后至少保留 8 MiB 可用空间。
7. 访问码和 token 不持久化、不记录、不远传，停止开始即失效。
8. 左键退出和十分钟自动退出都遵守“停止 Console 后再释放资源”的顺序。
9. 断网、拔卡、空间不足和停止超时不会导致提前卸载、带句柄休眠或覆盖原文件。
10. 异常复位后不会长期显示已经失效的访问码。
11. 共享网页使用通用“设备存储”文案，DeskMate Files 行为无回归。
12. 自动化检查与 PhotoPainter 固件构建分别通过；OTA、安装和实体设备验收明确单独报告。

## 12. 实施边界

书面规格确认后，实施计划应至少按以下可独立验证的顺序拆分：

1. 共享 Files 文案和构建测试通用化。
2. PhotoPainter 构建裁剪与 Files-only 产品装配。
3. SD 容量快照、文件系统租约和拔卡事实契约。
4. 右键长按请求及临时模态页恢复语义。
5. Power Application 的启动、运行、停止和错误恢复状态机。
6. 静态检查、共享测试与固件构建证明。

每一步只修改和提交本任务文件及代码块，保留工作区中现有 Hub 等无关修改。设计未授权 OTA、
烧录、实机安装、服务器配置或 Hub 变更。
