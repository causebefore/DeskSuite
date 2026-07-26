/*
 * 文件职责：把天气代码映射到通用静态图片资源。
 * 主要依赖：LVGL、ui_platform 图片目录。
 * 调用方：主页、天气页。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/**
 * @brief 按和风天气代码查询图标资源
 *
 * 先按 code 精确查找；找不到且 code 非 100 时回退到晴天（100）图标。
 *
 * @param code  和风天气代码
 * @param large true 取大尺寸变体，false 取小尺寸变体
 * @return const lv_image_dsc_t* 对应图片描述符；无任何匹配时返回 NULL
 */
const lv_image_dsc_t *weather_icon_resolver_get(uint16_t code, bool large);
