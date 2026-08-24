# Shared Components

> DeskSuite 可跨产品复用、可独立发布的 ESP-IDF 组件集合。

## 1. 组件清单

| 组件 | 目录 | 版本 |
| --- | --- | --- |
| `desksuite/utils` | `utils/` | 0.1.0 |
| `desksuite/connect` | `communication/connect/` | 0.1.0 |
| `desksuite/network_manager` | `communication/network_manager/` | 0.1.0 |
| `desksuite/transport` | `communication/transport/` | 0.1.0 |
| `desksuite/protocols` | `communication/protocols/` | 0.1.0 |
| `desksuite/time_sync` | `communication/time_sync/` | 0.1.0 |
| `desksuite/firmware_ota` | `communication/tools/firmware_ota/` | 0.1.0 |
| `desksuite/remote_log` | `communication/tools/remote_log/` | 0.1.0 |
| `desksuite/web_console_service` | `services/web_console_service/` | 0.1.0 |
| `desksuite/web_console_network_provider` | `services/web_console_network_provider/` | 0.1.0 |

各目录内的 `idf_component.yml` 是组件唯一清单：`version` 声明组件版本，
内部互依以 `desksuite/<name>` + `override_path` 指向同仓库兄弟目录。

## 2. 引用方式

- DeskSuite 设备工程：通过 `EXTRA_COMPONENT_DIRS` 直接引用本目录（见各设备
  `CMakeLists.txt`），构建时不经组件管理器下载。
- 外部工程：在项目 `main/idf_component.yml` 中以 git 依赖引用任意组件，例如：

  ```yaml
  dependencies:
    desksuite/protocols:
      path: shared/components/communication/protocols
      git: https://example.com/DeskSuite.git
  ```

  组件管理器会按各组件清单中的 `override_path` 递归拉取同仓库的兄弟组件，
  CMake 组件名即目录名，无需命名空间前缀目录。

- 独立编译验证：`examples/standalone/` 是只依赖本目录组件的最小工程，可脱离
  设备工程单独编译。

## 3. 发布约束

- 所有组件遵循 `docs/standards/` 命名与术语规范；公共 C API 使用中文 Doxygen。
- `remote_log` 强制 `CONFIG_LOG_VERSION_2` 与 `CONFIG_LOG_MODE_TEXT`，并通过
  `--wrap=esp_log` 接管日志入口，引用前先阅读其 README。
- `firmware_ota` 的产品身份宏默认为空值，真实身份由宿主工程的
  `firmware_ota_build_project.h` 覆盖头注入。
- 许可证见本目录 [LICENSE](LICENSE)。
