# 状态栏 SVG 图标

这些 SVG 是状态栏的唯一视觉源：Wi-Fi 两态、服务端两态和五档电池。

不要手改 `components/graphics/ui_platform/images/generated/status_i1_assets.c/.h`。图标变更后执行：

```powershell
python .\tools\weather_icons\generate.py --all
python .\tools\weather_icons\generate.py --all --check
```
