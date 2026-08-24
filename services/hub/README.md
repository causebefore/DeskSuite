# DeskSuite Hub

DeskSuite Hub 使用 FastAPI 为 PhotoPainter 和 DeskMate 提供通用设备身份、OTA、日志、语音及产品业务服务。天气、月相、日历、邮件、RSS 与 AI 额度由 Hub 统一取数；PhotoPainter 的数据会聚合后注入 HTML/JS 页面，ESP32-S3 只上传本地温湿度/电池状态并下载最终的 PPF2 四灰阶页面集合。

Hub 现在将跨服务职责拆成四个工作流：Assistant 统一处理文字/语音的 LangChain
Agent、工具和记忆；Voice 只适配 ASR/TTS 与既有设备帧；Display 和 Dashboard 保持
确定性，不经过大模型。第三方 LLM/语音协议位于 `app/providers/`，以后可在装配处替换
供应商。详细边界见 [`app/workflows/README.md`](app/workflows/README.md)。

后端目录职责和代码放置规则见 [`app/README.md`](app/README.md)。

## 环境初始化

项目使用 `uv` 管理 Python 3.12 环境：

```powershell
uv sync --extra dev
uv run playwright install chromium
Copy-Item .env.example .env
```

密钥写入 `.env`，普通配置写入 `config.toml`。`DEVICE_API_TOKEN` 留空时既有设备接口允许局域网开发访问；
文字 Assistant 为避免意外公开，始终要求先配置该 Token。配置后，语音、显示、OTA 和三个日志写入接口必须携带
`Authorization: Bearer <token>`。日志查询接口保持只读访问。

容器与进程探活使用无需鉴权的 `GET /healthz`。该接口只返回 `status` 和应用
`version`，不会请求天气、邮箱、模型或 MCP；构建镜像时可通过进程环境变量
`DESKSUITE_BUILD_ID` 注入 Git SHA，此时响应会额外包含 `build_id`。

上传语音默认不会落盘，三个 `/api/v1/voice/debug/audio/*` 路由也不会注册。仅在临时排查
录音质量时设置 `[voice.debug_audio].enabled = true`；此模式要求 `.env` 中存在非空
`DEVICE_API_TOKEN`，访问调试路由时也必须携带同一 Bearer Token。

## Assistant 与文字输入

最小文字入口为 `POST /api/v1/assistant/text`：

```powershell
$headers = @{ Authorization = "Bearer <DEVICE_API_TOKEN>" }
$body = @{ text = "今天天气怎么样"; thread_id = "home-chat" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8765/api/v1/assistant/text `
  -Headers $headers -ContentType 'application/json' -Body $body
```

`thread_id` 可省略，服务端会生成新会话；重复使用同一个值可继续多轮对话。当前只有一个
用户，长期身份由 `[assistant].principal_id = "owner"` 固定，客户端不能声明身份。
语音设备默认使用 `voice:<device_id>`，也可通过 `X-Thread-Id` 和文字入口共享会话。
短期完整历史存入 `[assistant].checkpoint_path` 指定的 LangGraph Checkpoint；跨 thread 的
长期事实存入 `[assistant.memory].store_path` 指定的 LangGraph Store。只有问题确实涉及过去
保存的事实或偏好、且当前会话没有答案时，Agent 才调用 `search_user_memory`；只有用户明确
要求长期记住时，才调用 `remember_user_fact` 执行 `put`。普通问候、复述和实时查询不会
发起 embedding 请求。
语义检索使用 `[providers].embedding` 指定的供应商，当前复用智谱 `embedding-3`。
旧版 `data/mem0_chroma` 不会被自动读取或删除；若部署环境曾启用 mem0，应先单独检查并
决定迁移或归档。

智谱联网搜索 MCP 由 `[mcp.web_search_prime].enabled` 控制；URL 留在 `config.toml`，
Authorization 在运行时复用现有 `ZHIPU_API_KEY`，不会写入配置或日志。Hub 启动时预热并
发现工具；真正搜索时由适配器创建并清理独立 session，当前只白名单允许
`webSearchPrime` 及适配后的 `web_search_prime`。

RSS 订阅地址在 `config.toml [rss].feeds` 中配置，支持最多 8 个 RSS/Atom URL。服务端使用 `feedparser` 解析，按 `cache_seconds` 缓存；单个源失败不影响其他源，全部失败时优先回退到最近缓存。

## 运行

```powershell
.\start_server.ps1
```

或直接使用 uvicorn：

```powershell
uv run uvicorn app.main:app --host 0.0.0.0 --port 8765
```

接口文档：`http://127.0.0.1:8765/docs`。

