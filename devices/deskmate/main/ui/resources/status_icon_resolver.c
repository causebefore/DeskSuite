/*
 * 文件职责：实现状态栏图标 ID 到静态图片资源的查找。
 * 主要依赖：ui_platform 图片目录。
 * 调用方：ui_status_bar。
 */
#include "status_icon_resolver.h"

#include "ui_platform_image.h"

const lv_image_dsc_t *status_icon_resolver_get(status_icon_id_t icon)
{
    if (icon < STATUS_ICON_WIFI_ONLINE || icon > STATUS_ICON_BATTERY_100)
    {
        return NULL;
    }
    return ui_platform_image_find(ui_platform_image_status_catalog(),
                                  (uint32_t) icon,
                                  UI_PLATFORM_IMAGE_VARIANT_DEFAULT);
}
