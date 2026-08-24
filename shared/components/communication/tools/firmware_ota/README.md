# `firmware_ota`

> 在调用方已经建立的在线会话中查询、缓存、安装并激活应用固件。

## 定位与边界

- 执行资源：一个独立 FreeRTOS Task、命令队列、状态互斥量和同步停止信号。
- 负责：固件身份上报、`app` 目标解析与缓存、流式写入备用 OTA 分区、完整文件 SHA-256
  校验、启动分区切换和启动确认/回滚。
- 不负责：Wi-Fi 启停、联网等待、产品检查周期、自动安装、显示、深睡、按键或外部存储。

## OTA v2 身份

`firmware_ota_configure_copy()` 复制共享 `protocol_backend_context_t`，从同一值对象取得
`base_url`、可选设备 Token、`product_id`、`firmware_target` 与稳定 `device_id`。
检查和制品下载请求都携带相同的可选 Bearer Token 与 `X-Device-Id`；检查 JSON 中的
`device_id` 也来自该上下文。

`product_id` 与 `firmware_target` 对 DeskSuite 设备的唯一配置源是仓库根 `products.toml`；
统一构建工具把它们写入 `build/generated/firmware_ota_build_project.h` 覆盖头，
`include/firmware_ota_build.h` 会在检测到该覆盖头时采用注入值，否则回落到默认值。
其他工程可自行在 include 路径提供同名覆盖头，注入真实产品身份。

检查固定使用 `POST /api/v1/ota/check`，请求 `protocol_version=2`。Hub 根据
`firmware_target` 选择清单，并再次校验清单内的产品和目标身份。产品隔离不再通过不同 URL
或设备侧路径配置实现。

## 事务约束

`firmware_ota_request_check()` 异步提交一次有界查询并立即返回。有效新目标经过版本、防回滚、
大小和摘要格式校验后，以完整不可变结构缓存在 OTA Runtime，状态进入
`UPDATE_AVAILABLE`，此时尚未下载。

`firmware_ota_request_install()` 只消费已缓存目标。Task 接受安装命令后进入
`DOWNLOADING`，事务不可取消；下载、文件摘要、镜像校验或启动分区切换失败时清除目标、保留
当前启动分区并回到 `IDLE`。安装成功后先发布完成事件，再调用 `esp_restart()`。

完成回调在 OTA Task 上下文、内部锁之外执行，必须快速复制事件并返回，不得重入 OTA 控制
API。组件同一时刻只接受一个事务。

## 身份、版本与回滚

`artifact_id` 使用 ESP 镜像 Validation SHA-256；`file_sha256` 使用完整下载文件 SHA-256；
`ota_version` 是根构建工具嵌入固件的目标内单调递增序号。设备只接受严格高于当前
`ota_version` 的目标，并拒绝当前镜像和上次无效镜像；人工线刷仍可显式降级。

新镜像首次启动处于 `PENDING_VERIFY` 时，产品装配层应在本地关键运行资源就绪后调用
`firmware_ota_confirm_running_image()`。外部网络和 Hub 可用性不属于健康确认门槛。

## 构建与发布

设备脚本已经由 DeskSuite 根构建工具完全取代：

```powershell
& .\ds.ps1 ota photopainter
& .\ds.ps1 ota deskmate
```

构建工具按 `firmware_target` 生成版本头和运行时清单，制品统一写入
`services/hub/firmwares/artifacts/<artifact_id>.bin`。目标清单写入
`services/hub/firmwares/manifests/<firmware_target>.json`，并通过临时文件加原子替换发布。
