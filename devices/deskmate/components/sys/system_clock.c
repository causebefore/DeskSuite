/**
 * @file system_clock.c
 * @brief 以板级 RTC 或 SNTP 可信时间维护系统时钟
 */

#include "system_clock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "device_rtc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "system_storage.h"

/** @brief 日志标签 */
static const char *TAG = "system_clock";

/** @brief 可接受的最早 UTC 时间：2024-01-01 00:00:00 */
#define SYSTEM_CLOCK_MIN_TIMESTAMP                  ((time_t) 1704067200LL)

/** @brief 可接受的最晚 UTC 时间：2099-12-31 23:59:59 */
#define SYSTEM_CLOCK_MAX_TIMESTAMP                  ((time_t) 4102444799LL)

/** @brief 无需二次确认的最大 SNTP 跳变秒数：6 小时 */
#define SYSTEM_CLOCK_MAX_UNCONFIRMED_JUMP_SECONDS   INT64_C(21600)

/** @brief 两个大跳变 SNTP 候选允许的最大差异秒数：5 分钟 */
#define SYSTEM_CLOCK_CONFIRMATION_TOLERANCE_SECONDS INT64_C(300)

/** @brief 一秒包含的微秒数 */
#define SYSTEM_CLOCK_US_PER_SECOND                  INT64_C(1000000)

/** @brief POSIX 时区字符串：固定 UTC+8，不启用夏令时 */
#define SYSTEM_CLOCK_TIMEZONE                       "CST-8"

/** @brief 系统时钟初始化状态 */
static bool s_initialized                  = false;

/** @brief 系统时钟初始化事务是否正在由唯一调用者执行 */
static bool s_initializing                 = false;

/** @brief 系统时间最近一次成功校准所使用的可信来源 */
static system_clock_source_t s_time_source = SYSTEM_CLOCK_SOURCE_NONE;

/** @brief 当前本地时区相对 UTC 的偏移分钟数 */
static int16_t s_utc_offset_minutes        = SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES;

/** @brief 最近一次接受可信时间时的 UTC 秒数 */
static time_t s_trusted_anchor_timestamp;

/** @brief 最近一次接受可信时间时的单调时钟微秒数 */
static int64_t s_trusted_anchor_monotonic_us;

/** @brief 是否已经暂存一个等待二次确认的 SNTP 候选 */
static bool s_confirmation_pending;

/** @brief 等待二次确认的第一个 SNTP 候选 UTC 秒数 */
static time_t s_pending_confirmation_timestamp;

/** @brief 串行化 RTC/SNTP 可信时间写事务 */
static SemaphoreHandle_t s_writer_mutex;

/** @brief 保护可信锚点、来源、偏移和初始化标记的短临界区 */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

/** @brief 显式监听器上限；当前由首页和状态栏使用 */
#define SYSTEM_CLOCK_MAX_LISTENERS 4U

typedef struct
{
    system_clock_listener_t listener;
    void                   *ctx;
} system_clock_listener_entry_t;

static system_clock_listener_entry_t s_listeners[SYSTEM_CLOCK_MAX_LISTENERS];
static portMUX_TYPE                  s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t system_clock_register_listener_borrow(system_clock_listener_t listener, void *ctx)
{
    ESP_RETURN_ON_FALSE(listener != NULL, ESP_ERR_INVALID_ARG, TAG, "系统时钟监听器为空");
    taskENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "系统时钟尚未初始化");

    esp_err_t result = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&s_listener_lock);
    for (size_t index = 0; index < SYSTEM_CLOCK_MAX_LISTENERS; ++index)
    {
        if (s_listeners[index].listener == listener && s_listeners[index].ctx == ctx)
        {
            result = ESP_OK;
            break;
        }
        if (s_listeners[index].listener == NULL)
        {
            s_listeners[index] = (system_clock_listener_entry_t) {
                .listener = listener,
                .ctx      = ctx,
            };
            result = ESP_OK;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_listener_lock);
    return result;
}

