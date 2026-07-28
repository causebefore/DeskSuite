# DeskSuite Repository Guidelines

## 项目边界

- `devices/photopainter/` 是 PhotoPainter ESP32-S3 固件。
- `devices/deskmate/` 是 DeskMate ESP32-S3 固件。
- `services/hub/` 是两类设备共用的 DeskSuite Hub。
- 修改子项目时，同时遵守该目录内更具体的 `AGENTS.md`。

## 固定命令

Agent 默认不主动编译固件；只有用户明确要求编译时，才可在 DeskSuite 根目录执行统一脚本：

```powershell
& .\ds.ps1 build photopainter
```

```powershell
& .\ds.ps1 build deskmate
```

不得绕过脚本直接调用 `idf.py`、`cmake` 或 `ninja`。Hub 的测试在 `services/hub/` 中使用：

```powershell
uv run pytest -q
```

## 跨项目约定

- 日志、OTA、设备身份和基础传输属于通用能力，不应绑定到单一产品名称。
- 共享设备通信源码只保留在 `shared/components/communication/`；PhotoPainter 显示帧、
  DeskMate Dashboard 等产品契约保留在各自 `components/product_protocols/`。
- 新增或重命名项目自有 C/C++ 公共 API、跨文件接口、生命周期、并发、所有权或跨模块领域
  术语前，必须阅读 `docs/standards/c_cpp_naming_conventions.md` 和
  `docs/standards/c_cpp_terminology.md`。受控术语表不存在所需术语时，必须在同一任务中先
  补充术语，并向用户说明新增理由、适用边界和推荐示例，禁止静默创造近义词。
- 修改跨设备 API、数据流或持久化语义前，先阅读相关设备的根 README 和架构规范。
- 所有项目自有运行日志和错误信息使用中文，公共 C/C++ API 使用中文 Doxygen。
- 不提交 `.env`、运行日志、显示帧、固件二进制、构建目录、虚拟环境或本地缓存。

## Git

默认开发分支为 `dev`。提交格式统一为 `type(scope): 中文描述`。每次提交只包含当前任务的
文件和代码块，不得混入来源不明的工作区修改。