显示页面的字号、线宽、灰阶图形、图表、刷新和降级规则见 [`docs/UI_CONSTRAINTS.md`](docs/UI_CONSTRAINTS.md)。页面直接使用本地 TRMNL Framework 的画布、布局、文字角色和通用组件，再自动内嵌 [`web/shared/epaper.css`](web/shared/epaper.css) 补充四灰阶、中文可读性与墨水屏效果约束；页面 CSS 只保留功能特有布局。

## 使用真实数据生成四灰阶页面集合

渲染入口只使用 `.env` 和 `config.toml` 中已配置的真实天气/月相、iCloud 日程、QQ 邮箱、RSS 和智谱额度数据。设备 ID 缺省时读取 `config.toml [display.defaults]`：

```powershell
uv run python .\scripts\render_display.py
uv run python .\scripts\render_display.py --device-id photopainter-001
```

脚本和显示 API 都通过请求驱动的刷新协调服务运行：先根据页面依赖并行获取必要数据，再按页生成独立可见快照。它们会按 `config.toml [display].pages` 依次执行 `web/pages/<page-id>/` 中的 HTML/CSS/JavaScript。默认包含 `demo`、`calendar`、`month-calendar`、`weather`、`moon` 和 `rss` 六个页面；`month-calendar` 左侧显示当前自然月概览，右侧显示今天和最近四条日程，`caldav.month_max_events` 控制月历查询上限。RSS 专页展示订阅源统计和最近四篇文章，天气专页显示和风天气实况、7 日高低温趋势、分钟降水、预警、AQI/UV、风力与日出日落；月相专页显示当前逐小时月相、照明度、月升月落和全天六个采样点。页面采用本地固定的 TRMNL Framework 3.1.2（资源位于 `web/vendor/trmnl/3.1.2/`），并在页面样式前内嵌 `web/shared/epaper.css`，不依赖 CDN。中文统一使用本地 Alibaba PuHuiTi 3.0，字体文件位于 `web/vendor/fonts/`，渲染时以内嵌 Data URL 加载。天气与月相图标使用 QWeather Icons v1.8.0，本地资源位于 `web/vendor/qweather-icons/1.8.0/`；和风天气 API 的图标字段会直接匹配同名 SVG 并内嵌到页面。Chromium 先生成普通 RGBA 截图，再由 Pillow 强制量化为 2bpp 四灰阶图，输出到 `rendered_frames/<device-key>/`：

- `manifest.json`：当前多页面集合及每个页面的版本、摘要和下载地址。
- `pages/<page-id>/<version>.png`：量化后的四灰阶预览图。
- `pages/<page-id>/<version>.ppf`：32 字节 PPF2 头部 + 96000 字节 GRAY2 payload。
- `pages/<page-id>/render_state.json`：页面内部的可见内容快照、内容/静态资源指纹及其对应版本；不下发设备。

设备状态单独保存到 `config.toml [storage].device_status_dir`，不与生成的显示帧混放。

## 设备状态与显示 API

请求可通过 `X-Device-Id` 指定设备；缺省值来自 `config.toml [display.defaults]`。

- `PUT /api/v1/device/status`：上传最近一次有效的温湿度与电池电量；环境测量失败时可只上传电池。
- `POST /api/v2/display/render`：聚合服务端数据并生成新页面集合。
- `GET /api/v2/display/manifest`：按需渲染全部配置页面并返回集合 Manifest，支持 `ETag/304`。
- `GET /api/v2/display/frame/{page_id}/{version}.ppf`：下载指定页面的不可变 PPF2 文件。
- `GET /api/v2/display/preview/{page_id}/{version}.png`：查看指定页面的四灰阶预览。

显示 API 默认使用真实数据；日常生成和设备同步都不使用固定 demo 数据。

每个页面只比较自己依赖的可见内容与模板静态资源 SHA-256 指纹。例如邮件变化只影响 `demo`；天气变化影响 `demo` 和 `weather`；月相变化只影响 `moon`；RSS 变化只影响 `rss`；近期日程变化影响 `demo` 和 `calendar`，自然月日程变化影响 `month-calendar`。页面依赖的数据、取整后的设备状态及对应数据源可用状态都不变时，不启动 Chromium，直接沿用该页面原版本。请求时间、状态接收时间、设备 ID 和原始未取整传感器值不参与判定。语义内容变化但最终四灰阶 payload 相同时，只更新该页面的 `render_state.json`，不创建新版本。

