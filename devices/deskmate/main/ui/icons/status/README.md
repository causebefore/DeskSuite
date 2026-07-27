# 状态栏 SVG 图标

这些 SVG 是状态栏的唯一视觉源：Wi-Fi 两态、服务端两态、五档电池，以及番茄钟运行、
暂停、完成三态。

不要手改 `components/graphics/ui_platform/images/generated/status_i1_assets.c/.h`。图标变更后执行：

```powershell
python .\tools\weather_icons\generate.py .\tools\weather_icons\manifests\status.json
python .\tools\weather_icons\generate.py .\tools\weather_icons\manifests\status.json --check
```
