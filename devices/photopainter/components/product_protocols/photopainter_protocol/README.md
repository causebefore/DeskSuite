# `photopainter_protocol`

PhotoPainter 产品协议组件，负责设备状态上传、显示集合清单与 PPF 帧协议。

本组件只实现 PhotoPainter 与 Hub 之间的产品契约，不拥有联网时机、显示事务、重试或电源策略。
通用身份、URL、HTTP 传输能力分别由共享 `protocols` 与 `transport` 组件提供。设备状态、
显示 Manifest 和帧下载接口统一借用 `protocol_backend_context_t`，不再接收可能彼此不一致的
URL、Token 与设备 ID 参数组合。

主要调用方包括 `device_status_upload_app`、`display_collection_service`、
`display_present_service` 与 `photo_playback_app`。协议错误以 `esp_err_t` 返回，产品级降级和
恢复仍由调用方决定。