esp_err_t system_clock_unregister_listener(system_clock_listener_t listener, void *ctx)
{
    ESP_RETURN_ON_FALSE(listener != NULL, ESP_ERR_INVALID_ARG, TAG, "系统时钟监听器为空");
    esp_err_t result = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_listener_lock);
    for (size_t index = 0; index < SYSTEM_CLOCK_MAX_LISTENERS; ++index)
    {
        if (s_listeners[index].listener == listener && s_listeners[index].ctx == ctx)
        {
            s_listeners[index] = (system_clock_listener_entry_t) { 0 };
            result             = ESP_OK;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_listener_lock);
    return result;
}

/** @brief 在可信时间写入者上下文中通知显式监听器 */
static void system_clock_notify(system_clock_event_t event, const system_clock_snapshot_t *snapshot)
{
    system_clock_listener_entry_t listeners[SYSTEM_CLOCK_MAX_LISTENERS];
    taskENTER_CRITICAL(&s_listener_lock);
    for (size_t index = 0; index < SYSTEM_CLOCK_MAX_LISTENERS; ++index)
    {
        listeners[index] = s_listeners[index];
    }
    taskEXIT_CRITICAL(&s_listener_lock);

    for (size_t index = 0; index < SYSTEM_CLOCK_MAX_LISTENERS; ++index)
    {
        if (listeners[index].listener != NULL)
        {
            listeners[index].listener(event, snapshot, listeners[index].ctx);
        }
    }
}

/**
 * @brief 判断 UTC 时间戳是否位于产品支持范围
 *
 * @param[in] timestamp 待检查的 UTC 时间戳
 * @return true 时间位于 2024 至 2099 年范围内，false 超出范围
 */
static bool system_clock_timestamp_is_valid(time_t timestamp)
{
    return timestamp >= SYSTEM_CLOCK_MIN_TIMESTAMP && timestamp <= SYSTEM_CLOCK_MAX_TIMESTAMP;
}

/** @brief 返回指定年份和月份的日数，参数无效时返回 0 */
static uint8_t system_clock_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = { 31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U };
    if (month < 1U || month > 12U)
    {
        return 0U;
    }
    uint8_t    result = days[month - 1U];
    const bool leap   = year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
    if (month == 2U && leap)
    {
        ++result;
    }
    return result;
}

/**
 * @brief 将 RTC 的 UTC+8 日历时间转换为 UTC 时间戳
 *
 * 使用公历日数算法直接换算，不依赖进程全局 `TZ` 或 `mktime()` 的本地时区状态。
 *
 * @param[in] datetime RTC 本地日历时间
 * @param[in] utc_offset_minutes 本地时间相对 UTC 的分钟偏移
 * @param[out] out_timestamp 转换后的 UTC 时间戳
 * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE RTC 日历字段或结果超出支持范围
 */
static esp_err_t system_clock_datetime_to_utc(const device_rtc_datetime_t *datetime, int16_t utc_offset_minutes,
                                              time_t *out_timestamp)
{
    ESP_RETURN_ON_FALSE(datetime != NULL && out_timestamp != NULL, ESP_ERR_INVALID_ARG, TAG, "RTC 时间转换参数无效");
    const uint8_t month_days = system_clock_days_in_month(datetime->year, datetime->month);
    ESP_RETURN_ON_FALSE(datetime->year >= 2000U && datetime->year <= 2099U && month_days > 0U && datetime->day >= 1U
                            && datetime->day <= month_days && datetime->hour <= 23U && datetime->minute <= 59U
                            && datetime->second <= 59U,
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "RTC 日历时间字段无效");

    int32_t       year  = (int32_t) datetime->year;
    const int32_t month = (int32_t) datetime->month;
    year -= month <= 2;
    const int32_t  era              = (year >= 0 ? year : year - 399) / 400;
    const uint32_t year_of_era      = (uint32_t) (year - era * 400);
    const uint32_t shifted_month    = (uint32_t) (month + (month > 2 ? -3 : 9));
    const uint32_t day_of_year      = (153U * shifted_month + 2U) / 5U + (uint32_t) datetime->day - 1U;
    const uint32_t day_of_era       = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    const int64_t  days_since_epoch = (int64_t) era * 146097LL + (int64_t) day_of_era - 719468LL;
    const int64_t  local_seconds    = days_since_epoch * 86400LL + (int64_t) datetime->hour * 3600LL
                                      + (int64_t) datetime->minute * 60LL + (int64_t) datetime->second;
    const time_t   utc_timestamp    = (time_t) (local_seconds - (int64_t) utc_offset_minutes * 60LL);
    ESP_RETURN_ON_FALSE(system_clock_timestamp_is_valid(utc_timestamp),
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "RTC 时间超出系统时钟支持范围");
    *out_timestamp = utc_timestamp;
    return ESP_OK;
}

