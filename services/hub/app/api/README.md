# `app/api` 接口层

本目录负责解析 HTTP/WebSocket 请求、执行公共依赖校验、调用 `app.state` 中的服务，并把结果转换为协议响应。复杂业务逻辑、外部 API 调用、缓存和文件持久化不应写在接口层。

## 文件说明

- `dependencies.py`：解析设备 ID、Bearer Token 等公共请求依赖。
- `assistant.py`：需要显式共享 Token 的最小文字对话入口。
- `dashboard.py`：向 DeskMate 返回经过裁剪的 Dashboard schema 3 业务投影。
- `display.py`：显示集合刷新、Manifest、PPF2 下载和 PNG 预览接口。
- `health.py`：不访问外部服务的容器与进程健康检查接口。
- `device_status.py`：接收设备温湿度和电池状态。
- `ota.py`：应用固件更新检查与当前制品的受控下载。
- `logs.py`：设备启动、批量和错误日志接口。
- `voice.py`：HTTP 语音交互接口。
- `voice_ws.py`：WebSocket 实时语音协议入口。
- `__init__.py`：接口包说明。

## 开发约定

- 请求和响应模型优先引用 `app.schemas`。
- 业务处理委托给 `app.services` 或 `app.workflows`，API 只保留参数传递、异常到状态码的转换和响应头处理。
- `GET /api/v1/dashboard` 的原始业务 JSON 只供 DeskMate Dashboard 使用；顶层
  `next_refresh_at_utc` 与 PhotoPainter Manifest 共用 `[display.refresh_schedule]`，
  但它不改变 PhotoPainter 的显示协议字段。
- PhotoPainter 显示接口仍只返回 Manifest 和最终 PPF2，不通过显示接口暴露业务 JSON。
- OTA 检查和下载必须复用共享设备 Token；禁止重新挂载公开固件静态目录。
- `GET /healthz` 只反映 Hub 进程是否可服务，不探测天气、邮箱、模型或 MCP。
- 三个 `/api/v1/voice/debug/audio/*` 路由仅在
  `[voice.debug_audio].enabled = true` 时注册，并必须复用共享设备 Token。
