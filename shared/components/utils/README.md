# Utils

> DeskSuite 各设备固件与共享组件通用的字符串、字节序与校验工具。

## 1. 能力

- 有界字符串：`utils_copy_string()`、`utils_append_string()`。
- 十六进制：`utils_hex_digit_value()`、`utils_is_hex_string()`、`utils_bytes_to_hex()`、
  `utils_hex_to_bytes()`。
- 字节序：16/32/64 位大端与小端读写。
- 时间边界：`utils_duration_reached_us()`（纯函数，不读时钟）。
- CRC：CRC-8（MSB-first）、CRC-16/CCITT-FALSE、CRC-16/MODBUS、IEEE CRC-32，
  均提供流式 `_update()` 形式。
- SHA-256：基于 mbedTLS 的流式与一次性封装。

## 2. 边界

- 本组件不包含产品领域函数（例如显示像素打包归各显示驱动所有）。
- 所有函数不依赖 FreeRTOS，可在任意任务上下文调用。
