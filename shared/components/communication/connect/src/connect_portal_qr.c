/**
 * @file connect_portal_qr.c
 * @brief 配网二维码矩形生成实现（显示格式无关的 sink 模式）
 */

#include "connect.h"

#include "esp_check.h"
#include "esp_log.h"
#include "qrcode.h"

/** @brief 二维码四周的最小留白模块数量 */
#define CONNECT_PORTAL_QR_QUIET_MODULES 4U

/** @brief 日志标签 */
static const char *TAG = "connect_portal_qr";

/** @brief 第三方 QR 组件日志标签 */
#define CONNECT_PORTAL_QR_LIBRARY_LOG_TAG "QRCODE"

/** @brief QR 码渲染内部上下文 */
typedef struct
{
    const connect_qr_sink_t *sink;            /**< 调用方渲染接收端 */
    void                    *ctx;             /**< 调用方上下文 */
    uint16_t                 max_side_pixels; /**< 允许的最大输出边长 */
    esp_err_t                error;           /**< 回调产生的首个错误 */
} connect_portal_qr_context_t;

/**
 * @brief 根据真实模块数量计算布局并输出黑色模块矩形
 *
 * @param[in] qrcode 二维码句柄
 * @param[in,out] user_data 内部上下文
 */
static void connect_portal_qr_on_generated(esp_qrcode_handle_t qrcode, void *user_data)
{
    connect_portal_qr_context_t *context = user_data;
    if (context == NULL)
    {
        return;
    }

    const int module_count = esp_qrcode_get_size(qrcode);
    if (module_count <= 0)
    {
        context->error = ESP_ERR_INVALID_STATE;
        return;
    }

    const uint16_t total_modules = (uint16_t) module_count + CONNECT_PORTAL_QR_QUIET_MODULES * 2U;
    const uint16_t module_pixels = context->max_side_pixels / total_modules;
    if (module_pixels == 0U)
    {
        context->error = ESP_ERR_INVALID_SIZE;
        return;
    }

    const connect_qr_layout_t layout = {
        .side_pixels        = total_modules * module_pixels,
        .module_pixels      = module_pixels,
        .module_count       = (uint16_t) module_count,
        .quiet_zone_modules = CONNECT_PORTAL_QR_QUIET_MODULES,
    };
    context->error = context->sink->begin(&layout, context->ctx);
    if (context->error != ESP_OK)
    {
        return;
    }

    const uint16_t quiet_offset = CONNECT_PORTAL_QR_QUIET_MODULES * module_pixels;
    for (int module_y = 0; module_y < module_count; ++module_y)
    {
        for (int module_x = 0; module_x < module_count; ++module_x)
        {
            if (!esp_qrcode_get_module(qrcode, module_x, module_y))
            {
                continue;
            }

            const uint16_t base_x = quiet_offset + (uint16_t) module_x * module_pixels;
            const uint16_t base_y = quiet_offset + (uint16_t) module_y * module_pixels;
            context->error = context->sink->fill_dark_rect(base_x, base_y, module_pixels, module_pixels, context->ctx);
            if (context->error != ESP_OK)
            {
                return;
            }
        }
    }
}

/**
 * @brief 调用第三方 QR 编码器并禁止其输出包含凭据的原文日志
 *
 * @param[in] in_payload 待编码文本
 * @param[in,out] context QR 码渲染上下文
 * @return ESP_OK 编码和回调完成；其他值为编码器错误
 */
static esp_err_t connect_portal_qr_generate(const char *in_payload, connect_portal_qr_context_t *context)
{
    esp_log_level_set(CONNECT_PORTAL_QR_LIBRARY_LOG_TAG, ESP_LOG_NONE);
    esp_qrcode_config_t config  = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func_with_cb = connect_portal_qr_on_generated;
    config.qrcode_ecc_level     = ESP_QRCODE_ECC_MED;
    config.user_data            = context;
    return esp_qrcode_generate(&config, in_payload);
}

/**
 * @brief 将配网 QR 码同步渲染到调用方 sink
 *
 * @param[in] in_portal 配网信息，仅在调用期间借用
 * @param[in] max_side_pixels 允许输出的最大边长（像素）
 * @param[in] in_sink 渲染接收端，仅在调用期间借用
 * @param[in] ctx 用户上下文，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 配网信息未就绪；ESP_ERR_INVALID_SIZE
 *         可用边长不足；其他值为编码或 sink 错误
 */
esp_err_t connect_render_portal_qr_borrow(const connect_portal_info_t *in_portal, uint16_t max_side_pixels,
                                          const connect_qr_sink_t *in_sink, void *ctx)
{
    ESP_RETURN_ON_FALSE(in_portal != NULL && in_sink != NULL && in_sink->begin != NULL
                            && in_sink->fill_dark_rect != NULL && max_side_pixels > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "配网二维码渲染参数无效");
    ESP_RETURN_ON_FALSE(in_portal->active && in_portal->wifi_qr_payload[0] != '\0',
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "配网二维码信息尚未就绪");

    connect_portal_qr_context_t context = {
        .sink            = in_sink,
        .ctx             = ctx,
        .max_side_pixels = max_side_pixels,
        .error           = ESP_OK,
    };
    const esp_err_t err = connect_portal_qr_generate(in_portal->wifi_qr_payload, &context);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "生成配网二维码失败：%s", esp_err_to_name(err));
        return err;
    }
    if (context.error != ESP_OK)
    {
        ESP_LOGE(TAG, "渲染配网二维码失败：%s", esp_err_to_name(context.error));
        return context.error;
    }
    ESP_LOGI(TAG, "已完成配网二维码渲染");
    return ESP_OK;
}
