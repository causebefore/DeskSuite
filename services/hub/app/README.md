# `app` 服务端源码

本目录是 DeskSuite Hub 的 Python 应用包，负责接收设备请求、调用内部数据源、生成 PhotoPainter 显示帧，以及提供 Dashboard、语音、OTA 和日志能力。网页模板不放在这里，而位于 `../web/pages/<page-id>/`。

## 数据流

```text
API 请求
  → DisplayRefreshService（按设备加锁、协调刷新）
  → DisplayContextService（按页面依赖并行取数）
  → DisplayRenderService（裁剪上下文、截图、量化、发布 PPF2）
  → Manifest 与不可变页面文件
```

## 目录与入口

- `main.py`：FastAPI 应用工厂，创建服务、挂载 `app.state` 并注册路由。
- `api/`：HTTP 与 WebSocket 接口层。
- `core/`：配置加载和日志初始化。
- `schemas/`：Pydantic 请求、响应和内部数据模型。
- `services/`：外部数据源、显示、设备、语音、OTA 与日志业务逻辑。
- `__init__.py`：声明 `app` 为 Python 包。

## 新代码放置规则

- 新 HTTP/WebSocket 接口放入 `api/`。
- 新请求、响应或服务间数据结构放入 `schemas/`。
- 新外部数据源或业务流程放入 `services/`。
- 新页面的 HTML/CSS/JavaScript 放入 `../web/pages/<page-id>/`，并在页面注册表声明数据依赖。
- 新服务统一在 `main.py` 创建和注入，避免在 API 函数中临时实例化。

新增、删除或重命名模块后，应同步更新本目录及对应子目录的 `README.md`。
