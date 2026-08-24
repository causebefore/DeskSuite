# Protocols

> 稳定硬件设备 ID、统一后端上下文、URL 与语音/日志上传等通用协议事实。

## 1. 能力

- `protocol_identity`：稳定硬件设备 ID 及 HTTP/WebSocket 身份头。
- `protocol_backend_context`：统一后端上下文的构造与校验。
- `protocol_url`：后端 URL 拼装。
- `voice_protocol`：语音流解码器。
- `log_upload`：日志批次上传协议。

`include/protocols.h` 为聚合头，是使用本组件的推荐入口。

## 2. 边界

产品专有协议（如 PhotoPainter 显示帧、DeskMate Dashboard schema）不属于本组件；
它们由各产品在自己的 `product_protocols` 中定义，可以依赖本组件，反向不允许。

## 3. 依赖

- ESP-IDF：esp_common、esp_hw_support、esp_timer、heap。
- 组件：`desksuite/transport`。
- Registry：`espressif/cjson`。
