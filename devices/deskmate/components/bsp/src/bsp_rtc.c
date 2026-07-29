/*
 * 文件职责：创建板级 RTC I2C 设备并组合 PCF85063 Driver。
 * 主要依赖：board_rtc.h、bsp_i2c、pcf85063_driver。
 * 调用方：device_rtc 与 BSP 内部轻睡眠事务。
 */
#include "bsp.h"

#include <string.h>

#include "board.h"
#include "bsp_i2c_internal.h"
#include "bsp_rtc_internal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pcf85063_driver.h"

static const char *TAG = "bsp_rtc";

#define BSP_RTC_I2C_TIMEOUT_MS              100
#define BSP_RTC_MUTEX_TIMEOUT_MS            100
#define BSP_RTC_INT_PULLUP_SETTLE_US        500U
#define BSP_RTC_INT_LOW_CONFIRM_COUNT       3U
#define BSP_RTC_INT_LOW_CONFIRM_INTERVAL_US 100U

static bool                         s_ready;
static i2c_master_dev_handle_t      s_i2c_device;
static pcf85063_driver_t            s_driver;
static SemaphoreHandle_t            s_mutex;
static StaticSemaphore_t            s_mutex_buffer;
static bsp_rtc_interrupt_callback_t s_interrupt_callback;
static void                        *s_interrupt_context;
static portMUX_TYPE                 s_interrupt_lock = portMUX_INITIALIZER_UNLOCKED;

_Static_assert(BOARD_RTC_PIN_INT >= 0 && BOARD_RTC_PIN_INT < GPIO_NUM_MAX, "RTC INT GPIO 必须是有效数字 IO");
_Static_assert(BOARD_RTC_PIN_INT != BOARD_I2C_PIN_SDA && BOARD_RTC_PIN_INT != BOARD_I2C_PIN_SCL,
               "RTC INT GPIO 不能与 RTC I2C 引脚复用");

/** @brief 串行化同一 RTC Device 上的多寄存器 I2C 事务 */
static esp_err_t lock_driver(void)
{
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(BSP_RTC_MUTEX_TIMEOUT_MS)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief 恢复 RTC INT 输入上拉并确认低电平是否持续存在
 *
 * 当前板型的 PCF85063 INT 为开漏输出且没有外部上拉。每次休眠检查前重新声明 GPIO15
 * 为上拉输入；首个样本为低时等待弱上拉完成充电并连续复采，避免把配置切换后的慢上升沿
 * 或短毛刺误判成仍在断言的中断。
 *
 * @param[out] out_asserted 是否确认 RTC INT 持续为低
 * @return ESP_OK 采样完成；其他值表示 GPIO 配置失败
 */
static esp_err_t sample_interrupt_asserted(bool *out_asserted)
{
    ESP_RETURN_ON_ERROR(gpio_set_direction((gpio_num_t) BOARD_RTC_PIN_INT, GPIO_MODE_INPUT),
                        TAG,
                        "恢复 RTC INT 输入方向失败");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode((gpio_num_t) BOARD_RTC_PIN_INT, GPIO_PULLUP_ONLY),
                        TAG,
                        "恢复 RTC INT 上拉失败");

    if (gpio_get_level((gpio_num_t) BOARD_RTC_PIN_INT) != 0)
    {
        *out_asserted = false;
        return ESP_OK;
    }

    esp_rom_delay_us(BSP_RTC_INT_PULLUP_SETTLE_US);
    for (uint32_t sample = 0; sample < BSP_RTC_INT_LOW_CONFIRM_COUNT; ++sample)
    {
        if (gpio_get_level((gpio_num_t) BOARD_RTC_PIN_INT) != 0)
        {
            *out_asserted = false;
            return ESP_OK;
        }
        if (sample + 1U < BSP_RTC_INT_LOW_CONFIRM_COUNT)
        {
            esp_rom_delay_us(BSP_RTC_INT_LOW_CONFIRM_INTERVAL_US);
        }
    }

    *out_asserted = true;
    return ESP_OK;
}

