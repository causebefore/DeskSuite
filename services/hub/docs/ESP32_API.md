# DeskSuite Hub — ESP32 接口

## 1. 边界

PhotoPainter 设备使用设备状态、显示、OTA 和日志接口；DeskMate 使用 Dashboard
schema 3 业务投影、语音、OTA 和日志接口。两种设备共用 Hub 的身份、OTA、
日志和刷新时间表等基础能力。

Base URL 示例：`http://192.168.1.100:8765`。

## 2. 设备身份

设备状态、显示、Dashboard 与语音接口使用以下请求头：

```http
Authorization: Bearer <DEVICE_API_TOKEN>
X-Device-Id: esp32-001122aabbcc
```

固件使用 Wi-Fi Station 基础 MAC 生成稳定的 `esp32-xxxxxxxxxxxx` 设备 ID，并在需要
设备身份的协议中复用。上述四类接口都允许省略 `X-Device-Id`；省略时 Hub 使用
`config.toml [display.defaults].device_id`。OTA 检查和三个日志写入接口只校验 Bearer Token，
设备身份分别来自请求体中的 `device_id`；OTA 制品下载不需要设备 ID。

当服务端 `.env` 中 `DEVICE_API_TOKEN` 为空时，开发期允许省略 Authorization；
一旦配置，所有设备写入和受保护读取都必须提供正确 Bearer Token。

## 3. 设备状态

### 3.1 上传当前状态

设备上传环境 Service 最近一次有效的温湿度与电池测量：

```http
PUT /api/v1/device/status
Content-Type: application/json
```

```json
{
  "environment": {
    "temperature_c": 28.58,
    "humidity_percent": 35.89
  },
  "battery": {
    "percent": 99.6,
    "voltage_mv": 4193
  }
}
```

`battery` 是必需对象。温湿度传感器本轮测量失败时，设备省略整个 `environment` 对象；服务端仍保存电池状态，并在页面上以 `--` 表示室内温湿度不可用。

成功时返回 `204 No Content`，避免下发设备不会使用的响应 JSON。服务端不保留旧 `/api/v1/display/status` 或 `/api/v2/display/status` 路由，也不接受旧的采样窗口、样本数和充电状态字段。上传失败不得阻断后续 Manifest 查询。服务端使用接收时间判断新鲜度，超过 30 分钟的状态不再显示。页面显示温度按 0.5°C、湿度和电量按整数归一化，避免微小波动触发无意义刷新。

## 4. 显示帧

### 4.1 查询当前页面集合

```http
GET /api/v2/display/manifest
```

响应示例：

```json
{
  "device_id": "photopainter-001",
  "protocol_version": 3,
  "format": "PPF2",
  "pixel_format": "GRAY2",
  "collection_version": "20260717-153000",
  "default_page": "demo",
  "width": 800,
  "height": 480,
  "bits_per_pixel": 2,
  "header_size": 32,
  "payload_size": 96000,
  "file_size": 96032,
  "pages": [
    {
      "page_id": "demo",
      "content_version": "20260717-152500",
      "file_size": 96032,
      "crc32": "b96a4242",
      "sha256": "21bd67ae354909cc52b4b40485ea6e2c89ae6203a7591e9653a02d56e1291322",
      "payload_sha256": "5ab9fdf0b635a4f4d8fdc8ca60ed737f4ba9715f85d3d453736bf5f170631c10",
      "created_at": "2026-07-17T07:25:00Z",
      "frame_url": "/api/v2/display/frame/demo/20260717-152500.ppf",
      "preview_url": "/api/v2/display/preview/demo/20260717-152500.png"
    },
    {
      "page_id": "calendar",
      "content_version": "20260717-153000",
      "file_size": 96032,
      "crc32": "87654321",
      "sha256": "11bd67ae354909cc52b4b40485ea6e2c89ae6203a7591e9653a02d56e1291333",
      "payload_sha256": "6ab9fdf0b635a4f4d8fdc8ca60ed737f4ba9715f85d3d453736bf5f170631c11",
      "created_at": "2026-07-17T07:30:00Z",
      "frame_url": "/api/v2/display/frame/calendar/20260717-153000.ppf",
      "preview_url": "/api/v2/display/preview/calendar/20260717-153000.png"
    }
  ],
  "created_at": "2026-07-17T07:30:00Z",
  "next_refresh_at": 1784275200
}
```

