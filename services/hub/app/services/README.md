# `app/services` 业务服务层

本目录实现外部数据访问、设备状态、OTA 和日志等单一能力。跨服务编排位于 `app/workflows/`，供应商协议适配位于 `app/providers/`。

## 外部数据服务

- `weather_service.py`：和风天气实况、7 日预报、分钟降水、预警、空气质量和月相查询；和风请求在原超时预算内最多尝试两次，天气分项与月相回源失败时优先保留旧数据。无旧天气数据时使用不入缓存的临时空分项，API 明确返回的合法空结果仍正常缓存。
- `calendar_service.py`：iCloud CalDAV 近期/自然月日程查询、缓存和降级。
- `mail_service.py`：QQ 邮箱 IMAP 只读摘要、缓存和降级。
- `rss_service.py`：使用 `feedparser` 聚合可配置 RSS/Atom 订阅源，并处理超时、缓存和旧数据降级。
- `quota_service.py`：智谱额度查询和名称映射。

外部数据服务应统一处理超时、缓存、异常日志和可用的 mock/旧数据降级，不把失败细节泄露给 ESP32。

## 设备服务

- `device_status_service.py`：按设备原子保存和读取温湿度、电池状态。

显示页面模板位于 `../../web/pages/<page-id>/`，当前包括综合简报 `demo`、今日日程 `calendar`、整月概览 `month-calendar`、天气 `weather`、月相 `moon` 和订阅阅读 `rss`。新增页面时，应同时增加模板目录和页面注册定义，不要把 HTML/CSS/JavaScript 放进服务层。

页面中文正文的最小字号为 `12px`。纯英文、纯数字、日期和图表坐标可以使用 `9px` 或 `10px`；包含中文的标签、提示、地点和摘要不得低于 `12px`。模板先直接使用本地 TRMNL Framework 的布局与组件，再由 `config.toml [display].shared_css` 指定的约束补丁统一四灰阶和中文可读性；字体由 `font_file` 指定并由渲染服务内嵌。模板不要创建与 TRMNL 同义的公共样式，也不要引用系统字体路径或在线字体。

## OTA 与日志

- `ota_service.py`：按不可变制品标识读取 OTA 运行时清单、校验文件并定位当前固件。
- `log_store.py`：运行日志落盘、轮转和查询。
- `__init__.py`：服务包总览。

Display、Dashboard、Assistant 和 Voice 的数据流见 `../workflows/` 中各自的中文
README。新增、删除或重命名服务后，应同步更新本文件和 `../README.md`。
