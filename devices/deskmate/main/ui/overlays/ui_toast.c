/*
 * 文件职责：实现 Toast 占位，后续创建短时提示控件和自动隐藏动画。
 * 主要依赖：LVGL、UI 根容器。
 * 调用方：ui_main。
 */
#include "ui_toast.h"

esp_err_t ui_toast_init(void)
{
    return ESP_OK;
}

esp_err_t ui_toast_show(const char *text)
{
    (void) text;
    return ESP_OK;
}
