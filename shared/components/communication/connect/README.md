# Connect

> Wi-Fi STA/SoftAP、配网 Portal、DNS、扫描与完整链路快照的唯一所有者。

## 1. 能力

- Wi-Fi Driver 与 STA/SoftAP 生命周期的串行拥有。
- 配网 Portal：HTTP 表单、DNS 劫持、扫码二维码渲染（依赖 `espressif/qrcode`）。
- 后台扫描任务与扫描结果快照。
- `connect_get_link_snapshot_copy()` 输出完整链路快照，供上层诊断展示。

## 2. 主要接口

`connect_init()`、`connect_request_start_station_copy()`、
`connect_request_start_station_with_portal_copy()`、`connect_start_portal_copy()`、
`connect_stop()`、`connect_deinit()`、`connect_get_link_snapshot_copy()` 等，
见 `include/connect.h`。

## 3. Kconfig

`Communication - Connect` 菜单提供 Portal 热点名称、密码和默认服务地址；
均为通用默认值，产品应在项目级 `sdkconfig.defaults` 覆盖。

## 4. 依赖

- ESP-IDF：esp_wifi、esp_event、esp_netif、esp_http_server、lwip、freertos。
- 组件：`desksuite/utils`。
- Registry：`espressif/qrcode`。
