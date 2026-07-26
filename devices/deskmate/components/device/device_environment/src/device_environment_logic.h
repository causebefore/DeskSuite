/* 文件职责：声明温湿度整数指数移动平均纯逻辑。 */
#pragma once

#include <stdint.h>

int16_t device_environment_ema_centi(int16_t previous, int16_t sample, uint8_t alpha_percent);