设备按照 `config.toml [display.refresh_schedule].daily_times` 配置的本地时间唤醒，时区由同一配置段的 `timezone` 指定；Manifest 只下发最近一次目标的 UTC Unix 时间戳。未配置每日时间表时，服务端兼容使用 `[display].refresh_interval_seconds` 对齐 UTC 周期。刷新由 Manifest 请求触发，服务端不运行显示后台调度任务；同一设备的刷新通过进程内锁串行执行。已显示的可用设备状态会在 `[display].device_status_min_refresh_seconds` 内保持不变；超过该时间后，温度不足 `1°C`、湿度不足 `3%RH`、电量不足 `5%` 的变化继续沿用旧显示值，避免小幅采样波动造成无意义刷新。

```toml
[display.refresh_schedule]
timezone = "Asia/Shanghai"
daily_times = ["11:15", "11:50"]
```

渲染器能够接收普通 RGB/RGBA 截图，但项目页面应直接使用黑、深灰、浅灰和白四个目标灰阶，并遵守不使用渐变、透明叠层和阴影的 UI 约束；照片例外可开启抖动，文字型页面保持关闭。默认 `demo`、`calendar` 与 `weather` 模板优先使用高对比纯色块和直角边框，灰阶只用于辅助层次。

显示排版遵循以下最小字号规则：包含中文的可见文字不得小于 `12px`，并建议使用 `700` 以上字重；纯英文标签、纯数字、日期和图表坐标可按空间需要使用 `9px` 或 `10px`。完整约束以 [`docs/UI_CONSTRAINTS.md`](docs/UI_CONSTRAINTS.md) 为准。

额度区固定显示 `MCP 每月额度`、`每 5 小时使用额度`、`每周使用额度`。智谱接口当前返回的原始类型顺序是 `TIME_LIMIT`、`TOKENS_LIMIT`、`TOKENS_LIMIT`；后两项无法只靠原始类型区分，因此映射集中维护在 `quota_service.py`，并在响应结构变化时自动停止映射、回退原始名称。

## 保留 API

- `POST /api/v1/ota/check`
- `GET /api/v1/ota/artifacts/<artifact_id>`
- `POST /api/v1/logs/boot`
- `POST /api/v1/logs/batch`
- `POST /api/v1/logs/errors`
- `PUT /api/v1/device/status`
- `GET /api/v1/dashboard`
- `POST /api/v1/assistant/text`
- `POST /api/v1/voice/chat`
- `GET /api/v1/voice/ws`（WebSocket）
- `/api/v2/display/*`

原 devices、weather、settings、quota HTTP API 已删除。`GET /api/v1/dashboard` 为 DeskMate
保留裁剪后的 schema 3 业务投影，并按 `[display.refresh_schedule]` 返回
`next_refresh_at_utc`，使设备只在服务端计划到期时恢复网络。

## 设备拉取式 OTA

OTA 只负责应用固件，服务端不主动推送。设备联网后向 `POST /api/v1/ota/check` 上报
`protocol_version=2`、`product_id`、`firmware_target`、当前应用镜像的 ESP Validation
SHA-256 和上次回滚镜像标识。Hub 按 `firmware_target` 读取目标清单，并验证清单中的产品和
目标身份；仅当清单 `ota_version` 严格更高、制品不同且未被设备判定失败时返回下载目标。
版本字符串只用于诊断。

运行时 OTA 清单由 DeskSuite 发布流程原子更新，不纳入 Git。未设置外部下载地址时，现有
`ds.ps1 ota <product>` 仍把二进制发布到 Hub：

```text
firmwares/
├─ manifests/<firmware_target>.json
└─ artifacts/<artifact_id>.bin
```

清单按固件目标隔离，Hub 本地制品全局按哈希存放和去重。清单中的
`artifacts.app.download_url` 可以指向公开 GitHub Release 的 HTTPS 资产；此时 Hub 只负责
选择目标并返回地址，不要求本地保存或代理该二进制。结构示例见
[`firmwares/manifests/photopainter_esp32s3_v1.example.json`](firmwares/manifests/photopainter_esp32s3_v1.example.json)。
本地固件不通过静态目录公开；下载接口只允许访问至少被一个有效当前清单引用、未设置外部
地址且摘要匹配的 `artifact_id`。OTA 检查与 Hub 本地下载复用 `DEVICE_API_TOKEN`；设备访问
公开 Release 时不发送该 Token。当前公开制品仓库为
<https://github.com/causebefore/desksuite-firmware>。

## 测试

```powershell
uv run pytest -q
```

普通测试即使本机存在真实密钥也不会访问 GLM、MCP 或邮箱。真实测试必须分别显式传入
`--run-live-glm`、`--run-live-mcp` 或 `--run-live-mail`；不要在五分钟内进行十次以上真实
调用，也不要为失败自动重试。
