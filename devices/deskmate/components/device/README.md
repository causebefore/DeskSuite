# Device 同步能力边界

`components/device` 存放依赖 BSP、但不拥有产品调度策略的设备能力组件。

- 允许同步初始化、单次采样、内部快照和纯逻辑滤波。
- 禁止创建产品 Task、Command Queue 或调度 Timer。
- 禁止依赖 Application、Presentation、UI、Network 或 Service Core。
- 周期、重试和启停由上层 Service 或产品 Task 负责。

当前组件：

- `device_button`：封装实体按键读取、消抖和长短按识别。
- `device_battery`：封装 ADC 采样、滤波、百分比和低电状态快照。
- `device_environment`：封装温湿度采样、滤波和快照。
- `device_rtc`：封装 RTC 日历、星期、电压过低状态、告警比较配置、AF/AIE 和 INT 快速通知。
- `device_display`：封装显示尺寸、帧写入和刷新。
- `device_audio`：封装音频输入、输出、音量和 PCM。
- `device_power`：以单个同步事务封装双按键 EXT1、内部 Timer 唤醒 Light-sleep 及临时
  配置清理；Timer 间隔由上层传入，本组件不拥有刷新周期、停机顺序或产品重试。
- `device_storage`：封装外部 SD 卡的同步块设备信息、就绪检查和串行扇区读写。

上层只能包含 `device_xxx.h`。GPIO、I2C、SPI、I2S、Codec、具体芯片类型和
`BOARD_*` 宏必须停留在 BSP/Boards 以下；按键周期由 `button_service` 拥有，电池与温湿度
采样周期由 Application 的 `app_environment` 拥有。
Audio 的采样率、默认音量和增益由 `app_main` 的 Composition Root 组装配置后传入 Device。