/**
 * @brief 判断当前状态是否适合执行无损 AIE 门控恢复
 *
 * 仅当 AIE 已启用且芯片没有任何已知告警、分钟或计时器中断源时允许恢复，
 * 避免把真实中断误当成输出异常。
 *
 * @param[in] snapshot RTC 中断控制与标志快照
 * @return true 可以执行 AIE 门控恢复；false 应保留当前输出状态
 */
static bool stale_alarm_output_recovery_is_applicable(const pcf85063_interrupt_snapshot_t *snapshot)
{
    return snapshot->alarm_interrupt_enabled && !snapshot->alarm_flag && !snapshot->minute_interrupt_enabled
           && !snapshot->half_minute_interrupt_enabled && !snapshot->timer_flag && !snapshot->timer_enabled
           && !snapshot->timer_interrupt_enabled;
}

/**
 * @brief 短暂关闭并恢复 AIE，尝试释放没有寄存器来源的持续低电平
 *
 * 整个寄存器序列持有 RTC 事务锁。Driver 对 AF/TF 写 1 以保留恢复窗口内新产生的
 * 标志；恢复 AIE 后再次读取状态，因此不会吞掉真正到期的告警。
 *
 * @param[out] out_recovered true 表示 GPIO15 已在 AIE 恢复后释放
 * @return ESP_OK 无需恢复、恢复成功或已完成故障分类；其他值表示 I2C/GPIO 操作失败
 */
static esp_err_t try_recover_stale_alarm_output(bool *out_recovered)
{
    *out_recovered = false;
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 输出恢复事务锁超时");

    pcf85063_interrupt_snapshot_t initial_snapshot;
    esp_err_t                     error = pcf85063_driver_read_interrupt_snapshot(&s_driver, &initial_snapshot);
    if (error != ESP_OK || !stale_alarm_output_recovery_is_applicable(&initial_snapshot))
    {
        xSemaphoreGive(s_mutex);
        return error;
    }

    const esp_err_t disable_error           = pcf85063_driver_enable_alarm_interrupt(&s_driver, false);
    bool            released_while_disabled = false;
    if (disable_error == ESP_OK)
    {
        esp_rom_delay_us(BSP_RTC_INT_PULLUP_SETTLE_US);
        released_while_disabled = gpio_get_level((gpio_num_t) BOARD_RTC_PIN_INT) != 0;
    }
    /*
     * 即使关闭事务报错也尝试恢复 AIE：I2C 错误可能发生在设备已经接收字节之后，
     * 不能把“返回失败”等同于寄存器一定未改变。
     */
    const esp_err_t restore_error                = pcf85063_driver_enable_alarm_interrupt(&s_driver, true);
    error                                        = disable_error != ESP_OK ? disable_error : restore_error;

    bool                          still_asserted = true;
    pcf85063_interrupt_snapshot_t final_snapshot = { 0 };
    if (error == ESP_OK)
    {
        error = sample_interrupt_asserted(&still_asserted);
    }
    if (error == ESP_OK)
    {
        error = pcf85063_driver_read_interrupt_snapshot(&s_driver, &final_snapshot);
    }
    xSemaphoreGive(s_mutex);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "重置 RTC AIE 门控并恢复原配置失败: %s", esp_err_to_name(error));
        return error;
    }

    if (!still_asserted)
    {
        *out_recovered = true;
        ESP_LOGW(TAG, "RTC INT 无标志低电平已通过重置 AIE 门控恢复");
    }
    else if (!final_snapshot.alarm_flag)
    {
        if (released_while_disabled)
        {
            ESP_LOGW(TAG, "RTC INT 在关闭 AIE 后释放，但恢复 AIE 后无 AF 再次拉低，RTC 输出门控异常");
        }
        else
        {
            ESP_LOGW(TAG, "RTC INT 在关闭 AIE 后仍持续为低，判定为板级拉低或 RTC 输出级异常");
        }
    }
    return ESP_OK;
}

