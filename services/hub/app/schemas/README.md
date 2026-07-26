# `app/schemas` 数据模型

本目录使用 Pydantic 定义 HTTP 请求、HTTP 响应和服务间传递的数据结构。Schema 负责字段、类型和校验规则，不负责联网、缓存、业务编排或文件存储。

## 文件说明

- `weather.py`：地点、实时天气、预报和空气质量模型。
- `moon.py`：月升、月落、逐小时月相和旧缓存状态模型。
- `calendar.py`：日历事件和日程集合模型，包含 UTC 起止时间及设备时区下的日期/时间字段。
- `mail.py`：邮件条目和邮箱摘要模型。
- `rss.py`：RSS/Atom 订阅源摘要、文章和聚合载荷模型。
- `quota.py`：智谱额度及剩余额度模型。
- `dashboard.py`：DeskMate Dashboard schema 3 的四领域设备协议模型，并携带服务端计划的
  下一次联网刷新 UTC 时间戳。
- `device_status.py`：设备环境与电池状态模型。
- `display.py`：显示渲染请求、页面 Manifest 和集合 Manifest。
- `ota.py`：通用 OTA 制品状态、应用固件目标和运行时清单模型。
- `logs.py`：设备日志请求和查询结果。
- `__init__.py`：Schema 包说明。

新增模型时应保持字段语义明确，并为边界、兼容性或行为变化补充测试。ESP32 不需要的业务数据不得加入显示 Manifest。
