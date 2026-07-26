# DeskSuite

DeskSuite 是面向桌面智能设备的统一项目，包含两条设备产品线和一个共享后端：

- `PhotoPainter`：低功耗墨水屏信息终端。
- `DeskMate`：带本地交互界面的桌面终端。
- `DeskSuite Hub`：为设备提供身份、日志、OTA、语音和产品业务数据等后端能力。

## 目录结构

```text
DeskSuite/
├─ devices/
│  ├─ photopainter/       # PhotoPainter ESP32-S3 固件
│  └─ deskmate/           # DeskMate ESP32-S3 固件
├─ services/
│  └─ hub/                # DeskSuite Hub（FastAPI）
└─ README.md
```

各子项目仍保留独立的构建、配置和架构文档。跨产品通用能力应优先放在 Hub 的公共边界中，
产品专属显示和交互逻辑仍由对应设备或产品模块拥有。

## 常用命令

### DeskSuite Hub

```powershell
Set-Location .\services\hub
uv sync --extra dev
uv run pytest -q
.\start_server.ps1
```

### PhotoPainter Device

```powershell
Set-Location .\devices\photopainter
& .\build_tools\dm.ps1 build
& .\build_tools\dm.ps1 ota
```

`ota` 默认把固件发布到本项目的 `services\hub\firmwares\`。

### DeskMate Device

```powershell
Set-Location .\devices\deskmate
& .\dm.ps1 build
& .\dm.ps1 ota
```

## 命名约定

| 范围 | 正式名称 | 目录或包名 |
| --- | --- | --- |
| 总项目 | DeskSuite | `DeskSuite` |
| 统一后端 | DeskSuite Hub | `services/hub`、`desksuite-hub` |
| 墨水屏设备 | PhotoPainter Device | `devices/photopainter` |
| 交互设备 | DeskMate Device | `devices/deskmate` |

历史协议字段、设备产品 ID 和固件内部前缀暂不因目录迁移而改变，避免破坏现有设备兼容性。