/** @brief 在 GPIO ISR 中复制并调用已注册的快速通知回调 */
static void rtc_gpio_isr(void *arg)
{
    (void) arg;
    portENTER_CRITICAL_ISR(&s_interrupt_lock);
    bsp_rtc_interrupt_callback_t callback = s_interrupt_callback;
    void                        *context  = s_interrupt_context;
    portEXIT_CRITICAL_ISR(&s_interrupt_lock);
    if (callback != NULL)
    {
        callback(context);
    }
}

static pcf85063_datetime_t to_driver_datetime(const bsp_rtc_datetime_t *value)
{
    return (pcf85063_datetime_t) {
        .year   = value->year,
        .month  = value->month,
        .day    = value->day,
        .hour   = value->hour,
        .minute = value->minute,
        .second = value->second,
    };
}

/** @brief 在普通 Task 上下文记录持续低电平对应的 RTC 中断来源分类 */
static void log_asserted_interrupt_diagnosis(void)
{
    const esp_err_t lock_error = lock_driver();
    if (lock_error != ESP_OK)
    {
        ESP_LOGW(TAG, "RTC INT 已拉低，但等待寄存器诊断锁失败: %s", esp_err_to_name(lock_error));
        return;
    }

    pcf85063_interrupt_snapshot_t snapshot;
    const esp_err_t               error = pcf85063_driver_read_interrupt_snapshot(&s_driver, &snapshot);
    xSemaphoreGive(s_mutex);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "RTC INT 已拉低，但读取中断状态失败: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGW(TAG,
             "RTC INT 诊断快照: GPIO%d=低, Control_2=0x%02x, Timer_mode=0x%02x, "
             "AIE=%d, AF=%d, MI=%d, HMI=%d, TF=%d, TE=%d, TIE=%d",
             BOARD_RTC_PIN_INT,
             snapshot.control2_raw,
             snapshot.timer_mode_raw,
             (int) snapshot.alarm_interrupt_enabled,
             (int) snapshot.alarm_flag,
             (int) snapshot.minute_interrupt_enabled,
             (int) snapshot.half_minute_interrupt_enabled,
             (int) snapshot.timer_flag,
             (int) snapshot.timer_enabled,
             (int) snapshot.timer_interrupt_enabled);

    if (snapshot.alarm_interrupt_enabled && snapshot.alarm_flag)
    {
        ESP_LOGW(TAG, "RTC INT 诊断结论: AIE=1 且 AF=1，PCF85063 闹钟正在主动拉低 GPIO%d", BOARD_RTC_PIN_INT);
        return;
    }

    if (snapshot.timer_interrupt_enabled && snapshot.timer_flag)
    {
        ESP_LOGW(TAG, "RTC INT 诊断结论: TIE=1 且 TF=1，PCF85063 计时器正在主动拉低 GPIO%d", BOARD_RTC_PIN_INT);
        return;
    }
    if ((snapshot.minute_interrupt_enabled || snapshot.half_minute_interrupt_enabled) && snapshot.timer_flag)
    {
        ESP_LOGW(TAG,
                 "RTC INT 诊断结论: 分钟中断已启用且 TF=1，PCF85063 周期源可能正在主动拉低 GPIO%d",
                 BOARD_RTC_PIN_INT);
        return;
    }

    ESP_LOGW(TAG,
             "RTC INT 诊断结论: RTC 寄存器没有有效中断来源，GPIO%d 持续为低更倾向板级拉低或 RTC 输出级异常",
             BOARD_RTC_PIN_INT);
}

