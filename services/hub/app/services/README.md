# `app/services` 业务服务层

本目录实现外部数据访问、显示渲染、设备状态、语音、OTA 和日志业务。API 层只调用这些服务，不直接复制业务逻辑。

## 外部数据服务

- `weather_service.py`：和风天气实况、7 日预报、分钟降水、预警、空气质量和月相查询，以及分层缓存、旧月相缓存与 mock 降级。
- `calendar_service.py`：iCloud CalDAV 近期/自然月日程查询、缓存和降级。
- `mail_service.py`：QQ 邮箱 IMAP 只读摘要、缓存和降级。
- `rss_service.py`：使用 `feedparser` 聚合可配置 RSS/Atom 订阅源，并处理超时、缓存和旧数据降级。
- `quota_service.py`：智谱额度查询和名称映射。
- `dashboard_service.py`：并发复用天气、日历、邮件和额度单例，投影并裁剪
  DeskMate Dashboard schema 3；单源超时或异常只降级对应数据块，并复用显示时间表计算
  下一次联网刷新时间。
- `memory_service.py`：长期记忆查询、写入和可选依赖管理。

外部数据服务应统一处理超时、缓存、异常日志和可用的 mock/旧数据降级，不把失败细节泄露给 ESP32。

## 显示与设备服务

- `display_page_registry.py`：声明页面依赖、计算数据源并集并裁剪页面上下文。
- `display_refresh_service.py`：按设备加锁，协调请求驱动的取数和集合刷新。
- `display_context_service.py`：把各业务模型转换为网页可消费的上下文。
- `display_render_service.py`：内嵌公共墨水屏 CSS 与本地中文字体，完成 HTML/JS 截图、四灰阶量化、PPF2 和多页面 Manifest 原子发布。
- `device_status_service.py`：按设备原子保存和读取温湿度、电池状态。

显示页面模板位于 `../../web/pages/<page-id>/`，当前包括综合简报 `demo`、今日日程 `calendar`、整月概览 `month-calendar`、天气 `weather`、月相 `moon` 和订阅阅读 `rss`。新增页面时，应同时增加模板目录和页面注册定义，不要把 HTML/CSS/JavaScript 放进服务层。

页面中文正文的最小字号为 `12px`。纯英文、纯数字、日期和图表坐标可以使用 `9px` 或 `10px`；包含中文的标签、提示、地点和摘要不得低于 `12px`。模板先直接使用本地 TRMNL Framework 的布局与组件，再由 `config.toml [display].shared_css` 指定的约束补丁统一四灰阶和中文可读性；字体由 `font_file` 指定并由渲染服务内嵌。模板不要创建与 TRMNL 同义的公共样式，也不要引用系统字体路径或在线字体。

## 语音服务

- `voice_service.py`：ASR、LLM、工具调用、TTS 和记忆集成。
- `voice_protocol.py`：WebSocket 消息与音频协议辅助逻辑。

## OTA 与日志

- `ota_service.py`：按不可变制品标识读取 OTA 运行时清单、校验文件并定位当前固件。
- `log_store.py`：运行日志落盘、轮转和查询。
- `__init__.py`：服务包总览。

新增、删除或重命名服务后，应同步更新本文件和 `../README.md`。
