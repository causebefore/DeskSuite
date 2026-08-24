"""
应用服务层 — 外部数据访问、设备状态、OTA 与日志。

各模块职责：
- weather_service.py：和风天气 API 封装（城市搜索、实时、预报、降水、预警）+ 缓存
- device_status_service.py：按设备原子保存温湿度与电池状态
- calendar_service.py / mail_service.py / quota_service.py / rss_service.py：只读数据源
- log_store.py：开发期网络日志落盘（latest.log / sessions / errors）
- ota_service.py：按 firmware_target 读取清单，以单调 ota_version 比对并选择本地或外部制品

跨服务编排已经移动到 app.workflows，第三方模型与语音协议适配位于 app.providers。
"""