`next_refresh_at` 是 Manifest v3 的必填字段，表示设备下一次刷新目标的 UTC Unix 时间戳
（秒）。服务端在 `config.toml [display.refresh_schedule].timezone` 指定的时区中，从
`daily_times` 选择严格晚于当前时刻的最近时间；当天计划已结束时选择次日第一个时间。
未配置每日时间表时，兼容按 `[display].refresh_interval_seconds` 对齐 UTC 时间轴。设备应在
实际进入深睡前，用可信系统时间换算内部定时器间隔，避免下载、显示和交互窗口造成累积
漂移。Manifest v3 不再下发相对轮询周期，缺失或非法的绝对时间必须视为协议错误。

每次查询时，服务端根据配置页面的数据依赖并行获取必要数据，再按 `config.toml [display].pages` 逐页判断是否需要渲染。每个页面拥有独立 `content_version`：邮件变化只更新 `demo`；天气变化更新 `demo` 和 `weather`，`calendar` 可复用本地文件；日历变化会同时更新展示日程的 `demo` 和 `calendar`。服务端 ETag 由集合版本与本轮 `next_refresh_at` 共同组成；设备必须回传完整 ETag，同一调度窗口重复请求时可收到 `304`。首次生成失败时返回 `503`；已有集合时若本轮任一页面生成失败，服务端继续提供上一完整集合，并重新计算下一次绝对刷新时间。

显示刷新完全由设备在计划时间发起的 Manifest 请求驱动，服务端不运行后台显示调度任务。同一设备的并发刷新在服务端串行执行，不同设备相互独立。

同步顺序固定为：十秒平均采样 → 上传状态 → 查询集合 Manifest → 逐页下载本地缺少的 PPF2 → 全部校验通过后原子启用集合 → 显示默认页面。任一页面失败时不得提交不完整集合。

### 4.2 下载帧

```http
GET /api/v2/display/frame/{page_id}/{content_version}.ppf
```

响应为 `application/octet-stream`，每个文件固定总长 96032 字节。设备应逐个流式写入 SD 卡临时文件，完成 PPF2、CRC32 与 SHA-256 校验后再原子重命名。页面文件由 `(page_id, content_version)` 唯一标识。

### 4.3 PPF2/GRAY2 文件格式

所有多字节整数均为小端序：

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | ASCII `PPF2` |
| 4 | 1 | format version，固定为 `2` |
| 5 | 1 | bits per pixel，固定为 `2` |
| 6 | 2 | header size，固定为 `32` |
| 8 | 2 | width，固定为 `800` |
| 10 | 2 | height，固定为 `480` |
| 12 | 4 | payload size，固定为 `96000` |
| 16 | 4 | payload CRC32 |
| 20 | 8 | content version |
| 28 | 4 | reserved，当前为零 |
| 32 | 96000 | GRAY2 payload |

payload 按行从上到下、每行从左到右，每行正好 200 字节。每字节依次存放四个像素，位组 `7..6` 对应最左像素，随后为 `5..4`、`3..2` 和 `1..0`。像素值 `0` 表示黑、`1` 表示深灰、`2` 表示浅灰、`3` 表示白，因此全黑帧 payload 全为 `0x00`，全白帧全为 `0xFF`。CRC32 只覆盖 96000 字节 payload，Manifest 中的 SHA-256 覆盖完整 96032 字节文件。

## 5. OTA

### 5.1 检查更新

```http
POST /api/v1/ota/check
Content-Type: application/json
```

```json
{
  "protocol_version": 2,
  "product_id": 1,
  "firmware_target": "photopainter_esp32s3_v1",
  "device_id": "photopainter-001",
  "artifacts": {
    "app": {
      "current_version": "1.0.0",
      "ota_version": 1773960000000,
      "current_artifact_id": "<64 位小写 SHA-256>",
      "last_invalid_artifact_id": null
    }
  }
}
```

Hub 先按 `firmware_target` 选择清单，再要求清单中的 `product_id` 和 `firmware_target`
与请求完全一致。只有清单的 `ota_version` 严格大于设备当前值，且 `artifact_id` 既不是
当前镜像也不是上次回滚镜像时，响应的 `updates.app` 才包含下载目标。无更新或拒绝降级时
`updates` 为空对象。

```json
{
  "protocol_version": 2,
  "updates": {
    "app": {
      "version": "1.0.1",
      "ota_version": 1773961000000,
      "artifact_id": "<64 位小写 SHA-256>",
      "file_sha256": "<64 位小写 SHA-256>",
      "size": 1215568,
      "url": "/api/v1/ota/artifacts/<artifact_id>"
    }
  }
}
```

当前固件目标：

