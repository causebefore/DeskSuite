# Sys 边界

`components/sys` 是离线即可成立的同步系统能力组件。

来自 PhotoPainter 的通用模块：

- `system_storage`：网络配置、时区、RTC 告警和 OTA 显示恢复状态。
- `system_clock`：RTC/SNTP 候选可信度、系统墙上时钟、时钟快照和显式同步监听器；SNTP
  客户端生命周期和单次网络样本由共享 `time_sync` 提供。
- `system_partition`：字体等逻辑数据分区的稳定标签和 subtype。

DeskMate 扩展：

- `settings_store`：兼容旧 NVS schema，并把网络字段迁移/镜像到 `system_storage`。
- `system_info`：固件、重启原因和运行资源快照。通信设备 ID 统一由共享
  `protocol_identity_get_hardware_device_id_copy()` 生成。

Sys 不创建产品 Task，不访问产品网络，不依赖 App/UI，也不保存页面或调度策略。
`system_clock` 的监听回调在接受可信时间的调用者上下文执行，不依赖默认 ESP Event Loop。
单一调用方需要周期执行时直接使用 ESP-IDF Timer，不在 Sys 中保留只改名的薄封装。