/**
 * @brief 将 UTC 时间戳转换为设备 RTC 使用的固定偏移日历时间
 *
 * @param[in] timestamp UTC 时间戳
 * @param[in] utc_offset_minutes 本地时间相对 UTC 的分钟偏移
 * @param[out] out_datetime 转换后的 RTC 日历时间
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_RESPONSE 转换结果无效
 */
static esp_err_t system_clock_utc_to_datetime(time_t timestamp, int16_t utc_offset_minutes,
                                              device_rtc_datetime_t *out_datetime)
{
    ESP_RETURN_ON_FALSE(out_datetime != NULL, ESP_ERR_INVALID_ARG, TAG, "RTC 回写时间转换参数无效");
    const time_t local_timestamp = timestamp + (time_t) ((int64_t) utc_offset_minutes * 60LL);
    struct tm    calendar_time;
    ESP_RETURN_ON_FALSE(gmtime_r(&local_timestamp, &calendar_time) != NULL,
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "UTC 时间转换为 RTC 日历失败");

    const int year = calendar_time.tm_year + 1900;
    ESP_RETURN_ON_FALSE(year >= 2000 && year <= 2099, ESP_ERR_INVALID_RESPONSE, TAG, "RTC 回写年份超出支持范围");
    out_datetime->year   = (uint16_t) year;
    out_datetime->month  = (uint8_t) (calendar_time.tm_mon + 1);
    out_datetime->day    = (uint8_t) calendar_time.tm_mday;
    out_datetime->hour   = (uint8_t) calendar_time.tm_hour;
    out_datetime->minute = (uint8_t) calendar_time.tm_min;
    out_datetime->second = (uint8_t) calendar_time.tm_sec;
    return ESP_OK;
}

/**
 * @brief 计算两个受支持 UTC 时间戳之间的绝对差值
 *
 * @param[in] left 第一个 UTC 时间戳
 * @param[in] right 第二个 UTC 时间戳
 * @return 两个时间戳相差的秒数
 */
static int64_t system_clock_timestamp_difference(time_t left, time_t right)
{
    const int64_t difference = (int64_t) left - (int64_t) right;
    return difference < 0 ? -difference : difference;
}

/**
 * @brief 将 UTC 时间戳写入系统时钟
 *
 * @param[in] timestamp 要设置的 UTC 时间戳
 *
 * @return ESP_OK 成功，或其他错误码
 */
static esp_err_t system_clock_set_system_time(time_t timestamp)
{
    const struct timeval time_value = {
        .tv_sec  = timestamp,
        .tv_usec = 0,
    };
    ESP_RETURN_ON_FALSE(settimeofday(&time_value, NULL) == 0, ESP_FAIL, TAG, "设置系统时间失败");
    return ESP_OK;
}

/**
 * @brief 使用单调时钟推算当前可信 UTC 时间
 *
 * 本函数不读取可能已被 SNTP 候选提前改写的 POSIX 墙上时钟。
 *
 * @param[out] timestamp 推算得到的当前可信 UTC 时间戳
 * @param[out] source 与锚点同一事务复制的可信来源，可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚无可信锚点或单调时钟异常
 */