| `product_id` | 产品 | `firmware_target` |
| ---: | --- | --- |
| `1` | PhotoPainter | `photopainter_esp32s3_v1` |
| `2` | DeskMate | `deskmate_esp32s3_v1` |

清单保存在 `firmwares/manifests/<firmware_target>.json`，所有目标共享
`firmwares/artifacts/<artifact_id>.bin` 哈希制品库。下载接口只暴露有效当前清单引用的制品。

## 6. 日志

所有日志上报请求都携带正整数 `product_id`，产品编号从 1 连续递增：

| `product_id` | 产品 |
| ---: | --- |
| `1` | PhotoPainter |
| `2` | DeskMate |

产品内再以 `device_id` 区分设备。服务端存储路径为
`runtime_logs/products/<product_id>/devices/<device_id>/`，每台设备独立保留 `latest`、错误汇总和最近会话，日志不会互相覆盖。

三个日志写入接口均要求 `Authorization: Bearer <DEVICE_API_TOKEN>`，并从请求体的
`device_id` 识别设备，不要求 `X-Device-Id`。Hub 校验请求体中的 `device_id`
数据格式；日志查询接口不要求设备 Token。

- `POST /api/v1/logs/boot`：创建启动日志会话。请求包含 `product_id`、`device_id`、`firmware_version`、`reset_reason`、`ip`。
- `POST /api/v1/logs/batch`：批量上报运行日志。请求包含 `product_id`、`session_id`、`device_id`、`lines`。
- `POST /api/v1/logs/errors`：上报 NVS 中持久化错误。请求包含 `product_id`、`session_id`、`device_id`、`errors`。

所有新请求都必须显式携带 `product_id`，Hub 不从设备名称或路由推断产品。

查询接口：

- `GET /api/v1/logs/products`：列出已有日志的产品及设备数量。
- `GET /api/v1/logs/products/{product_id}/devices`：列出该产品的设备和最新会话。
- `GET /api/v1/logs/products/{product_id}/devices/{device_id}/sessions`：列出该设备的会话。
- `GET /api/v1/logs/products/{product_id}/devices/{device_id}/sessions/{session_id}?offset=0&limit=200`：分页读取结构化日志事件。

字段结构以服务端 OpenAPI `/docs` 为准。

## 7. 语音

- `POST /api/v1/voice/chat`：上传单声道 16-bit raw PCM，采样率通过 `X-Audio-Sample-Rate` 指定。
- `GET /api/v1/voice/ws`：WebSocket 语音会话。

下行二进制帧格式：`[1 字节 type][4 字节 big-endian payload length][payload]`。

| type | 含义 |
| --- | --- |
| `0x00` | END |
| `0x01` | ASR_TEXT，UTF-8 |
| `0x02` | REPLY_TEXT，UTF-8 |
| `0x03` | TTS_PCM，24kHz 单声道 16-bit PCM |
| `0x04` | THINKING 心跳 |
| `0x80` | ERROR，UTF-8 |

## 8. DeskMate Dashboard

```http
GET /api/v1/dashboard
Authorization: Bearer <DEVICE_API_TOKEN>
X-Device-Id: <稳定设备 ID>
```

建议固件显式携带稳定的 `X-Device-Id`；省略时 Hub 使用
`config.toml [display.defaults].device_id`。

响应保持 schema 3 的天气、日历、邮件和额度投影，并在顶层返回：

```json
{
  "schema": 3,
  "device_id": "esp32-001122aabbcc",
  "generated_at": "2026-07-26T02:15:00Z",
  "next_refresh_at_utc": 1785034800
}
```

`next_refresh_at_utc` 是严格晚于 `generated_at` 的 UTC Unix 时间戳（秒），与显示 Manifest
共用 `config.toml [display.refresh_schedule]`。设备在普通内部 Timer 唤醒时保持离线，只有
可信系统时间达到该截止时间后才恢复网络并再次拉取 Dashboard；成功响应会替换下一截止时间。

Dashboard 的 `mail.messages` 保持最多 5 条，但专门按 DeskMate 浏览顺序投影：先按时间倒序
返回未读邮件，不足上限时再按时间倒序补入已读邮件。`mail.unread_count` 仍表示收件箱完整
未读总数，不受列表上限影响。共享显示页面使用的邮件数据仍保持“最近邮件”顺序。

## 9. 已删除接口

以下路径不再提供，调用返回 `404`：

- `/api/v1/devices/*`
- `/api/v1/weather/*`
- `/api/v1/settings`
- `/api/v1/quota/*`
