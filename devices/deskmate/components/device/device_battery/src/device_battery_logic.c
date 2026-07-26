/* 文件职责：实现电池电压换算、滤波和低电判断纯逻辑。 */
#include "device_battery_logic.h"

/**
 * @brief 将电池电压按分段线性规则换算为百分比
 */
uint8_t device_battery_percent_from_mv(uint16_t mv)
{
    if (mv <= 3300U)
    {
        return 0;
    }
    if (mv >= 4200U)
    {
        return 100;
    }
    if (mv <= 3700U)
    {
        return (uint8_t) (((uint32_t) (mv - 3300U) * 50U + 200U) / 400U);
    }
    return (uint8_t) (50U + ((uint32_t) (mv - 3700U) * 50U + 250U) / 500U);
}

/**
 * @brief 对电池电压应用整数指数移动平均
 */
uint16_t device_battery_ema_mv(uint16_t previous_mv, uint16_t sample_mv, uint8_t alpha_percent)
{
    if (previous_mv == 0U || alpha_percent >= 100U)
    {
        return sample_mv;
    }
    if (alpha_percent == 0U)
    {
        return previous_mv;
    }

    const uint32_t filtered = (uint32_t) previous_mv * (100U - alpha_percent) + (uint32_t) sample_mv * alpha_percent;
    return (uint16_t) ((filtered + 50U) / 100U);
}

/**
 * @brief 判断电池电压是否进入低电区间
 */
bool device_battery_is_low(uint16_t mv, uint16_t low_threshold_mv)
{
    return mv <= low_threshold_mv;
}
