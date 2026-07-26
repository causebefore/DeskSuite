# `deskmate_protocol`

DeskMate 产品协议组件，负责拉取并校验 Hub Dashboard schema 3。

本组件只实现 DeskMate 产品契约，不拥有联网时机、重试退避、缓存、页面状态或 UI 呈现。
通用身份、URL 与 HTTP 传输能力分别由共享 `protocols` 和 `transport` 组件提供。

当前由 `app_network` 同步调用，解析结果交给 `dashboard_store`；协议错误以 `esp_err_t` 返回，
产品级重试、降级和会话释放由 Application 决定。
