# Drivers 边界

Driver 只实现具体芯片协议、寄存器、CRC 和物理量换算。当前包括
`pcf85063_driver` 与 `shtc3_driver`。

Driver 接收 BSP 注入的 I2C 设备句柄，不包含 GPIO、板级地址、产品生命周期或业务策略，
也不得依赖 Boards、BSP、Device、App 或 UI。
