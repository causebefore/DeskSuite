/*
 * 文件职责：实现和风天气代码到静态图片资源的查找。
 * 主要依赖：ui_platform 图片目录。
 * 调用方：主页、天气页。
 */
#include "weather_icon_resolver.h"

#include "ui_platform_image.h"

const lv_image_dsc_t *weather_icon_resolver_get(uint16_t code, bool large)
{
    const ui_platform_image_variant_t variant =
        large ? UI_PLATFORM_IMAGE_VARIANT_LARGE : UI_PLATFORM_IMAGE_VARIANT_SMALL;
    const ui_platform_image_catalog_t *catalog = ui_platform_image_qweather_catalog();
    return ui_platform_image_find(catalog, code, variant);
}
