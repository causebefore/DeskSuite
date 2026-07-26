# QWeather Icons v1.8.0

本目录固定保存 PhotoPainter 显示模板所用的和风天气官方 SVG 图标。

- 官方使用文档：https://icons.qweather.com/usage/
- 官方源码：https://github.com/qwd/Icons/tree/v1.8.0
- 版本：v1.8.0
- 代码许可：MIT（见 `LICENSE`）
- 图标许可：Creative Commons Attribution 4.0 International（CC BY 4.0）
- 图标版权：QWeather 和风天气

显示渲染服务根据和风天气 API 的 `icon`、`iconDay` 字段读取 `icons/<code>.svg`，再把实际用到的 SVG 内嵌到 HTML。不要改为 CDN 地址：无头 Chromium 截图阶段会阻止所有网络请求。
