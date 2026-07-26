# 和风天气图标源文件

本目录保存当前 UI 原型实际展示过的和风天气填充图标源文件：

- `100-fill.svg`：晴
- `101-fill.svg`：少云
- `104-fill.svg`：阴

固件不直接读取 SVG。通用生成工具根据 manifest 将完整天气代码集转换为
`components/graphics/ui_platform/images/generated/qweather_i1_assets.c/.h`；业务代码到资源 key
的映射保留在 `main/ui/resources/weather_icon_resolver.c`。

重新生成命令：

```powershell
python .\tools\weather_icons\generate.py --all
python .\tools\weather_icons\generate.py --all --check
```

图标来源：QWeather Icons，CC BY 4.0。