static esp_err_t system_clock_get_trusted_time(time_t *timestamp, system_clock_source_t *source)
{
    ESP_RETURN_ON_FALSE(timestamp != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    taskENTER_CRITICAL(&s_state_lock);
    const system_clock_source_t copied_source       = s_time_source;
    const time_t                anchor_timestamp    = s_trusted_anchor_timestamp;
    const int64_t               anchor_monotonic_us = s_trusted_anchor_monotonic_us;
    taskEXIT_CRITICAL(&s_state_lock);

    if (copied_source == SYSTEM_CLOCK_SOURCE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now_monotonic_us = esp_timer_get_time();
    ESP_RETURN_ON_FALSE(now_monotonic_us >= anchor_monotonic_us,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "单调时钟早于可信时间锚点");
    *timestamp = anchor_timestamp + (time_t) ((now_monotonic_us - anchor_monotonic_us) / SYSTEM_CLOCK_US_PER_SECOND);
    if (source != NULL)
    {
        *source = copied_source;
    }
    return ESP_OK;
}

/**
 * @brief 接受一个可信 UTC 时间并同时更新墙上时钟与单调锚点
 *
 * @param[in] timestamp 要接受的 UTC 时间戳
 * @param[in] source 可信时间来源
 * @return ESP_OK 成功，或设置系统墙上时钟的错误码
 */
static esp_err_t system_clock_accept_trusted_time(time_t timestamp, system_clock_source_t source)
{
    ESP_RETURN_ON_ERROR(system_clock_set_system_time(timestamp), TAG, "设置系统时间失败");
    const int64_t anchor_monotonic_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_state_lock);
    s_trusted_anchor_timestamp       = timestamp;
    s_trusted_anchor_monotonic_us    = anchor_monotonic_us;
    s_time_source                    = source;
    const int16_t utc_offset_minutes = s_utc_offset_minutes;
    taskEXIT_CRITICAL(&s_state_lock);

    const system_clock_snapshot_t snapshot = {
        .utc_timestamp      = timestamp,
        .utc_offset_minutes = utc_offset_minutes,
        .source             = source,
        .valid              = true,
    };
    system_clock_notify(SYSTEM_CLOCK_EVENT_UPDATED, &snapshot);
    return ESP_OK;
}

/**
 * @brief 尽力把已接受的 SNTP 时间按固定本地偏移回写设备 RTC
 *
 * 调用方必须已经持有 s_writer_mutex，使系统可信锚点与 RTC 写入保持同一校时事务顺序。
 *
 * @param[in] timestamp 已接受的 UTC 时间戳
 */
static void system_clock_write_sntp_time_to_rtc(time_t timestamp)
{
    taskENTER_CRITICAL(&s_state_lock);
    const int16_t utc_offset_minutes = s_utc_offset_minutes;
    taskEXIT_CRITICAL(&s_state_lock);

    device_rtc_datetime_t datetime;
    esp_err_t             error = system_clock_utc_to_datetime(timestamp, utc_offset_minutes, &datetime);
    if (error == ESP_OK)
    {
        error = device_rtc_set_datetime(&datetime);
    }
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "SNTP 时间已接受，但回写设备 RTC 失败，系统时间继续有效：%s", esp_err_to_name(error));
        return;
    }

    ESP_LOGI(TAG,
             "SNTP 时间已回写设备 RTC：%04u-%02u-%02u %02u:%02u:%02u (UTC+08:00)",
             (unsigned int) datetime.year,
             (unsigned int) datetime.month,
             (unsigned int) datetime.day,
             (unsigned int) datetime.hour,
             (unsigned int) datetime.minute,
             (unsigned int) datetime.second);
}

/**
 * @brief 从系统存储恢复 UTC 偏移，缺失时写入默认值
 *
 * 存储读取或默认值写入失败时仍采用固定 +480 分钟，并记录降级日志。
 */
