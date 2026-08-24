# Network Manager

> Wi-Fi/Portal 技术状态机、候选配置验证和单轮技术重试。

## 1. 能力

- 通过 `network_manager_config_store_t` 注入配置加载、保存和清除回调。
- 候选配置只有在 STA 获得 IPv4 后才提交；验证失败时保持 Portal 和原有 active 配置。
- `network_manager_get_portal_info_copy()` 在 Portal 尚未激活时仍返回 `ESP_OK`
  与 `active=false` 的零值快照，这是瞬时等待条件，不是错误。
- `network_manager_stop()` 同步回收 Portal、DNS、扫描、回调和 Wi-Fi Driver。

## 2. 主要接口

`network_manager_init_borrow()`、`network_manager_start()`、`network_manager_stop()`、
`network_manager_get_status_copy()`、`network_manager_get_diagnostics_copy()`、
`network_manager_request_start_portal()` 等，见 `include/network_manager.h`。

本组件不决定联网时机与产品重试策略，这些决策由应用层拥有。

## 3. 依赖

- ESP-IDF：esp_common、freertos。
- 组件：`desksuite/connect`、`desksuite/utils`。