esp_err_t bsp_rtc_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "初始化 I2C 失败");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_RTC_PCF85063_ADDR,
        .scl_speed_hz    = BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_bus_handle(), &config, &s_i2c_device),
                        TAG,
                        "创建 RTC I2C 设备失败");

    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    if (s_mutex == NULL)
    {
        (void) i2c_master_bus_rm_device(s_i2c_device);
        s_i2c_device = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = pcf85063_driver_init(&s_driver, s_i2c_device, BSP_RTC_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        (void) i2c_master_bus_rm_device(s_i2c_device);
        s_i2c_device = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    const gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << BOARD_RTC_PIN_INT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&io_config);
    if (err == ESP_OK)
    {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE)
        {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK)
    {
        err = gpio_isr_handler_add((gpio_num_t) BOARD_RTC_PIN_INT, rtc_gpio_isr, NULL);
    }
    if (err != ESP_OK)
    {
        (void) gpio_reset_pin((gpio_num_t) BOARD_RTC_PIN_INT);
        (void) i2c_master_bus_rm_device(s_i2c_device);
        s_i2c_device = NULL;
        memset(&s_driver, 0, sizeof(s_driver));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        ESP_LOGE(TAG, "配置 RTC INT GPIO%d 失败: %s", BOARD_RTC_PIN_INT, esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "RTC 初始化完成: addr=0x%02x, INT=GPIO%d（低电平有效）", BOARD_RTC_PCF85063_ADDR, BOARD_RTC_PIN_INT);
    return ESP_OK;
}

esp_err_t bsp_rtc_read_datetime(bsp_rtc_datetime_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    pcf85063_datetime_t value;
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_read_datetime(&s_driver, &value);
    xSemaphoreGive(s_mutex);
    ESP_RETURN_ON_ERROR(error, TAG, "读取 RTC 时间失败");
    *out = (bsp_rtc_datetime_t) {
        .year   = value.year,
        .month  = value.month,
        .day    = value.day,
        .hour   = value.hour,
        .minute = value.minute,
        .second = value.second,
    };
    return ESP_OK;
}

esp_err_t bsp_rtc_read_voltage_low(bool *out_voltage_low)
{
    if (out_voltage_low == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_read_voltage_low(&s_driver, out_voltage_low);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_set_datetime(const bsp_rtc_datetime_t *value)
{
    if (value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const pcf85063_datetime_t driver_value = to_driver_datetime(value);
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_set_datetime(&s_driver, &driver_value);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_start_wakeup_timer(uint32_t interval_ms)
{
    if (interval_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t interval_s = ((uint64_t) interval_ms + 999ULL) / 1000ULL;
    if (interval_s == 0U || interval_s > UINT8_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    esp_err_t error = pcf85063_driver_enable_alarm_interrupt(&s_driver, false);
    if (error == ESP_OK)
    {
        error = pcf85063_driver_clear_alarm_flag(&s_driver);
    }
    if (error == ESP_OK)
    {
        error = pcf85063_driver_start_timer(&s_driver, (uint8_t) interval_s);
    }
    if (error != ESP_OK)
    {
        const esp_err_t cleanup_error = pcf85063_driver_stop_timer(&s_driver);
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "启动 RTC 唤醒计时器失败且回滚停止失败: start=%s stop=%s",
                     esp_err_to_name(error),
                     esp_err_to_name(cleanup_error));
        }
    }
    xSemaphoreGive(s_mutex);

    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "RTC 唤醒计时器已启动: %llu 秒，AIE/AF/TF 已清理", (unsigned long long) interval_s);
    }
    return error;
}

esp_err_t bsp_rtc_stop_wakeup_timer(void)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_stop_timer(&s_driver);
    xSemaphoreGive(s_mutex);
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "RTC 唤醒计时器已停止，TF 已清除");
    }
    return error;
}