static void system_clock_load_utc_offset(void)
{
    int16_t         stored_offset = SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES;
    const esp_err_t err           = system_storage_get_utc_offset_minutes(&stored_offset);
    if (err == ESP_OK && stored_offset == SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES)
    {
        s_utc_offset_minutes = stored_offset;
        return;
    }

    s_utc_offset_minutes = SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES;
    if (err == ESP_OK)
    {
        ESP_LOGW(TAG,
                 "当前产品固定使用 UTC+8，已将不支持的偏移 %d 分钟恢复为 +%d 分钟",
                 stored_offset,
                 SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES);
        const esp_err_t persist_err = system_storage_set_utc_offset_minutes(SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES);
        if (persist_err != ESP_OK)
        {
            ESP_LOGW(TAG, "保存固定 UTC+8 偏移失败：%s", esp_err_to_name(persist_err));
        }
        return;
    }
    if (err != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG,
                 "读取 UTC 偏移失败，采用默认 +%d 分钟：%s",
                 SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES,
                 esp_err_to_name(err));
        return;
    }

    const esp_err_t persist_err = system_storage_set_utc_offset_minutes(SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES);
    if (persist_err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "保存默认 UTC 偏移失败，本次仍采用 +%d 分钟：%s",
                 SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES,
                 esp_err_to_name(persist_err));
    }
}

/**
 * @brief 配置 C 运行库本地时区，使日志和 localtime 使用产品固定 UTC+8
 *
 * @return ESP_OK 配置成功；ESP_FAIL 写入 TZ 环境变量失败
 */
static esp_err_t system_clock_configure_timezone(void)
{
    if (setenv("TZ", SYSTEM_CLOCK_TIMEZONE, 1) != 0)
    {
        ESP_LOGE(TAG, "配置系统本地时区失败");
        return ESP_FAIL;
    }

    tzset();
    return ESP_OK;
}

/**
 * @brief 恢复 UTC 偏移并等待后续网络校时
 *
 * @return ESP_OK 系统时钟已可供后续校时使用
 */
