# RLCD 字体工具

这个目录包含外置字库生成脚本、字体源、本地 `lv_font_conv` 依赖和生成产物。
脚本默认会按文件名自动发现这些输入，也支持命令行参数覆盖，不再依赖写死的目录。

## 目录约定

- `fronts/`：当前字体源目录。
- `output/gb2312_chars.txt`：当前字符集文件。
- `output/font.bin`：当前默认输出文件。
- `output/flash_font.txt`：根据分区表自动生成的烧录提示。

默认情况下，脚本会在 `tools/fonts2bin/` 下按文件名搜索：

- `AlibabaPuHuiTi-3-55-Regular.ttf`
- `AlibabaPuHuiTi-3-75-SemiBold.ttf`
- `JetBrainsMono-Regular.ttf`
- `gb2312_chars.txt`

如果不传 `--output`，会把 `font.bin` 输出到字符集文件所在目录。

## 用法

从仓库根目录运行：

```powershell
python tools\fonts2bin\gen_font_bin.py --partitions partitions.csv
```

或在工具目录下运行：

```powershell
cd tools\fonts2bin
npm run generate
```

常用可选参数：

```powershell
python tools\fonts2bin\gen_font_bin.py --fonts-dir custom_fonts --charset custom_chars.txt --output build\font.bin
```

TTF 和 `font.bin` 都是本地文件，发布固件前需要自行确认字体授权。

## 安装 lv_font_conv

若 PATH 中没有全局 `lv_font_conv`，脚本会优先使用本目录 `node_modules/.bin` 下的本地依赖：

```powershell
& npm.cmd --prefix tools/fonts2bin install
```