esp_err_t bsp_rtc_set_alarm(const bsp_rtc_alarm_t *alarm)
{
    if (alarm == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const pcf85063_alarm_t driver_alarm = {
        .second       = alarm->second,
        .minute       = alarm->minute,
        .hour         = alarm->hour,
        .day          = alarm->day,
        .weekday      = alarm->weekday,
        .match_fields = alarm->match_fields,
    };
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_set_alarm(&s_driver, &driver_alarm);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_read_alarm(bsp_rtc_alarm_t *out_alarm)
{
    if (out_alarm == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    pcf85063_alarm_t driver_alarm;
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_read_alarm(&s_driver, &driver_alarm);
    xSemaphoreGive(s_mutex);
    if (error == ESP_OK)
    {
        *out_alarm = (bsp_rtc_alarm_t) {
            .second       = driver_alarm.second,
            .minute       = driver_alarm.minute,
            .hour         = driver_alarm.hour,
            .day          = driver_alarm.day,
            .weekday      = driver_alarm.weekday,
            .match_fields = driver_alarm.match_fields,
        };
    }
    return error;
}

esp_err_t bsp_rtc_enable_alarm_interrupt(bool enabled)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_enable_alarm_interrupt(&s_driver, enabled);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_read_alarm_interrupt_enabled(bool *out_enabled)
{
    if (out_enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_read_alarm_interrupt_enabled(&s_driver, out_enabled);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_clear_alarm_flag(void)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_clear_alarm_flag(&s_driver);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_read_alarm_flag(bool *out_pending)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(lock_driver(), TAG, "等待 RTC 事务锁超时");
    const esp_err_t error = pcf85063_driver_read_alarm_flag(&s_driver, out_pending);
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t bsp_rtc_read_interrupt_asserted(bool *out_asserted)
{
    if (out_asserted == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(sample_interrupt_asserted(out_asserted), TAG, "确认 RTC INT 电平失败");
    if (*out_asserted)
    {
        bool recovered = false;
        ESP_RETURN_ON_ERROR(try_recover_stale_alarm_output(&recovered), TAG, "恢复 RTC INT 无标志低电平失败");
        if (recovered)
        {
            *out_asserted = false;
            return ESP_OK;
        }
        log_asserted_interrupt_diagnosis();
    }
    return ESP_OK;
}

esp_err_t bsp_rtc_set_interrupt_callback_borrow(bsp_rtc_interrupt_callback_t callback, void *context)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_interrupt_lock);
    s_interrupt_callback = callback;
    s_interrupt_context  = callback != NULL ? context : NULL;
    portEXIT_CRITICAL(&s_interrupt_lock);
    return ESP_OK;
}

esp_err_t bsp_rtc_deinit(void)
{
    if (!s_ready)
    {
        return ESP_OK;
    }

    esp_err_t result = gpio_isr_handler_remove((gpio_num_t) BOARD_RTC_PIN_INT);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "移除 RTC INT GPIO ISR 失败: %s", esp_err_to_name(result));
    }
    portENTER_CRITICAL(&s_interrupt_lock);
    s_interrupt_callback = NULL;
    s_interrupt_context  = NULL;
    portEXIT_CRITICAL(&s_interrupt_lock);

    const esp_err_t timer_disable_err = bsp_rtc_stop_wakeup_timer();
    if (result == ESP_OK)
    {
        result = timer_disable_err;
    }
    if (timer_disable_err != ESP_OK)
    {
        ESP_LOGW(TAG, "关闭 RTC 计时器失败: %s", esp_err_to_name(timer_disable_err));
    }

    const esp_err_t disable_err = bsp_rtc_enable_alarm_interrupt(false);
    if (result == ESP_OK)
    {
        result = disable_err;
    }
    if (disable_err != ESP_OK)
    {
        ESP_LOGW(TAG, "关闭 RTC alarm 中断失败: %s", esp_err_to_name(disable_err));
    }

    const esp_err_t remove_err = i2c_master_bus_rm_device(s_i2c_device);
    if (result == ESP_OK)
    {
        result = remove_err;
    }
    if (remove_err != ESP_OK)
    {
        ESP_LOGW(TAG, "移除 RTC I2C 设备失败: %s", esp_err_to_name(remove_err));
    }

    s_i2c_device = NULL;
    memset(&s_driver, 0, sizeof(s_driver));
    (void) gpio_reset_pin((gpio_num_t) BOARD_RTC_PIN_INT);
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    s_ready = false;
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "RTC 反初始化完成（硬件继续走时）");
    }
    return result;
}
