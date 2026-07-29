# BSP 边界

`components/bsp` 是 DeskMate 当前板型的单一装配组件，公开头文件只有 `bsp.h`。
内部源文件按 I2C、Display、Audio、Battery、Button、Environment、Power、RTC 和 Storage 拆分。

BSP 读取 Boards 参数，创建 ESP-IDF 总线和 Driver 实例，并提供同步硬件操作。它不再
使用 `bsp_core`、descriptor、全局注册表或 `bsp_board_init()`，也不决定产品初始化顺序、
采样周期、页面行为和网络策略。

业务层不直接依赖 BSP；稳定能力由 `components/device/device_*` 再封装。

`bsp_storage` 独占 SD 卡的 SPI2 总线，负责 SDSPI 卡探测与同步扇区访问；FAT 分区、格式化和
VFS 挂载由 System 通过 `device_storage` 完成。当前板型未提供 Card Detect 和 Write Protect，
不支持运行期热插拔；硬件必须使用 3.3 V 电平并为 SD SPI 信号提供必要上拉。

`bsp_power` 只检查当前板型按键电平、配置左右键 EXT1 与 ESP32 内部 Timer Light-sleep
唤醒、调用芯片睡眠入口并清理本轮临时配置。它不拥有 Timer 刷新周期、无活动窗口、
网络/UI 停机顺序、重试或失败降级。

当前 RLCD 的异步 DMA/TE 刷新仍使用 BSP 内部传输 Worker，它只处理硬件传输，不承载页面、
刷新周期或产品状态机。显示 `stop()` 只关闭新帧入口、等待传输 Worker 静止、关闭 TE 中断并
保持 LCD 输出脚；SPI、面板控制器、Worker 和缓冲区继续保留，`start()` 原地恢复。只有
`deinit()` 才释放这些资源。若后续要完全对齐 PhotoPainter 的“BSP 不创建线程”约束，应把
这段传输状态机改为由 `ui_runtime_task` 驱动的非阻塞 step API。
