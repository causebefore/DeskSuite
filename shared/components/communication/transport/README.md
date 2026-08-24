# Transport

> 同步 HTTP、流式下载/上传和 WebSocket 传输。

## 1. 能力

- `transport_http`：同步 HTTP 请求、流式下载与上传。
- `transport_websocket`：WebSocket 客户端会话。

`include/transport.h` 为聚合头，是使用本组件的推荐入口。

## 2. Kconfig

`Communication - Transport` 菜单提供 HTTP 分阶段耗时诊断日志开关，
只影响诊断输出，不改变传输行为。

## 3. 依赖

- ESP-IDF：esp_common、esp_http_client、esp_timer、freertos、heap。
- Registry：`espressif/esp_websocket_client`。