esp_err_t system_clock_init(void)
{
    while (true)
    {
        taskENTER_CRITICAL(&s_state_lock);
        if (s_initializing)
        {
            taskEXIT_CRITICAL(&s_state_lock);
            vTaskDelay(1U);
            continue;
        }
        if (s_initialized)
        {
            taskEXIT_CRITICAL(&s_state_lock);
            return ESP_OK;
        }
        s_initializing = true;
        taskEXIT_CRITICAL(&s_state_lock);
        break;
    }

    if (s_writer_mutex == NULL)
    {
        s_writer_mutex = xSemaphoreCreateMutex();
        if (s_writer_mutex == NULL)
        {
            taskENTER_CRITICAL(&s_state_lock);
            s_initializing = false;
            taskEXIT_CRITICAL(&s_state_lock);
            ESP_LOGE(TAG, "创建系统时钟写事务互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }
    system_clock_load_utc_offset();
    const esp_err_t timezone_error = system_clock_configure_timezone();
    if (timezone_error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_initializing = false;
        taskEXIT_CRITICAL(&s_state_lock);
        return timezone_error;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_initialized  = true;
    s_initializing = false;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "系统时钟已初始化，本地时区=UTC+8，等待 RTC 或网络校时");
    return ESP_OK;
}

/**
 * @brief 获取已经校准的系统时间
 *
 * @param[out] timestamp 当前 UTC 时间戳
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 时间尚未校准；或其他错误码
 */
esp_err_t system_clock_get_time(time_t *timestamp)
{
    ESP_RETURN_ON_FALSE(timestamp != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    system_clock_snapshot_t snapshot;
    ESP_RETURN_ON_ERROR(system_clock_get_snapshot_copy(&snapshot), TAG, "读取系统时钟快照失败");
    ESP_RETURN_ON_FALSE(snapshot.valid, ESP_ERR_INVALID_STATE, TAG, "系统时间尚未校准");
    *timestamp = snapshot.utc_timestamp;
    return ESP_OK;
}

esp_err_t system_clock_schedule_rtc_alarm(time_t utc_timestamp)
{
    ESP_RETURN_ON_FALSE(system_clock_timestamp_is_valid(utc_timestamp),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "RTC 告警目标时间超出支持范围");

    time_t current_timestamp = 0;
    ESP_RETURN_ON_ERROR(system_clock_get_time(&current_timestamp), TAG, "系统时间尚不可信，无法调度 RTC 告警");
    const int64_t delay_seconds = (int64_t) utc_timestamp - (int64_t) current_timestamp;
    ESP_RETURN_ON_FALSE(delay_seconds > 0 && delay_seconds <= CONFIG_DESKMATE_RTC_ALARM_MAX_DELAY_SEC,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "RTC 告警目标不在允许的未来窗口");

    taskENTER_CRITICAL(&s_state_lock);
    const int16_t utc_offset_minutes = s_utc_offset_minutes;
    taskEXIT_CRITICAL(&s_state_lock);

    device_rtc_datetime_t datetime;
    ESP_RETURN_ON_ERROR(system_clock_utc_to_datetime(utc_timestamp, utc_offset_minutes, &datetime),
                        TAG,
                        "RTC 告警时间转换失败");
    const device_rtc_alarm_t alarm = {
        .second       = datetime.second,
        .minute       = datetime.minute,
        .hour         = datetime.hour,
        .day          = datetime.day,
        .weekday      = 0U,
        .match_fields = DEVICE_RTC_ALARM_MATCH_SECOND | DEVICE_RTC_ALARM_MATCH_MINUTE | DEVICE_RTC_ALARM_MATCH_HOUR
                        | DEVICE_RTC_ALARM_MATCH_DAY,
    };
    ESP_RETURN_ON_ERROR(device_rtc_set_alarm(&alarm), TAG, "写入设备 RTC 告警失败");
    ESP_LOGI(TAG,
             "RTC 告警已调度：UTC=%lld，本地=%04u-%02u-%02u %02u:%02u:%02u，延迟=%lld 秒",
             (long long) utc_timestamp,
             (unsigned int) datetime.year,
             (unsigned int) datetime.month,
             (unsigned int) datetime.day,
             (unsigned int) datetime.hour,
             (unsigned int) datetime.minute,
             (unsigned int) datetime.second,
             (long long) delay_seconds);
    return ESP_OK;
}

/**
 * @brief 复制获取当前系统时钟快照
 *
 * @param[out] out_snapshot 系统时钟快照输出指针
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 系统时钟尚未初始化；或系统时钟读取错误码
 */
esp_err_t system_clock_get_snapshot_copy(system_clock_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    taskENTER_CRITICAL(&s_state_lock);
    const bool    initialized        = s_initialized;
    const int16_t utc_offset_minutes = s_utc_offset_minutes;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "系统时钟未初始化");

    system_clock_snapshot_t snapshot = {
        .utc_timestamp      = 0,
        .utc_offset_minutes = utc_offset_minutes,
        .source             = SYSTEM_CLOCK_SOURCE_NONE,
        .valid              = false,
    };
    const esp_err_t trusted_err = system_clock_get_trusted_time(&snapshot.utc_timestamp, &snapshot.source);
    if (trusted_err == ESP_ERR_INVALID_STATE)
    {
        *out_snapshot = snapshot;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(trusted_err, TAG, "读取可信系统时间失败");
    if (system_clock_timestamp_is_valid(snapshot.utc_timestamp))
    {
        snapshot.valid = true;
    }
    *out_snapshot = snapshot;
    return ESP_OK;
}

/**
 * @brief 提交一个 SNTP 时间候选
 *
 * @param[in] timestamp 经 SNTP 校准的 UTC 时间戳
 * @return ESP_OK 候选已接受；ESP_ERR_INVALID_ARG 候选超出范围；
 *         SYSTEM_CLOCK_ERR_CONFIRMATION_REQUIRED 大跳变仍需第二个样本；
 *         或系统时钟错误码
 */
esp_err_t system_clock_set_time(time_t timestamp)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(initialized && s_writer_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "系统时钟未初始化");
    ESP_RETURN_ON_FALSE(system_clock_timestamp_is_valid(timestamp), ESP_ERR_INVALID_ARG, TAG, "SNTP 时间超出可信范围");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_writer_mutex, portMAX_DELAY) == pdTRUE,
                        ESP_FAIL,
                        TAG,
                        "取得系统时钟写事务互斥锁失败");

    esp_err_t       result = ESP_OK;
    time_t          trusted_timestamp;
    const esp_err_t trusted_err = system_clock_get_trusted_time(&trusted_timestamp, NULL);
    if (trusted_err == ESP_OK)
    {
        const int64_t jump_seconds = system_clock_timestamp_difference(timestamp, trusted_timestamp);
        const bool    pending_candidate_matches =
            s_confirmation_pending
            && system_clock_timestamp_difference(timestamp, s_pending_confirmation_timestamp)
                   <= SYSTEM_CLOCK_CONFIRMATION_TOLERANCE_SECONDS;
        const bool confirmation_required =
            (s_confirmation_pending && !pending_candidate_matches)
            || (!s_confirmation_pending && jump_seconds > SYSTEM_CLOCK_MAX_UNCONFIRMED_JUMP_SECONDS);
        if (confirmation_required)
        {
            s_pending_confirmation_timestamp = timestamp;
            s_confirmation_pending           = true;
            result                           = system_clock_set_system_time(trusted_timestamp);
            if (result != ESP_OK)
            {
                ESP_LOGE(TAG, "恢复大跳变前的可信系统时间失败：%s", esp_err_to_name(result));
                goto done;
            }
            ESP_LOGW(TAG, "SNTP 候选与可信时间相差 %lld 秒，等待第二个相近样本确认", (long long) jump_seconds);
            result = SYSTEM_CLOCK_ERR_CONFIRMATION_REQUIRED;
            goto done;
        }
    }
    else if (trusted_err != ESP_ERR_INVALID_STATE)
    {
        result = trusted_err;
        ESP_LOGE(TAG, "读取当前可信时间失败：%s", esp_err_to_name(result));
        goto done;
    }

    s_confirmation_pending = false;
    result                 = system_clock_accept_trusted_time(timestamp, SYSTEM_CLOCK_SOURCE_SNTP);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "接受 SNTP 时间失败：%s", esp_err_to_name(result));
        goto done;
    }
    system_clock_write_sntp_time_to_rtc(timestamp);

