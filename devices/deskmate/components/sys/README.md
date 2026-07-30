# Sys 边界

`components/sys` 是离线即可成立的同步系统能力组件。

来自 PhotoPainter 的通用模块：

- `system_storage`：网络配置与时区偏移的底层持久化。
- `system_clock`：RTC/SNTP 候选可信度、系统墙上时钟、时钟快照和显式同步监听器；SNTP
  客户端生命周期和单次网络样本由共享 `time_sync` 提供。
- `system_partition`：字体等逻辑数据分区的稳定标签和 subtype。

DeskMate 扩展：

- `settings_store`：DeskMate 产品设置的规范 Store 和统一门面。调用方统一读写
  `device_settings_t`；网络字段通过 `system_storage` 的网络配置后端持久化，OTA 字段由
  DeskMate 设置命名空间持久化。它不是迁移期镜像，也不维护网络配置的第二份副本。
- `system_info`：固件、重启原因和运行资源快照。通信设备 ID 统一由共享
  `protocol_identity_get_hardware_device_id_copy()` 生成。

Sys 不创建产品 Task，不访问产品网络，不依赖 App/UI，也不保存页面或调度策略。
`system_clock` 的监听回调在接受可信时间的调用者上下文执行，不依赖默认 ESP Event Loop。
单一调用方需要周期执行时直接使用 ESP-IDF Timer，不在 Sys 中保留只改名的薄封装。
