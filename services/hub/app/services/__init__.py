"""
应用服务层 — 网页渲染、语音、OTA、日志与第三方数据聚合。

各模块职责：
- weather_service.py：和风天气 API 封装（城市搜索、实时、预报、降水、预警）+ 缓存
- device_status_service.py：按设备原子保存温湿度与电池状态
- display_context_service.py：聚合网页渲染数据
- display_page_registry.py：声明页面数据依赖并裁剪可见上下文
- display_refresh_service.py：按设备加锁并协调请求驱动的集合刷新
- display_render_service.py：HTML/JS 截图、四灰阶量化、PPF2 与多页面集合发布
- log_store.py：开发期网络日志落盘（latest.log / sessions / errors）
- ota_service.py：按 firmware_target 读取清单，以单调 ota_version 比对并校验哈希制品
"""