done:
    xSemaphoreGive(s_writer_mutex);
    return result;
}

esp_err_t system_clock_sync_from_sntp(const char *server, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(server != NULL && server[0] != '\0' && timeout_ms > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "SNTP 校时参数无效");
    taskENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "系统时钟未初始化");

    const int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "开始 SNTP 网络校时：server=%s，单样本超时=%lu ms", server, (unsigned long) timeout_ms);

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    esp_err_t         result = esp_netif_sntp_init(&config);
    if (result != ESP_OK)
    {
        const uint64_t total_ms = (uint64_t) (esp_timer_get_time() - started_us + 999LL) / 1000ULL;
        ESP_LOGE(TAG,
                 "SNTP 网络校时失败：server=%s，阶段=初始化客户端，total=%llu ms，错误=%s",
                 server,
                 (unsigned long long) total_ms,
                 esp_err_to_name(result));
        return result;
    }

    uint8_t     sample_count          = 0U;
    time_t      candidate             = 0;
    time_t      trusted_at_sample     = 0;
    bool        had_trusted_at_sample = false;
    const char *failure_stage         = "等待第一个 SNTP 样本";
    while (sample_count < 2U)
    {
        result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
        if (result != ESP_OK)
        {
            break;
        }

        ++sample_count;
        failure_stage = "读取 SNTP 样本";
        if (time(&candidate) == (time_t) -1)
        {
            result = ESP_FAIL;
            break;
        }

        failure_stage         = "接受 SNTP 样本";
        had_trusted_at_sample = system_clock_get_trusted_time(&trusted_at_sample, NULL) == ESP_OK;
        result                = system_clock_set_time(candidate);
        if (result != SYSTEM_CLOCK_ERR_CONFIRMATION_REQUIRED || sample_count >= 2U)
        {
            break;
        }

        ESP_LOGW(TAG, "SNTP 大跳变需要二次确认，立即请求第二个连续样本");
        failure_stage = "请求第二个 SNTP 样本";
        result        = esp_netif_sntp_start();
        if (result != ESP_OK)
        {
            break;
        }
        failure_stage = "等待第二个 SNTP 样本";
    }

    esp_netif_sntp_deinit();
    const uint64_t total_ms = (uint64_t) (esp_timer_get_time() - started_us + 999LL) / 1000ULL;
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "SNTP 网络校时失败：server=%s，阶段=%s，样本=%u，total=%llu ms，错误=%s",
                 server,
                 failure_stage,
                 (unsigned int) sample_count,
                 (unsigned long long) total_ms,
                 esp_err_to_name(result));
        return result;
    }

    struct tm local_time;
    if (localtime_r(&candidate, &local_time) == NULL)
    {
        ESP_LOGI(TAG,
                 "SNTP 网络校时完成：server=%s，样本=%u，UTC=%lld，total=%llu ms",
                 server,
                 (unsigned int) sample_count,
                 (long long) candidate,
                 (unsigned long long) total_ms);
        return ESP_OK;
    }

    const int64_t correction_seconds = had_trusted_at_sample ? (int64_t) candidate - (int64_t) trusted_at_sample : 0LL;
    ESP_LOGI(TAG,
             "SNTP 网络校时完成：server=%s，样本=%u，本地时间=%04d-%02d-%02d %02d:%02d:%02d，"
             "校正=%lld 秒，total=%llu ms",
             server,
             (unsigned int) sample_count,
             local_time.tm_year + 1900,
             local_time.tm_mon + 1,
             local_time.tm_mday,
             local_time.tm_hour,
             local_time.tm_min,
             local_time.tm_sec,
             (long long) correction_seconds,
             (unsigned long long) total_ms);
    return ESP_OK;
}

