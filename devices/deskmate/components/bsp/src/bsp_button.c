/*
 * 文件职责：封装实体按键 GPIO 初始化和原始电平读取。
 * 主要依赖：board_pins.h、ESP-IDF GPIO 驱动。
 * 调用方：device_button。
 */
#include "bsp.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG                                = "bsp_button";

static portMUX_TYPE                   s_callback_lock = portMUX_INITIALIZER_UNLOCKED;
static bsp_button_activity_callback_t s_activity_callback;
static void                          *s_activity_context;
static bool                           s_handlers_registered;
static bool                           s_ready;

/** @brief 在 GPIO ISR 上下文复制并调用长期借用的按键活动回调 */
static void IRAM_ATTR button_gpio_isr(void *arg)
{
    (void) arg;
    bsp_button_activity_callback_t callback;
    void                          *context;

    taskENTER_CRITICAL_ISR(&s_callback_lock);
    callback = s_activity_callback;
    context  = s_activity_context;
    taskEXIT_CRITICAL_ISR(&s_callback_lock);
    if (callback != NULL)
    {
        callback(context);
    }
}

/** @brief 复位左右按键 GPIO，并保留最先发生的清理错误 */
static esp_err_t reset_button_gpios(esp_err_t first_error)
{
    const esp_err_t left_error = gpio_reset_pin(BOARD_PIN_BTN_LEFT);
    if (first_error == ESP_OK && left_error != ESP_OK)
    {
        first_error = left_error;
    }
    const esp_err_t right_error = gpio_reset_pin(BOARD_PIN_BTN_RIGHT);
    if (first_error == ESP_OK && right_error != ESP_OK)
    {
        first_error = right_error;
    }
    return first_error;
}

esp_err_t bsp_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BTN_LEFT) | (1ULL << BOARD_PIN_BTN_RIGHT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&cfg);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "按键 GPIO 配置失败: %s", esp_err_to_name(error));
        return error;
    }

    error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        (void) reset_button_gpios(ESP_OK);
        ESP_LOGE(TAG, "安装共享 GPIO ISR Service 失败: %s", esp_err_to_name(error));
        return error;
    }

    error = gpio_isr_handler_add(BOARD_PIN_BTN_LEFT, button_gpio_isr, NULL);
    if (error != ESP_OK)
    {
        (void) reset_button_gpios(ESP_OK);
        ESP_LOGE(TAG, "注册左键 GPIO ISR 失败: %s", esp_err_to_name(error));
        return error;
    }

    error = gpio_isr_handler_add(BOARD_PIN_BTN_RIGHT, button_gpio_isr, NULL);
    if (error != ESP_OK)
    {
        (void) gpio_isr_handler_remove(BOARD_PIN_BTN_LEFT);
        (void) reset_button_gpios(ESP_OK);
        ESP_LOGE(TAG, "注册右键 GPIO ISR 失败: %s", esp_err_to_name(error));
        return error;
    }

    s_handlers_registered = true;
    s_ready               = true;
    ESP_LOGI(TAG, "按键初始化完成: 左键=GPIO%d, 右键=GPIO%d", BOARD_PIN_BTN_LEFT, BOARD_PIN_BTN_RIGHT);
    return ESP_OK;
}

esp_err_t bsp_button_set_activity_callback_borrow(bsp_button_activity_callback_t callback, void *context)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (callback == NULL)
    {
        esp_err_t       first_error = gpio_intr_disable(BOARD_PIN_BTN_LEFT);
        const esp_err_t right_error = gpio_intr_disable(BOARD_PIN_BTN_RIGHT);
        if (first_error == ESP_OK && right_error != ESP_OK)
        {
            first_error = right_error;
        }

        taskENTER_CRITICAL(&s_callback_lock);
        s_activity_callback = NULL;
        s_activity_context  = NULL;
        taskEXIT_CRITICAL(&s_callback_lock);
        return first_error;
    }

    taskENTER_CRITICAL(&s_callback_lock);
    s_activity_callback = callback;
    s_activity_context  = context;
    taskEXIT_CRITICAL(&s_callback_lock);

    esp_err_t error = gpio_set_intr_type(BOARD_PIN_BTN_LEFT, GPIO_INTR_ANYEDGE);
    if (error == ESP_OK)
    {
        error = gpio_set_intr_type(BOARD_PIN_BTN_RIGHT, GPIO_INTR_ANYEDGE);
    }
    if (error == ESP_OK)
    {
        error = gpio_intr_enable(BOARD_PIN_BTN_LEFT);
    }
    if (error == ESP_OK)
    {
        error = gpio_intr_enable(BOARD_PIN_BTN_RIGHT);
    }
    if (error == ESP_OK)
    {
        return ESP_OK;
    }

    (void) gpio_intr_disable(BOARD_PIN_BTN_LEFT);
    (void) gpio_intr_disable(BOARD_PIN_BTN_RIGHT);
    taskENTER_CRITICAL(&s_callback_lock);
    s_activity_callback = NULL;
    s_activity_context  = NULL;
    taskEXIT_CRITICAL(&s_callback_lock);
    return error;
}

esp_err_t bsp_button_get_level(bsp_button_id_t button, bool *out_high)
{
    if (out_high == NULL || button < BSP_BUTTON_LEFT || button >= BSP_BUTTON_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const gpio_num_t gpio = button == BSP_BUTTON_LEFT ? BOARD_PIN_BTN_LEFT : BOARD_PIN_BTN_RIGHT;
    *out_high             = gpio_get_level(gpio) != 0;
    return ESP_OK;
}

esp_err_t bsp_button_deinit(void)
{
    if (!s_ready)
    {
        return ESP_OK;
    }

    esp_err_t first_error = bsp_button_set_activity_callback_borrow(NULL, NULL);
    if (s_handlers_registered)
    {
        const esp_err_t left_error = gpio_isr_handler_remove(BOARD_PIN_BTN_LEFT);
        if (first_error == ESP_OK && left_error != ESP_OK)
        {
            first_error = left_error;
        }
        const esp_err_t right_error = gpio_isr_handler_remove(BOARD_PIN_BTN_RIGHT);
        if (first_error == ESP_OK && right_error != ESP_OK)
        {
            first_error = right_error;
        }
        s_handlers_registered = false;
    }
    first_error = reset_button_gpios(first_error);
    s_ready     = false;
    ESP_LOGI(TAG, "按键反初始化完成: 左键=GPIO%d, 右键=GPIO%d", BOARD_PIN_BTN_LEFT, BOARD_PIN_BTN_RIGHT);
    return first_error;
}
