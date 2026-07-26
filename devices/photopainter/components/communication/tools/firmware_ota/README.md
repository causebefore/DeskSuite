# `firmware_ota`

> 在调用方已经建立的在线会话中查询、缓存、安装并激活应用固件。

## 定位与边界

- 层级：`components/communication/tools` 下的可迁移通信工具。
- 执行资源：一个独立 FreeRTOS Task、命令队列、状态互斥量和同步停止信号。
- 负责：固件身份上报、`app` 目标解析与缓存、流式写入备用 OTA 分区、完整文件 SHA-256
  校验、启动分区切换和启动确认/回滚。
- 不负责：Wi-Fi 启停、联网等待、内容刷新、显示、深睡策略、按键、SD 卡或外部 Flash。

## 事务约束

`firmware_ota_request_check()` 异步提交一次有界 HTTP 查询并立即返回，返回值只表示命令是否
成功入队。有效的新目标经过版本、防回滚和摘要格式校验后，以完整不可变结构缓存在 OTA
Runtime，状态进入 `UPDATE_AVAILABLE`，不会下载。服务端响应、传输、HTTP、解析或身份错误通过
`FIRMWARE_OTA_EVENT_CHECK_COMPLETED` 完成事件返回。

`firmware_ota_request_install()` 只消费已经缓存的目标并立即返回提交结果。Task 接受安装命令后
进入 `DOWNLOADING`，事务不可取消；下载、文件摘要、镜像校验或启动分区切换失败时清除目标、
保留当前启动分区并回到 `IDLE`，通过 `FIRMWARE_OTA_EVENT_INSTALL_COMPLETED` 返回错误，必须
重新检查才能重试。安装成功后先发布完成事件，再直接调用 `esp_restart()`。

完成回调通过 `firmware_ota_set_event_callback_borrow()` 注册，在 OTA Task 上下文、内部锁之外
执行。回调必须快速复制事件并返回，不能重入 OTA 控制 API。组件同一时刻只接受一个事务；
状态不允许时返回 `ESP_ERR_INVALID_STATE`，命令队列满时明确返回 `ESP_ERR_TIMEOUT`，不会静默
丢弃。

`firmware_ota_discard_pending_update()` 在 `UPDATE_AVAILABLE` 清除目标并回到 `IDLE`，在
`IDLE` 幂等成功；检查、下载和等待重启阶段拒绝调用。Tool 不保存产品模式，也不周期轮询。

## 身份与回滚

`artifact_id` 使用 ESP 镜像 Validation SHA-256；`file_sha256` 使用完整下载文件 SHA-256；
`ota_version` 是构建脚本嵌入固件的单调递增构建序号。设备端要求目标严格高于当前版本，并拒绝
当前镜像与上次无效镜像；人工线刷仍可显式降级。

新镜像首次启动处于 `PENDING_VERIFY` 时，装配层在本地关键资源与 Task 就绪后调用
`firmware_ota_confirm_running_image()`。在此之前发生致命本地故障，应调用
`firmware_ota_reject_running_image_and_reboot()`；外部网络、服务端、SD 卡和传感器可用性不属于
确认门槛。

## 生命周期

```text
init → start
    → set_event_callback_borrow + configure_copy
    → request_check → CHECKING ──完成事件──┐
        ├── 无更新或失败 → IDLE            │
        └── 有更新 → UPDATE_AVAILABLE       │
                         ├── discard_pending_update → IDLE
                         └── request_install → DOWNLOADING
                                      └── 完成事件 → AWAITING_RESTART / IDLE
    → stop → deinit
```

允许先启动尚未配置服务地址的空闲 Task，以便启动健康确认不依赖外部网络；仅在 `STOPPED` 或
`IDLE` 复制服务配置。完成回调可以在 `STOPPED`、`IDLE` 或 `UPDATE_AVAILABLE` 替换，但检查和
安装提交前必须已经注册。`stop()` 仍是同步操作，以便调用方在休眠或停机前确认 Task 已经退出；
检查和安装则是异步事务。公共接口保持 C ABI。
