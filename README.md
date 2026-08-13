# DeskSuite

DeskSuite 是面向桌面智能设备的统一项目，包含两条设备产品线和一个共享后端：

- `PhotoPainter`：低功耗墨水屏信息终端。
- `DeskMate`：带本地交互界面的桌面终端。
- `DeskSuite Hub`：为设备提供身份、日志、OTA、语音和产品业务数据等后端能力。

## 目录结构

```text
DeskSuite/
├─ build_tools/             # 多产品 ESP-IDF 构建与 OTA 发布工具
├─ docs/
│  └─ standards/            # 全仓嵌入式 C/C++ 术语与命名规范
├─ devices/
│  ├─ photopainter/       # PhotoPainter ESP32-S3 固件
│  └─ deskmate/           # DeskMate ESP32-S3 固件
├─ shared/
│  └─ components/
│     ├─ communication/                 # 两套固件共用的唯一 Communication 源码
│     └─ services/
│        ├─ web_console_service/        # 可裁剪、Provider 驱动的本地网页控制台
│        └─ web_console_network_provider/ # Network Manager 到 Console 的可选只读适配
├─ services/
│  └─ hub/                # DeskSuite Hub（FastAPI）
├─ products.toml           # 产品 ID、固件目标、工程和默认串口
├─ ds.ps1                  # 唯一设备构建入口
└─ README.md
```

两套固件共享 Wi-Fi/Portal、网络状态机、HTTP/WebSocket、设备身份、远端日志和 OTA 实现。
`web_console_service` 与 Communication 可随产品一起移植；需要网络诊断时由产品额外组合
`web_console_network_provider`。依赖只从该 Provider 指向 Console 与 `network_manager`，
Console Core 和 Communication 均不反向依赖它，PhotoPainter 也无需发现或链接这两个组件。
PhotoPainter 显示/状态协议与
DeskMate Dashboard 协议分别位于设备目录下的 `components/product_protocols/`，不进入共享目录。

## 常用命令

### DeskSuite Hub

```powershell
Set-Location .\services\hub
uv sync --extra dev
uv run pytest -q
.\start_server.ps1
```

### 设备固件

```powershell
& .\ds.ps1 build photopainter
& .\ds.ps1 build deskmate
& .\ds.ps1 flash photopainter
& .\ds.ps1 monitor deskmate
```

所有设备命令都在 DeskSuite 根目录执行；产品名是必填参数。可用命令还包括
`flash-monitor`、`clean`、`menuconfig`、`set-target-s3`、`build-log`、`flash-log` 和
`monitor-log`。产品元数据集中定义于 [`products.toml`](products.toml)，构建工具会把同一份
`product_id` 与 `firmware_target` 写入设备构建头，避免设备请求身份与发布清单漂移。

```powershell
& .\ds.ps1 ota photopainter
& .\ds.ps1 ota deskmate
```

`ota` 会先生成该 `firmware_target` 独立的单调版本头，再构建并发布。省略
`-ServiceRoot` 时，默认按 [`products.toml`](products.toml) 的 `[ota_publish]` 配置，通过
SSH 与 Docker 发布到 Ubuntu 生产 Hub；只有显式指定 `-ServiceRoot` 时才改为本地 Hub
目录发布。Hub 运行时使用以下目录结构：

```text
firmwares/
├─ manifests/
│  ├─ photopainter_esp32s3_v1.json
│  └─ deskmate_esp32s3_v1.json
└─ artifacts/
   └─ <artifact_id>.bin
```

需要在本地 Hub 目录验证发布时，显式执行：

```powershell
& .\ds.ps1 ota photopainter -ServiceRoot .\services\hub
& .\ds.ps1 ota deskmate -ServiceRoot .\services\hub
```

清单按固件兼容目标隔离，二进制在全局制品库中按哈希去重。设备统一请求
`POST /api/v1/ota/check`，通过 `product_id` 与 `firmware_target` 选择并双重校验清单。
各目标的本地单调版本状态保存在 `.build-state/ota/<firmware_target>.version`，不进入 Git。

## 开发规范

- [嵌入式 C/C++ 术语与命名规范](docs/standards/c_cpp_naming_conventions.md)
- [嵌入式 C/C++ 受控术语表](docs/standards/c_cpp_terminology.md)

两份文档面向嵌入式 C 公共 API 和 C++ 私有实现。引入受控术语表中不存在的公共动作词、
生命周期/并发/所有权名词或跨模块领域词时，必须先登记术语，并向用户说明新增理由和适用边界。

## 命名约定

| 范围 | 正式名称 | 目录或包名 |
| --- | --- | --- |
| 总项目 | DeskSuite | `DeskSuite` |
| 统一后端 | DeskSuite Hub | `services/hub`、`desksuite-hub` |
| 墨水屏设备 | PhotoPainter Device | `devices/photopainter` |
| 交互设备 | DeskMate Device | `devices/deskmate` |
