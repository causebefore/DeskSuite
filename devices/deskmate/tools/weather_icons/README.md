# DeskMate 静态图片生成工具

该工具由 JSON manifest 驱动，把 SVG 转换为 LVGL I1 静态资源和按
`(key, variant)` 排序的 catalog。天气和状态栏图标共用同一套编码实现。

## 依赖

```powershell
python -m pip install svglib reportlab rlPyCairo pillow
```

## 使用

在本目录执行：

```powershell
python .\generate.py --all
python .\generate.py --all --check
```

`--check` 不修改文件；生成结果与 manifest 或 SVG 不一致时返回非零退出码。

manifest 位于 `manifests\`，路径统一相对 DeskMate 仓库根目录解析。每个 manifest
定义资源包名称、SVG 根目录、输出 `.c/.h`、variant 尺寸和稳定数字 key。

生成文件位于：

```text
esp32\components\ui_platform\images\generated
```

运行时只通过 `ui_platform_image` 查询 catalog；生成文件不包含天气、联网、电池等
业务判断。
