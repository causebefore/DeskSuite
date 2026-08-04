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

`bsp_power` 只检查当前板型按键电平，按编译配置互斥选择 ESP32 内部 Timer，或 GPIO15
RTC INT 与 PCF85063 Timer 组合维护唤醒。测试模式下，一次睡眠事务会先通过 `bsp_rtc`
关闭 CIE/AIE/MI/HMI/TE/TIE 并清除 AF/TF，保持 GPIO15 内部上拉稳定 10 ms 后读取释放
基线。基线为低时不启动 Timer 或睡眠；基线为高才装载秒级 Timer 并调用芯片睡眠入口。任一
来源唤醒或入口失败后都停止 Timer 并清除 TF。若 IDF 仍拒绝睡眠，BSP 会在停止 Timer 前完成
一次电平和寄存器诊断。BSP 不拥有 Timer 刷新周期、无活动窗口、网络/UI 停机顺序、重试或失败降级。

当前 RLCD 的异步 DMA/TE 刷新仍使用 BSP 内部传输 Worker，它只处理硬件传输，不承载页面、
刷新周期或产品状态机。显示 `stop()` 关闭新帧入口、等待传输 Worker 静止并关闭 TE 中断，
随后按 ST7305 V0.2 7.11 的 `HPM → 电压重写 → 20 ms → LPM → 100 ms` 时序降低扫描功耗，
最后保持 LCD 输出脚。`start()` 先解除保持，再按
`LPM → HPM → 300 ms → 电压重写 → 20 ms` 恢复正常扫描，之后才恢复 TE 和新帧入口。
SPI、面板控制器、Worker 和缓冲区在两种模式间继续保留，只有 `deinit()` 才释放这些资源。
初始化在硬件复位与 Sleep-Out 配置后也复用同一套 HPM 稳定时序，确保 OTA 等未切断面板
电源的软件重启不会沿用旧的模拟电压状态；完成 300 ms 稳定与电压重写后才开启显示和首帧。
模式切换沿用初始化阶段已经验证过的源极电压值，不改变面板调校。若后续要完全对齐
PhotoPainter 的“BSP 不创建线程”约束，应把这段传输状态机改为由 `ui_runtime_task` 驱动的
非阻塞 step API。
