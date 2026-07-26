/* 文件职责：实现温湿度整数指数移动平均纯逻辑。 */
#include "device_environment_logic.h"

/**
 * @brief 对百分之一单位的传感器值应用整数指数移动平均
 */
int16_t device_environment_ema_centi(int16_t previous, int16_t sample, uint8_t alpha_percent)
{
    if (alpha_percent >= 100U)
    {
        return sample;
    }
    if (alpha_percent == 0U)
    {
        return previous;
    }

    const int32_t filtered =
        (int32_t) previous * (100 - (int32_t) alpha_percent) + (int32_t) sample * (int32_t) alpha_percent;
    return (int16_t) ((filtered >= 0 ? (filtered + 50) : (filtered - 50)) / 100);
}