/** @brief 从设备 RTC 接受可信日历时间并更新系统墙上时钟 */
esp_err_t system_clock_sync_from_rtc(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool    initialized        = s_initialized;
    const int16_t utc_offset_minutes = s_utc_offset_minutes;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(initialized && s_writer_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "系统时钟未初始化");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_writer_mutex, portMAX_DELAY) == pdTRUE,
                        ESP_FAIL,
                        TAG,
                        "取得系统时钟写事务互斥锁失败");

    device_rtc_snapshot_t snapshot;
    esp_err_t             result = device_rtc_get_snapshot_copy(&snapshot);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "读取 RTC 快照失败：%s", esp_err_to_name(result));
        goto done;
    }
    if (snapshot.voltage_low)
    {
        result = ESP_ERR_INVALID_STATE;
        ESP_LOGW(TAG, "RTC 电压过低标志仍置位，拒绝将其作为可信系统时间");
        goto done;
    }

    time_t timestamp;
    result = system_clock_datetime_to_utc(&snapshot.datetime, utc_offset_minutes, &timestamp);
    if (result != ESP_OK)
    {
        goto done;
    }
    result = system_clock_accept_trusted_time(timestamp, SYSTEM_CLOCK_SOURCE_RTC);
    if (result == ESP_OK)
    {
        s_confirmation_pending = false;
        ESP_LOGI(TAG,
                 "系统时间已从 RTC 校准: %04u-%02u-%02u %02u:%02u:%02u (UTC+08:00)",
                 (unsigned int) snapshot.datetime.year,
                 (unsigned int) snapshot.datetime.month,
                 (unsigned int) snapshot.datetime.day,
                 (unsigned int) snapshot.datetime.hour,
                 (unsigned int) snapshot.datetime.minute,
                 (unsigned int) snapshot.datetime.second);
    }

done:
    (void) xSemaphoreGive(s_writer_mutex);
    return result;
}

/**
 * @brief 判断系统时间是否已经由可信来源校准
 *
 * @return true 已校准，false 尚未校准
 */
bool system_clock_is_valid(void)
{
    system_clock_snapshot_t snapshot;
    return system_clock_get_snapshot_copy(&snapshot) == ESP_OK && snapshot.valid;
}
