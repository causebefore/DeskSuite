# Boards 边界

`components/boards` 只描述当前板型的静态硬件事实，包括 GPIO、I2C 地址、总线频率、
分辨率和校准参数。

Boards 不初始化硬件，不创建 Driver，不保存产品采样周期、滤波策略、页面策略或设备清单。
只有 BSP 可以直接依赖 Boards；上层不得包含 `board_*.h`。

Codec 地址使用数据手册定义的真实 7-bit 值；第三方库需要的移位或编码由 BSP 适配。音频
采样率、音量和增益属于产品运行配置，不放入 Board。
