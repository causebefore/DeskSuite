/**
 * @file power_management_app_task.cpp
 * @brief 实现手动与周期深睡的有序停机、显示握手、退避保留和 Task 生命周期
 */
#include "power_management_app_internal.hpp"

#include <cstdint>
#include <time.h>

#include "content_refresh_app.h"
#include "device_buzzer.h"
#include "device_display.h"
#include "device_led.h"
#include "device_power.h"
#include "device_sd.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "firmware_ota.h"
#include "network_manager.h"
#include "photo_playback_app.h"
#include "remote_log.h"
#include "sd_card_service.h"
#include "system_clock.h"
#include "system_storage.h"

/** @brief 日志标签 */
static const char *TAG = "power_management";
/** @brief 跨深睡保留状态格式标识 */
static constexpr uint32_t POWER_MANAGEMENT_RETAINED_MAGIC = 0x50575232U;
/** @brief 跨深睡保留状态格式版本 */
static constexpr uint16_t POWER_MANAGEMENT_RETAINED_VERSION = 3U;
/** @brief 跨深睡保留状态校验盐值 */
static constexpr uint32_t POWER_MANAGEMENT_RETAINED_CHECK_SALT = 0x6D4E91A7U;
/** @brief 普通刷新失败后的三档深睡退避 */
static constexpr uint32_t POWER_MANAGEMENT_BACKOFF_SECONDS[] = {
    60U,
    300U,
    900U,
};
/** @brief RTC 时间不可信时，长期失败计划使用的一小时相对兜底间隔 */
static constexpr uint32_t POWER_MANAGEMENT_LONG_FAILURE_FALLBACK_SECONDS = 3600U;
/** @brief 长期失败绝对计划使用的整点周期 */
static constexpr int64_t POWER_MANAGEMENT_PLAN_HOUR_SECONDS = 3600LL;
/** @brief 服务端绝对时间已过期时尽快重新联网取得新计划 */
static constexpr uint32_t POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS = 60U;
/** @brief 绝对目标深睡额外延后秒数，避免内部慢时钟漂移导致提前唤醒 */
static constexpr uint32_t POWER_MANAGEMENT_ABSOLUTE_WAKEUP_DELAY_SECONDS = 10U;
/** @brief 直接关网路径等待远端日志同步上传调用退出的上限 */
static constexpr uint32_t POWER_MANAGEMENT_REMOTE_LOG_STOP_TIMEOUT_MS = 10000U;
/** @brief OTA 交互等待 Network Manager 上线的总上限 */
static constexpr uint32_t POWER_MANAGEMENT_OTA_NETWORK_WAIT_MS = 30000U;
/** @brief OTA 结果页和更新确认页无人操作超时 */
static constexpr uint32_t POWER_MANAGEMENT_OTA_PROMPT_TIMEOUT_MS = 180000U;
/** @brief OTA 检查请求短提示音频率 */
static constexpr uint32_t POWER_MANAGEMENT_OTA_TONE_FREQUENCY_HZ = 2000U;
/** @brief OTA 检查请求短提示音占空比 */
static constexpr uint8_t POWER_MANAGEMENT_OTA_TONE_DUTY_PERCENT = 5U;
/** @brief OTA 检查请求短提示音持续时间 */
static constexpr uint32_t POWER_MANAGEMENT_OTA_TONE_DURATION_MS = 40U;
/** @brief OTA 状态页相邻文本行的垂直间距 */
static constexpr uint16_t POWER_MANAGEMENT_OTA_LINE_GAP_PIXELS = 24U;

/** @brief 跨深睡保留的刷新退避与绝对唤醒状态，不写入 Flash */
struct PowerRetainedSchedule
{
    uint32_t magic;
    uint16_t version;
    uint8_t consecutive_failures;
    uint8_t reserved;
    int64_t absolute_wakeup_at_utc;
    uint32_t checksum;
};

/** @brief 一次深睡计划，可同时携带定时器间隔与 RTC 保留的绝对目标 */
struct PowerSleepSchedule
{
    uint32_t wakeup_seconds = 0U;
    int64_t wakeup_at_utc = 0;
};

/** @brief 计算严格晚于当前时刻的下一本地整点所对应的 UTC Unix 秒 */
static constexpr int64_t power_management_calculate_next_hour_at_utc(
    int64_t now_utc, int16_t utc_offset_minutes)
{
    const int64_t local_timestamp =
        now_utc + static_cast<int64_t>(utc_offset_minutes) * 60LL;
    const int64_t seconds_into_hour =
        ((local_timestamp % POWER_MANAGEMENT_PLAN_HOUR_SECONDS)
         + POWER_MANAGEMENT_PLAN_HOUR_SECONDS)
        % POWER_MANAGEMENT_PLAN_HOUR_SECONDS;
    return now_utc + POWER_MANAGEMENT_PLAN_HOUR_SECONDS - seconds_into_hour;
}

static_assert(power_management_calculate_next_hour_at_utc(0, 0) == 3600);
static_assert(power_management_calculate_next_hour_at_utc(3599, 0) == 3600);
static_assert(power_management_calculate_next_hour_at_utc(3600, 0) == 7200);
static_assert(power_management_calculate_next_hour_at_utc(0, 330) == 1800);
static_assert(power_management_calculate_next_hour_at_utc(0, -210) == 1800);

/** @brief 深睡复位后仍保留的调度状态 */
RTC_DATA_ATTR static PowerRetainedSchedule s_retained_schedule;

/** @brief 本次停机已经成功停止、需要在失败时恢复的组件集合 */
struct PowerStoppedComponents
{
    bool photo_playback = false;
    bool content_refresh = false;
    bool firmware_ota = false;
    bool remote_log = false;
    bool sd_card = false;
    bool network = false;
    bool wakeup_prepared = false;
    bool led_turned_off = false;
};

/** @brief 目标集合当前能否安全进入自动深睡 */
enum class PowerDisplayReadiness : uint8_t
{
    Waiting = 0,
    Ready,
    Blocked,
};

/** @brief 电源 Task 内部拥有的 OTA 交互阶段 */
enum class PowerOtaInteraction : uint8_t
{
    Inactive = 0,
    CheckPending,
    Checking,
    UpdateAvailable,
    Installing,
    ResultPage,
    Blocked,
};

/** @brief 计算 RTC 保留调度字段的轻量完整性校验 */
static uint32_t power_management_retained_checksum(const PowerRetainedSchedule &state)
{
    const uint64_t absolute_target = static_cast<uint64_t>(state.absolute_wakeup_at_utc);
    return state.magic ^ (static_cast<uint32_t>(state.version) << 16U)
           ^ static_cast<uint32_t>(state.consecutive_failures)
           ^ static_cast<uint32_t>(absolute_target)
           ^ static_cast<uint32_t>(absolute_target >> 32U)
           ^ POWER_MANAGEMENT_RETAINED_CHECK_SALT;
}

/** @brief 校验并在需要时初始化跨深睡退避状态 */
static void power_management_ensure_retained_schedule()
{
    const bool valid = s_retained_schedule.magic == POWER_MANAGEMENT_RETAINED_MAGIC
                       && s_retained_schedule.version == POWER_MANAGEMENT_RETAINED_VERSION
                       && s_retained_schedule.reserved == 0U
                       && s_retained_schedule.checksum
                              == power_management_retained_checksum(s_retained_schedule);
    if (valid)
    {
        return;
    }
    s_retained_schedule = {};
    s_retained_schedule.magic = POWER_MANAGEMENT_RETAINED_MAGIC;
    s_retained_schedule.version = POWER_MANAGEMENT_RETAINED_VERSION;
    s_retained_schedule.checksum =
        power_management_retained_checksum(s_retained_schedule);
}

/** @brief 更新跨深睡保留的绝对唤醒目标，0 表示清除 */
static void power_management_set_retained_wakeup_target(int64_t wakeup_at_utc)
{
    power_management_ensure_retained_schedule();
    s_retained_schedule.absolute_wakeup_at_utc = wakeup_at_utc > 0 ? wakeup_at_utc : 0;
    s_retained_schedule.checksum =
        power_management_retained_checksum(s_retained_schedule);
}

/** @brief 复制并消费回调写入的最新刷新轮次 */
static bool power_management_take_round_event(content_refresh_app_round_event_t *out_event)
{
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const bool pending = g_power_management_runtime.round_event_pending;
    if (pending)
    {
        *out_event = g_power_management_runtime.latest_round_event;
        g_power_management_runtime.round_event_pending = false;
    }
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    return pending;
}

/** @brief 复制并消费 OTA Task 回调写入的最新事务完成事件 */
static bool power_management_take_firmware_ota_event(firmware_ota_event_t *out_event)
{
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const bool pending = g_power_management_runtime.firmware_ota_event_pending;
    if (pending)
    {
        *out_event = g_power_management_runtime.latest_firmware_ota_event;
        g_power_management_runtime.firmware_ota_event_pending = false;
    }
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    return pending;
}

/** @brief 发布下一次唤醒周期和当前等待的集合代数 */
static void power_management_publish_schedule(uint32_t wakeup_seconds,
                                              int64_t wakeup_at_utc,
                                              uint64_t collection_generation)
{
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    g_power_management_runtime.status.scheduled_wakeup_seconds = wakeup_seconds;
    g_power_management_runtime.status.scheduled_wakeup_at_utc = wakeup_at_utc;
    g_power_management_runtime.status.target_collection_generation =
        collection_generation;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
}

/**
 * @brief 复制需要由有界失败休眠收敛的终态错误
 *
 * OTA 正在写入固件时保持 INSTALLING 状态，不会被此处误判为可强制休眠的错误。
 *
 * @param[out] out_error 阻塞原因；状态未携带错误时使用 ESP_FAIL
 * @return true 表示应立即进入失败退避休眠
 */
static bool power_management_get_failure_sleep_error(esp_err_t *out_error)
{
    power_management_app_state_t state;
    esp_err_t                    error;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    state = g_power_management_runtime.status.state;
    error = g_power_management_runtime.status.last_error;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);

    if (state != POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED
        && state != POWER_MANAGEMENT_APP_STATE_CLEANUP_FAILED)
    {
        return false;
    }
    *out_error = error != ESP_OK ? error : ESP_FAIL;
    return true;
}

/** @brief 在最新可信系统时间上把绝对目标换算为深睡定时器相对秒数 */
static uint32_t power_management_resolve_wakeup_seconds(int64_t wakeup_at_utc)
{
    if (wakeup_at_utc <= 0)
    {
        ESP_LOGE(TAG,
                 "绝对唤醒目标无效，%lu 秒后重新联网获取计划",
                 (unsigned long) POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS);
        return POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS;
    }

    system_clock_snapshot_t snapshot = {};
    const esp_err_t clock_error = system_clock_get_snapshot_copy(&snapshot);
    if (clock_error != ESP_OK || !snapshot.valid)
    {
        ESP_LOGW(TAG,
                 "系统时间不可信，无法换算绝对唤醒目标，%lu 秒后重新校时",
                 (unsigned long) POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS);
        return POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS;
    }
    if (wakeup_at_utc <= (int64_t) snapshot.utc_timestamp)
    {
        ESP_LOGW(TAG,
                 "绝对唤醒目标已经到期，%lu 秒后重新联网获取计划："
                 "wakeup_at=%lld, now=%lld",
                 (unsigned long) POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS,
                 (long long) wakeup_at_utc,
                 (long long) snapshot.utc_timestamp);
        return POWER_MANAGEMENT_EXPIRED_TARGET_RETRY_SECONDS;
    }

    const uint64_t remaining_seconds =
        (uint64_t) (wakeup_at_utc - (int64_t) snapshot.utc_timestamp);
    if (remaining_seconds > UINT32_MAX - POWER_MANAGEMENT_ABSOLUTE_WAKEUP_DELAY_SECONDS)
    {
        return UINT32_MAX;
    }
    const uint32_t wakeup_seconds =
        (uint32_t) remaining_seconds + POWER_MANAGEMENT_ABSOLUTE_WAKEUP_DELAY_SECONDS;
    ESP_LOGI(TAG,
             "绝对目标深睡间隔增加 %lu 秒防提前补偿: target=%lld, now=%lld, interval=%lu 秒",
             (unsigned long) POWER_MANAGEMENT_ABSOLUTE_WAKEUP_DELAY_SECONDS,
             (long long) wakeup_at_utc,
             (long long) snapshot.utc_timestamp,
             (unsigned long) wakeup_seconds);
    return wakeup_seconds;
}

/** @brief 用可信 RTC 时间生成严格对齐下一本地整点的长期失败计划 */
static PowerSleepSchedule power_management_calculate_long_failure_schedule()
{
    PowerSleepSchedule schedule = {};
    system_clock_snapshot_t snapshot = {};
    const esp_err_t clock_error = system_clock_get_snapshot_copy(&snapshot);
    if (clock_error != ESP_OK || !snapshot.valid)
    {
        schedule.wakeup_seconds = POWER_MANAGEMENT_LONG_FAILURE_FALLBACK_SECONDS;
        ESP_LOGW(TAG,
                 "系统时间不可信，长期失败计划退回相对 %lu 秒: error=%s",
                 (unsigned long) schedule.wakeup_seconds,
                 esp_err_to_name(clock_error != ESP_OK ? clock_error
                                                       : ESP_ERR_INVALID_STATE));
        return schedule;
    }

    schedule.wakeup_at_utc = power_management_calculate_next_hour_at_utc(
        (int64_t) snapshot.utc_timestamp, snapshot.utc_offset_minutes);
    const uint32_t remaining_seconds =
        (uint32_t) (schedule.wakeup_at_utc - (int64_t) snapshot.utc_timestamp);
    schedule.wakeup_seconds =
        remaining_seconds + POWER_MANAGEMENT_ABSOLUTE_WAKEUP_DELAY_SECONDS;
    ESP_LOGI(TAG,
             "连续失败超过三次，长期重试对齐可信 RTC 的下一本地整点: "
             "wakeup_at=%lld, now=%lld, utc_offset=%d 分钟, interval=%lu 秒",
             (long long) schedule.wakeup_at_utc,
             (long long) snapshot.utc_timestamp,
             (int) snapshot.utc_offset_minutes,
             (unsigned long) schedule.wakeup_seconds);
    return schedule;
}

/**
 * @brief 根据最新成功或失败事实更新 RTC 退避状态并返回下一次深睡计划
 *
 * 前三次失败返回 1/5/15 分钟相对计划；继续失败时返回可信 RTC 下一本地整点的绝对计划。
 * RTC 时间不可信时，长期计划退回一小时相对间隔，保证设备仍能恢复联网校时。
 *
 * @param[in] error ESP_OK 表示完整轮次成功，其他值表示一次连续失败
 * @return 下一次深睡计划；成功时两个字段均为 0
 */
static PowerSleepSchedule power_management_update_retained_schedule(esp_err_t error)
{
    power_management_ensure_retained_schedule();
    PowerSleepSchedule schedule = {};
    if (error == ESP_OK)
    {
        s_retained_schedule.consecutive_failures = 0U;
    }
    else
    {
        if (s_retained_schedule.consecutive_failures < UINT8_MAX)
        {
            ++s_retained_schedule.consecutive_failures;
        }
        const uint8_t failures = s_retained_schedule.consecutive_failures;
        if (failures <= 3U)
        {
            schedule.wakeup_seconds = POWER_MANAGEMENT_BACKOFF_SECONDS[failures - 1U];
        }
        else
        {
            schedule = power_management_calculate_long_failure_schedule();
        }
    }
    s_retained_schedule.checksum =
        power_management_retained_checksum(s_retained_schedule);
    return schedule;
}

esp_err_t power_management_app_apply_startup_time_gate(bool woken_by_button,
                                                       bool woken_by_timer)
{
    power_management_ensure_retained_schedule();
    if (woken_by_button || !woken_by_timer)
    {
        if (s_retained_schedule.absolute_wakeup_at_utc > 0)
        {
            ESP_LOGI(TAG, "本次不是纯定时器唤醒，清除上一轮绝对唤醒目标");
            power_management_set_retained_wakeup_target(0);
        }
        return ESP_OK;
    }

    const int64_t wakeup_at_utc = s_retained_schedule.absolute_wakeup_at_utc;
    if (wakeup_at_utc <= 0)
    {
        ESP_LOGI(TAG, "定时器唤醒没有绝对目标，按相对退避或兼容流程继续启动");
        return ESP_OK;
    }

    system_clock_snapshot_t snapshot = {};
    const esp_err_t clock_error = system_clock_get_snapshot_copy(&snapshot);
    if (clock_error != ESP_OK || !snapshot.valid)
    {
        const esp_err_t result = clock_error != ESP_OK ? clock_error : ESP_ERR_INVALID_STATE;
        ESP_LOGW(TAG,
                 "系统时间不可信，无法执行绝对目标时间门禁，继续启动以便联网校时: %s",
                 esp_err_to_name(result));
        return result;
    }

    if ((int64_t) snapshot.utc_timestamp >= wakeup_at_utc)
    {
        ESP_LOGI(TAG,
                 "绝对目标时间门禁已放行: target=%lld, now=%lld, 滞后=%lld 秒",
                 (long long) wakeup_at_utc,
                 (long long) snapshot.utc_timestamp,
                 (long long) ((int64_t) snapshot.utc_timestamp - wakeup_at_utc));
        power_management_set_retained_wakeup_target(0);
        return ESP_OK;
    }

    const uint32_t wakeup_seconds = power_management_resolve_wakeup_seconds(wakeup_at_utc);
    ESP_LOGW(TAG,
             "内部定时器提前唤醒，绝对目标时间门禁重新深睡: target=%lld, now=%lld, "
             "含防提前补偿间隔=%lu 秒",
             (long long) wakeup_at_utc,
             (long long) snapshot.utc_timestamp,
             (unsigned long) wakeup_seconds);
    const uint64_t timer_wakeup_us = static_cast<uint64_t>(wakeup_seconds) * 1000000ULL;
    const esp_err_t prepare_error = device_power_prepare_deep_sleep(timer_wakeup_us);
    if (prepare_error != ESP_OK)
    {
        ESP_LOGE(TAG, "绝对目标时间门禁重新配置深睡失败: %s",
                 esp_err_to_name(prepare_error));
        return prepare_error;
    }
    device_power_start_deep_sleep();
}

/** @brief 记录由绝对计划或短期失败退避计算出的深睡定时器间隔 */
static void power_management_log_wakeup_schedule(
    uint32_t wakeup_seconds, esp_err_t error, int64_t wakeup_at_utc)
{
    const uint32_t hours   = wakeup_seconds / 3600U;
    const uint32_t minutes = (wakeup_seconds % 3600U) / 60U;
    const uint32_t seconds = wakeup_seconds % 60U;
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "深睡唤醒计划: 服务端 UTC 目标=%lld, 当前预计间隔=%lu 秒"
                 "（%lu 小时 %lu 分 %lu 秒）",
                 (long long) wakeup_at_utc,
                 (unsigned long) wakeup_seconds,
                 (unsigned long) hours,
                 (unsigned long) minutes,
                 (unsigned long) seconds);
    }
    else if (wakeup_at_utc > 0)
    {
        ESP_LOGW(TAG,
                 "长期失败深睡计划: RTC 下一本地整点 UTC 目标=%lld, "
                 "当前预计间隔=%lu 秒（%lu 小时 %lu 分 %lu 秒），"
                 "连续失败=%u 次, error=%s",
                 (long long) wakeup_at_utc,
                 (unsigned long) wakeup_seconds,
                 (unsigned long) hours,
                 (unsigned long) minutes,
                 (unsigned long) seconds,
                 (unsigned int) s_retained_schedule.consecutive_failures,
                 esp_err_to_name(error));
    }
    else
    {
        ESP_LOGW(TAG,
                 "深睡定时器间隔计划: 进入深睡后 %lu 秒（%lu 小时 %lu 分 %lu 秒），"
                 "依据连续失败第 %u 次退避，error=%s",
                 (unsigned long) wakeup_seconds,
                 (unsigned long) hours,
                 (unsigned long) minutes,
                 (unsigned long) seconds,
                 (unsigned int) s_retained_schedule.consecutive_failures,
                 esp_err_to_name(error));
    }
}

/** @brief 在系统时钟可信时输出预计的本地绝对唤醒时间 */
static void power_management_log_absolute_wakeup(uint32_t wakeup_seconds)
{
    if (wakeup_seconds == 0U)
    {
        return;
    }
    system_clock_snapshot_t snapshot = {};
    const esp_err_t clock_error = system_clock_get_snapshot_copy(&snapshot);
    if (clock_error != ESP_OK || !snapshot.valid)
    {
        ESP_LOGI(TAG,
                 "系统时间尚不可信，仅记录相对唤醒间隔: 进入深睡后 %lu 秒",
                 (unsigned long) wakeup_seconds);
        return;
    }

    const time_t local_timestamp =
        snapshot.utc_timestamp + (time_t) wakeup_seconds
        + (time_t) snapshot.utc_offset_minutes * 60;
    struct tm local_time = {};
    if (gmtime_r(&local_timestamp, &local_time) == nullptr)
    {
        ESP_LOGW(TAG, "换算预计定时唤醒时间失败，仅保留相对间隔");
        return;
    }
    const int offset = snapshot.utc_offset_minutes;
    const unsigned int offset_abs = (unsigned int) (offset < 0 ? -offset : offset);
    ESP_LOGI(TAG,
             "预计定时唤醒时间: %04d-%02d-%02d %02d:%02d:%02d (UTC%c%02u:%02u)，"
             "计时从实际进入深睡时开始",
             local_time.tm_year + 1900,
             local_time.tm_mon + 1,
             local_time.tm_mday,
             local_time.tm_hour,
             local_time.tm_min,
             local_time.tm_sec,
             offset >= 0 ? '+' : '-',
             offset_abs / 60U,
             offset_abs % 60U);
}

/**
 * @brief 检查照片播放 App 是否已稳定显示目标集合
 *
 * @param[in] target_generation 刷新轮次结束时的集合代数
 * @param[out] out_error 阻止自动休眠的显示错误
 * @return Ready 可休眠；Waiting 仍在刷新；Blocked 显示链路处于错误终态
 */
static PowerDisplayReadiness power_management_check_display(uint64_t target_generation,
                                                            esp_err_t *out_error)
{
    photo_playback_app_status_t status = {};
    const esp_err_t error = photo_playback_app_get_status_copy(&status);
    if (error != ESP_OK)
    {
        *out_error = error;
        return PowerDisplayReadiness::Blocked;
    }
    if (status.state == PHOTO_PLAYBACK_APP_STATE_ERROR
        || status.state == PHOTO_PLAYBACK_APP_STATE_CLEANUP_FAILED
        || (status.state == PHOTO_PLAYBACK_APP_STATE_IDLE && status.last_error != ESP_OK))
    {
        *out_error = status.last_error != ESP_OK ? status.last_error : ESP_FAIL;
        return PowerDisplayReadiness::Blocked;
    }
    if (status.state == PHOTO_PLAYBACK_APP_STATE_NO_CONTENT)
    {
        if (status.last_error != ESP_OK && status.last_error != ESP_ERR_NOT_FOUND)
        {
            *out_error = status.last_error;
            return PowerDisplayReadiness::Blocked;
        }
        if (status.collection_settled
            && status.settled_collection_generation >= target_generation)
        {
            *out_error = ESP_OK;
            return PowerDisplayReadiness::Ready;
        }
        return PowerDisplayReadiness::Waiting;
    }
    if (status.state == PHOTO_PLAYBACK_APP_STATE_IDLE && status.collection_settled
        && status.settled_collection_generation >= target_generation)
    {
        *out_error = ESP_OK;
        return PowerDisplayReadiness::Ready;
    }
    return PowerDisplayReadiness::Waiting;
}

/**
 * @brief 调用可选组件的停止 API，并把“未运行”视为无需停止
 *
 * @param[in] name 组件日志名称
 * @param[in] stop 停止函数
 * @param[out] out_stopped 返回 ESP_OK 时标记确实完成了停止
 * @return ESP_OK 已停止或原本未运行；其他值表示停止失败
 */
static esp_err_t power_management_stop_optional(const char *name, esp_err_t (*stop)(void),
                                                bool *out_stopped)
{
    ESP_LOGI(TAG, "开始停止%s", name);
    const esp_err_t error = stop();
    if (error == ESP_ERR_INVALID_STATE)
    {
        *out_stopped = false;
        ESP_LOGI(TAG, "%s当前未运行，无需停止", name);
        return ESP_OK;
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "停止%s失败: %s", name, esp_err_to_name(error));
        return error;
    }
    *out_stopped = true;
    ESP_LOGI(TAG, "%s已安全停止", name);
    return ESP_OK;
}

/** @brief 把带超时参数的远端日志停止接口适配为通用可选组件停止函数 */
static esp_err_t power_management_stop_remote_log()
{
    return remote_log_stop(POWER_MANAGEMENT_REMOTE_LOG_STOP_TIMEOUT_MS);
}

/**
 * @brief 已持久化配网意图后按依赖顺序停止运行期组件并重启
 *
 * 停止失败只记录诊断；一次性意图已经落盘，新启动仍会收敛到现有 Portal。
 */
[[noreturn]] static void power_management_restart_for_provisioning()
{
    ESP_LOGI(TAG, "开始为配网请求有序停止运行期组件");
    bool stopped = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        power_management_stop_optional("照片播放 App", photo_playback_app_stop, &stopped));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        power_management_stop_optional("内容刷新 App", content_refresh_app_stop, &stopped));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        power_management_stop_optional("固件 OTA Task", firmware_ota_stop, &stopped));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        power_management_stop_optional("远端日志 Task", power_management_stop_remote_log, &stopped));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        power_management_stop_optional("SD 卡 Service", sd_card_service_stop, &stopped));

    network_manager_status_t network_status = {};
    const esp_err_t status_error = network_manager_get_status_copy(&network_status);
    if (status_error == ESP_OK && network_status.state != NETWORK_STATE_STOPPED)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            power_management_stop_optional("网络会话", network_manager_stop, &stopped));
    }
    else if (status_error != ESP_OK && status_error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "配网重启前读取网络状态失败: %s", esp_err_to_name(status_error));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_buzzer_stop());
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_led_off());
    ESP_LOGI(TAG, "运行期配网意图已保存，立即重启进入现有 Portal");
    esp_restart();
}

/** @brief 记录首个回滚错误，同时继续恢复其余已经停止的组件 */
static void power_management_record_rollback_error(esp_err_t error,
                                                   esp_err_t *inout_first_error)
{
    if (error != ESP_OK && *inout_first_error == ESP_OK)
    {
        *inout_first_error = error;
    }
}

/**
 * @brief 在深睡准备失败后按依赖顺序恢复已经停止的运行期组件
 *
 * @return ESP_OK 已恢复；或首个恢复错误码
 */
static esp_err_t power_management_rollback(PowerStoppedComponents *stopped)
{
    ESP_LOGW(TAG, "开始恢复深睡准备期间已经停止的运行期组件");
    esp_err_t rollback_error = ESP_OK;
    if (stopped->wakeup_prepared)
    {
        power_management_record_rollback_error(device_power_cancel_deep_sleep(), &rollback_error);
        stopped->wakeup_prepared = false;
    }
    if (stopped->sd_card)
    {
        power_management_record_rollback_error(sd_card_service_start(), &rollback_error);
    }
    if (stopped->firmware_ota)
    {
        power_management_record_rollback_error(firmware_ota_start(), &rollback_error);
    }
    if (stopped->photo_playback)
    {
        power_management_record_rollback_error(photo_playback_app_start(), &rollback_error);
    }
    if (stopped->content_refresh)
    {
        power_management_record_rollback_error(content_refresh_app_start(), &rollback_error);
    }
    else if (stopped->network)
    {
        power_management_record_rollback_error(network_manager_start(), &rollback_error);
    }
    if (stopped->remote_log)
    {
        power_management_record_rollback_error(remote_log_start(), &rollback_error);
    }
    if (stopped->led_turned_off)
    {
        power_management_record_rollback_error(device_led_on(), &rollback_error);
    }
    return rollback_error;
}

/**
 * @brief 同步准备深睡；仅在失败时返回
 *
 * 停止顺序确保显示和 SD 使用者先退出，再卸载文件系统。按键与定时器唤醒配置在
 * 面板深睡前完成，因而配置或面板休眠失败仍可回滚到可运行状态。
 *
 * @param[in] timer_wakeup_seconds 定时唤醒间隔；0 表示本次仅由按键唤醒
 * @param[in] absolute_wakeup_at_utc 服务端计划或长期失败整点的绝对唤醒目标；
 *                                      0 表示短期相对退避或手动休眠
 * @param[in] allow_missing_components true 表示启动失败收敛允许组件尚未初始化
 * @param[out] out_cleanup_failed true 表示失败后的运行期恢复也失败
 * @return 原始停机错误；成功进入深睡时不返回
 */
static esp_err_t power_management_prepare_and_sleep(uint32_t timer_wakeup_seconds,
                                                    int64_t absolute_wakeup_at_utc,
                                                    bool allow_missing_components,
                                                    bool *out_cleanup_failed)
{
    ESP_LOGI(TAG,
             "开始执行整机深睡停机流程: 定时唤醒=%s, 间隔=%lu 秒",
             timer_wakeup_seconds > 0U ? "启用" : "关闭",
             (unsigned long) timer_wakeup_seconds);
    PowerStoppedComponents stopped;
    bool                   display_slept = false;
    bool                   stop_failed   = false;
    esp_err_t error = power_management_stop_optional(
        "照片播放 App", photo_playback_app_stop, &stopped.photo_playback);
    if (error == ESP_OK)
    {
        error = power_management_stop_optional(
            "内容刷新 App", content_refresh_app_stop, &stopped.content_refresh);
    }
    if (error == ESP_OK)
    {
        error = power_management_stop_optional(
            "固件 OTA Task", firmware_ota_stop, &stopped.firmware_ota);
    }
    if (error == ESP_OK)
    {
        error = power_management_stop_optional(
            "远端日志 Task", power_management_stop_remote_log, &stopped.remote_log);
    }
    if (error == ESP_OK)
    {
        error = power_management_stop_optional(
            "SD 卡 Service", sd_card_service_stop, &stopped.sd_card);
    }
    if (error == ESP_OK)
    {
        network_manager_status_t status;
        const esp_err_t status_error = network_manager_get_status_copy(&status);
        if (status_error == ESP_ERR_INVALID_STATE && allow_missing_components)
        {
            ESP_LOGI(TAG, "网络管理器尚未初始化，启动失败收敛无需关闭网络");
        }
        else if (status_error != ESP_OK)
        {
            ESP_LOGE(TAG, "读取网络停机状态失败: %s", esp_err_to_name(status_error));
            error = status_error;
        }
        else if (status.state != NETWORK_STATE_STOPPED)
        {
            error = power_management_stop_optional(
                "网络会话", network_manager_stop, &stopped.network);
        }
    }
    stop_failed = error != ESP_OK;
    if (error == ESP_OK)
    {
        bool led_on = false;
        if (device_led_is_on(&led_on) == ESP_OK && led_on)
        {
            ESP_LOGI(TAG, "开始关闭状态 LED");
            const esp_err_t led_error = device_led_off();
            if (led_error == ESP_OK)
            {
                stopped.led_turned_off = true;
                ESP_LOGI(TAG, "状态 LED 已关闭");
            }
            else
            {
                ESP_LOGW(TAG, "休眠前关闭 LED 失败，继续准备深睡: %s",
                         esp_err_to_name(led_error));
            }
        }
        if (absolute_wakeup_at_utc > 0)
        {
            timer_wakeup_seconds =
                power_management_resolve_wakeup_seconds(absolute_wakeup_at_utc);
            ESP_LOGI(TAG,
                     "配置唤醒源前重新换算绝对目标: wakeup_at=%lld, interval=%lu 秒",
                     (long long) absolute_wakeup_at_utc,
                     (unsigned long) timer_wakeup_seconds);
        }
        const uint64_t timer_wakeup_us =
            static_cast<uint64_t>(timer_wakeup_seconds) * 1000000ULL;
        ESP_LOGI(TAG,
                 "开始配置深睡唤醒源: 左/右/确认按键%s",
                 timer_wakeup_seconds > 0U ? " + 内部定时器" : "");
        error = device_power_prepare_deep_sleep(timer_wakeup_us);
        stopped.wakeup_prepared = error == ESP_OK;
        if (error == ESP_OK)
        {
            ESP_LOGI(TAG, "深睡唤醒源配置完成");
        }
    }
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "开始让墨水屏进入低功耗睡眠");
        const esp_err_t display_sleep_error = device_display_sleep();
        if (display_sleep_error == ESP_ERR_INVALID_STATE && allow_missing_components)
        {
            ESP_LOGI(TAG, "显示设备尚未初始化或已经休眠，启动失败收敛无需重复休眠");
        }
        else
        {
            error = display_sleep_error;
        }
        if (display_sleep_error == ESP_OK)
        {
            display_slept = true;
            ESP_LOGI(TAG, "墨水屏已进入低功耗睡眠");
        }
    }
    if (error == ESP_OK)
    {
        if (display_slept)
        {
            const esp_err_t display_deinit_error = device_display_deinit();
            if (display_deinit_error != ESP_OK)
            {
                ESP_LOGW(TAG,
                         "深睡前释放显示资源失败，继续进入整机深睡: %s",
                         esp_err_to_name(display_deinit_error));
            }
        }
        const esp_err_t sd_deinit_error = device_sd_deinit();
        if (sd_deinit_error != ESP_OK && sd_deinit_error != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG,
                     "深睡前关闭 SD 卡槽供电失败，继续进入整机深睡: %s",
                     esp_err_to_name(sd_deinit_error));
        }
        else if (sd_deinit_error == ESP_OK)
        {
            ESP_LOGI(TAG, "SD 卡槽与存储设备已关闭");
        }
        power_management_log_absolute_wakeup(timer_wakeup_seconds);
        if (timer_wakeup_seconds > 0U)
        {
            ESP_LOGI(TAG,
                     "运行期组件已停止；任意按键可唤醒，内部定时器将在进入深睡后计时 %lu 秒",
                     (unsigned long) timer_wakeup_seconds);
        }
        else
        {
            ESP_LOGI(TAG, "运行期组件已停止，全部按键释放后进入手动深睡");
        }
        power_management_set_retained_wakeup_target(absolute_wakeup_at_utc);
        device_power_start_deep_sleep();
    }

    const esp_err_t rollback_error = power_management_rollback(&stopped);
    *out_cleanup_failed = stop_failed || rollback_error != ESP_OK;
    if (rollback_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "深睡准备失败且运行期恢复不完整: prepare=%s rollback=%s",
                 esp_err_to_name(error),
                 esp_err_to_name(rollback_error));
    }
    else
    {
        ESP_LOGW(TAG, "深睡准备失败，运行期组件已恢复: %s", esp_err_to_name(error));
    }
    return error;
}

/**
 * @brief 完整停机重试仍失败时，以最小动作进入失败退避深睡
 *
 * 此路径不再依赖各组件的终态确认，只最佳努力关闭蜂鸣器和 LED、重建唤醒源并保留
 * 当前绝对调度目标。若连唤醒源都无法配置，则重启并由启动流程重新收敛，避免无限保持唤醒。
 *
 * @param[in] wakeup_seconds 失败退避唤醒间隔，必须大于 0
 * @param[in] wakeup_at_utc 绝对唤醒目标；0 表示相对退避
 * @param[in] reason 触发保底深睡的原始错误
 */
[[noreturn]] static void power_management_enter_forced_failure_sleep(uint32_t wakeup_seconds,
                                                                     int64_t wakeup_at_utc,
                                                                     esp_err_t reason)
{
    if (wakeup_at_utc > 0)
    {
        wakeup_seconds = power_management_resolve_wakeup_seconds(wakeup_at_utc);
    }
    ESP_LOGE(TAG,
             "完整停机重试仍失败，进入保底深睡: reason=%s interval=%lu 秒 target=%lld",
             esp_err_to_name(reason),
             (unsigned long) wakeup_seconds,
             (long long) wakeup_at_utc);
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_buzzer_stop());
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_led_off());
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_power_cancel_deep_sleep());

    const uint64_t wakeup_us = static_cast<uint64_t>(wakeup_seconds) * 1000000ULL;
    const esp_err_t prepare_error = device_power_prepare_deep_sleep(wakeup_us);
    if (prepare_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "保底深睡无法配置唤醒源，立即重启重新收敛: %s",
                 esp_err_to_name(prepare_error));
        esp_restart();
    }

    power_management_set_retained_wakeup_target(wakeup_at_utc);
    ESP_LOGW(TAG,
             "保底深睡唤醒源已配置；任意按键可唤醒，内部定时器将在 %lu 秒后唤醒",
             (unsigned long) wakeup_seconds);
    device_power_start_deep_sleep();
}

esp_err_t power_management_app_prepare_startup_sleep(
    power_management_app_startup_sleep_policy_t policy, esp_err_t reason)
{
    PowerSleepSchedule schedule = {};
    if (policy == POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF)
    {
        schedule = power_management_update_retained_schedule(reason);
        power_management_log_wakeup_schedule(
            schedule.wakeup_seconds, reason, schedule.wakeup_at_utc);
    }
    ESP_LOGW(TAG,
             "启动阶段未能继续运行，开始收敛到%s深睡: reason=%s",
             schedule.wakeup_seconds > 0U ? "定时重试" : "仅按键唤醒",
             esp_err_to_name(reason));
    bool cleanup_failed = false;
    return power_management_prepare_and_sleep(
        schedule.wakeup_seconds, schedule.wakeup_at_utc, true, &cleanup_failed);
}

/** @brief 播放一次短促 OTA 检查提示音，失败只记录诊断 */
static void power_management_play_ota_tone()
{
    const esp_err_t start_error = device_buzzer_start_tone(
        POWER_MANAGEMENT_OTA_TONE_FREQUENCY_HZ,
        POWER_MANAGEMENT_OTA_TONE_DUTY_PERCENT);
    if (start_error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动固件检查提示音失败: %s", esp_err_to_name(start_error));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(POWER_MANAGEMENT_OTA_TONE_DURATION_MS));
    const esp_err_t stop_error = device_buzzer_stop();
    if (stop_error != ESP_OK)
    {
        ESP_LOGW(TAG, "停止固件检查提示音失败: %s", esp_err_to_name(stop_error));
    }
}

/**
 * @brief 居中计算并串行呈现一张 OTA 多行 ASCII 状态页
 *
 * 第一次状态页刷新前先持久化一次性恢复标记；后续同一交互内的页面复用该标记，避免重复写入。
 *
 * @param[in] texts 文本数组
 * @param[in] scales 各行缩放倍数
 * @param[in] line_count 行数
 * @param[in,out] inout_restore_marked 是否已经设置恢复标记
 * @return ESP_OK 已完成一次物理全刷；或测量、持久化、页面显示错误码
 */
static esp_err_t power_management_present_status_page(const char *const *texts,
                                                      const uint8_t *scales,
                                                      size_t line_count,
                                                      bool *inout_restore_marked)
{
    ESP_RETURN_ON_FALSE(texts != nullptr && scales != nullptr && line_count > 0U
                            && line_count <= PHOTO_PLAYBACK_APP_STATUS_LINE_MAX
                            && inout_restore_marked != nullptr,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "状态页配置无效");
    if (!*inout_restore_marked)
    {
        ESP_RETURN_ON_ERROR(system_storage_set_display_restore_pending(true),
                            TAG,
                            "保存非照片状态页恢复标记失败");
        *inout_restore_marked = true;
    }

    device_display_info_t display_info = {};
    ESP_RETURN_ON_ERROR(device_display_get_info_copy(&display_info),
                        TAG,
                        "读取状态页显示尺寸失败");
    device_display_ascii_size_t sizes[PHOTO_PLAYBACK_APP_STATUS_LINE_MAX] = {};
    uint32_t total_height = 0U;
    for (size_t index = 0U; index < line_count; ++index)
    {
        ESP_RETURN_ON_ERROR(device_display_measure_ascii_copy(texts[index],
                                                              scales[index],
                                                              &sizes[index]),
                            TAG,
                            "测量状态页文本失败");
        ESP_RETURN_ON_FALSE(sizes[index].width_pixels <= display_info.width_pixels,
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "状态页文本宽度超过屏幕");
        total_height += sizes[index].height_pixels;
    }
    total_height += static_cast<uint32_t>(line_count - 1U)
                    * POWER_MANAGEMENT_OTA_LINE_GAP_PIXELS;
    ESP_RETURN_ON_FALSE(total_height <= display_info.height_pixels,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "状态页文本高度超过屏幕");

    photo_playback_app_status_line_t lines[PHOTO_PLAYBACK_APP_STATUS_LINE_MAX] = {};
    uint32_t y = (display_info.height_pixels - total_height) / 2U;
    for (size_t index = 0U; index < line_count; ++index)
    {
        lines[index].x_pixels = static_cast<uint16_t>(
            (display_info.width_pixels - sizes[index].width_pixels) / 2U);
        lines[index].y_pixels = static_cast<uint16_t>(y);
        lines[index].text = texts[index];
        lines[index].scale = scales[index];
        y += sizes[index].height_pixels + POWER_MANAGEMENT_OTA_LINE_GAP_PIXELS;
    }
    return photo_playback_app_present_status_page_copy(lines, line_count);
}

/** @brief 显示发现更新并等待确认的状态页 */
static esp_err_t power_management_present_update_available(bool *inout_restore_marked)
{
    static const char *const texts[] = { "UPDATE AVAILABLE", "CONFIRM: INSTALL", "LEFT: CANCEL" };
    static const uint8_t scales[] = { 5U, 4U, 4U };
    return power_management_present_status_page(texts, scales, 3U, inout_restore_marked);
}

/** @brief 显示当前固件已是最新版本的结果页 */
static esp_err_t power_management_present_up_to_date(bool *inout_restore_marked)
{
    static const char *const texts[] = { "FIRMWARE IS UP TO DATE", "LEFT: BACK" };
    static const uint8_t scales[] = { 4U, 4U };
    return power_management_present_status_page(texts, scales, 2U, inout_restore_marked);
}

/** @brief 显示服务器不可用结果页 */
static esp_err_t power_management_present_server_unavailable(bool *inout_restore_marked)
{
    static const char *const texts[] = { "SERVER UNAVAILABLE", "LEFT: BACK" };
    static const uint8_t scales[] = { 4U, 4U };
    return power_management_present_status_page(texts, scales, 2U, inout_restore_marked);
}

/** @brief 显示网络或服务器不可用提示，并只允许中键三秒长按进入配网 */
static esp_err_t power_management_present_connectivity_prompt(
    content_refresh_app_result_t result, bool *inout_restore_marked)
{
    ESP_RETURN_ON_FALSE(result == CONTENT_REFRESH_APP_RESULT_NETWORK_UNAVAILABLE
                            || result == CONTENT_REFRESH_APP_RESULT_SERVER_UNAVAILABLE,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "连接提示结果无效");
    ESP_RETURN_ON_ERROR(photo_playback_app_begin_provisioning_modal(),
                        TAG,
                        "启用连接提示配网按键失败");
    const char *const texts[] = {
        result == CONTENT_REFRESH_APP_RESULT_NETWORK_UNAVAILABLE ? "NO NETWORK" : "NO SERVER",
        "HOLD MIDDLE 3S TO SETUP",
    };
    static const uint8_t scales[] = { 4U, 2U };
    const esp_err_t error =
        power_management_present_status_page(texts, scales, 2U, inout_restore_marked);
    if (error != ESP_OK)
    {
        (void) photo_playback_app_end_modal();
    }
    return error;
}

/** @brief 显示固件正在写入且不可断电的状态页 */
static esp_err_t power_management_present_updating(bool *inout_restore_marked)
{
    static const char *const texts[] = { "UPDATING FIRMWARE", "DO NOT POWER OFF" };
    static const uint8_t scales[] = { 4U, 4U };
    return power_management_present_status_page(texts, scales, 2U, inout_restore_marked);
}

/** @brief 显示固件安装失败结果页 */
static esp_err_t power_management_present_update_failed(bool *inout_restore_marked)
{
    static const char *const texts[] = { "UPDATE FAILED", "LEFT: BACK" };
    static const uint8_t scales[] = { 5U, 4U };
    return power_management_present_status_page(texts, scales, 2U, inout_restore_marked);
}

/** @brief 在 OTA 会话中启动 Network Manager 并等待最多 30 秒上线 */
static esp_err_t power_management_wait_ota_network_online()
{
    while (xSemaphoreTake(g_power_management_runtime.network_changed, 0U) == pdTRUE) {}
    ESP_RETURN_ON_ERROR(network_manager_set_notify_callback_borrow(
                            power_management_app_on_network_change, nullptr),
                        TAG,
                        "注册 OTA 网络状态回调失败");
    network_manager_status_t status = {};
    ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status),
                        TAG,
                        "读取 OTA 网络状态失败");
    if (status.state == NETWORK_STATE_STOPPED)
    {
        ESP_RETURN_ON_ERROR(network_manager_start(), TAG, "启动 OTA 网络会话失败");
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(POWER_MANAGEMENT_OTA_NETWORK_WAIT_MS);
    while (true)
    {
        ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status),
                            TAG,
                            "等待 OTA 网络上线时读取状态失败");
        if (status.state == NETWORK_STATE_ONLINE)
        {
            return ESP_OK;
        }
        if (status.state == NETWORK_STATE_ERROR)
        {
            return status.last_error != ESP_OK ? status.last_error : ESP_FAIL;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }
        (void) xSemaphoreTake(g_power_management_runtime.network_changed,
                              timeout_ticks - elapsed);
    }
}

/** @brief 清除 OTA 网络回调并完整停止 Network Manager 会话 */
static esp_err_t power_management_stop_ota_network()
{
    esp_err_t first_error = network_manager_set_notify_callback_borrow(nullptr, nullptr);
    network_manager_status_t status = {};
    const esp_err_t status_error = network_manager_get_status_copy(&status);
    if (first_error == ESP_OK && status_error != ESP_OK)
    {
        first_error = status_error;
    }
    if (status_error == ESP_OK && status.state != NETWORK_STATE_STOPPED)
    {
        const esp_err_t stop_error = network_manager_stop();
        if (first_error == ESP_OK)
        {
            first_error = stop_error;
        }
    }
    return first_error;
}

/**
 * @brief 判断内容、网络与正常照片页面是否已经收敛到可开始 OTA 的稳定点
 *
 * @param[in] automatic_sleep 是否存在内容刷新自动流程
 * @param[in] round_ready 最近内容轮次是否已经完成网络清理
 * @param[in] target_generation 最近内容轮次对应集合代数
 * @param[out] out_error 阻止 OTA 的终态错误
 * @return Ready 可开始；Waiting 继续排队；Blocked 进入 AUTO_SLEEP_BLOCKED
 */
static PowerDisplayReadiness power_management_check_ota_start_ready(
    bool automatic_sleep, bool round_ready, uint64_t target_generation, esp_err_t *out_error)
{
    if (automatic_sleep && !round_ready)
    {
        return PowerDisplayReadiness::Waiting;
    }
    const PowerDisplayReadiness display = power_management_check_display(
        target_generation, out_error);
    if (display != PowerDisplayReadiness::Ready)
    {
        return display;
    }

    content_refresh_app_status_t content_status = {};
    const esp_err_t content_error = content_refresh_app_get_status_copy(&content_status);
    if (content_error == ESP_OK
        && content_status.state != CONTENT_REFRESH_APP_STATE_IDLE
        && content_status.state != CONTENT_REFRESH_APP_STATE_BACKOFF
        && content_status.state != CONTENT_REFRESH_APP_STATE_STOPPED)
    {
        if (content_status.state == CONTENT_REFRESH_APP_STATE_CLEANUP_FAILED)
        {
            *out_error = content_status.last_error != ESP_OK ? content_status.last_error
                                                              : ESP_FAIL;
            return PowerDisplayReadiness::Blocked;
        }
        return PowerDisplayReadiness::Waiting;
    }
    if (content_error != ESP_OK && content_error != ESP_ERR_INVALID_STATE)
    {
        *out_error = content_error;
        return PowerDisplayReadiness::Blocked;
    }

    network_manager_status_t network_status = {};
    const esp_err_t network_error = network_manager_get_status_copy(&network_status);
    if (network_error != ESP_OK)
    {
        *out_error = network_error;
        return PowerDisplayReadiness::Blocked;
    }
    return network_status.state == NETWORK_STATE_STOPPED ? PowerDisplayReadiness::Ready
                                                          : PowerDisplayReadiness::Waiting;
}

/** @brief 停止空闲内容刷新 Task，使 OTA 独占后续 Network Manager 会话 */
static esp_err_t power_management_suspend_content_for_ota(bool *inout_content_suspended)
{
    content_refresh_app_status_t status = {};
    const esp_err_t status_error = content_refresh_app_get_status_copy(&status);
    if (status_error == ESP_ERR_INVALID_STATE || status.state == CONTENT_REFRESH_APP_STATE_STOPPED)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(status_error, TAG, "读取待暂停内容刷新状态失败");
    ESP_RETURN_ON_FALSE(status.state == CONTENT_REFRESH_APP_STATE_IDLE
                            || status.state == CONTENT_REFRESH_APP_STATE_BACKOFF,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新尚未收敛，不能开始 OTA");
    ESP_RETURN_ON_ERROR(content_refresh_app_stop(), TAG, "暂停内容刷新 App 失败");
    *inout_content_suspended = true;
    return ESP_OK;
}

/**
 * @brief 关闭 OTA 网络并呈现服务器不可用结果页
 *
 * @return ESP_OK 页面与网络均已稳定；或首个网络清理、页面显示错误
 */
static esp_err_t power_management_present_ota_server_unavailable(bool *inout_network_held,
                                                                 bool *inout_restore_marked)
{
    const esp_err_t cleanup_error = power_management_stop_ota_network();
    *inout_network_held = cleanup_error != ESP_OK;
    const esp_err_t page_error = power_management_present_server_unavailable(inout_restore_marked);
    return cleanup_error != ESP_OK ? cleanup_error : page_error;
}

/**
 * @brief 准备在线会话并异步提交固件检查
 *
 * @param[in,out] inout_content_suspended 内容刷新是否已因 OTA 停止
 * @param[in,out] inout_network_held 是否持有 OTA 在线会话
 * @param[in,out] inout_restore_marked 是否已设置页面恢复标记
 * @param[out] out_submitted true 表示等待 OTA Task 完成事件；false 表示已显示稳定失败结果页
 * @return ESP_OK 请求已提交或失败结果页已经稳定；其他值表示资源清理或显示失败
 */
static esp_err_t power_management_request_ota_check(bool *inout_content_suspended,
                                                    bool *inout_network_held,
                                                    bool *inout_restore_marked,
                                                    bool *out_submitted)
{
    *out_submitted = false;
    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_CHECKING, ESP_OK);
    power_management_play_ota_tone();
    ESP_RETURN_ON_ERROR(power_management_suspend_content_for_ota(inout_content_suspended),
                        TAG,
                        "OTA 开始前暂停内容刷新失败");

    const esp_err_t online_error = power_management_wait_ota_network_online();
    if (online_error != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA 网络未能在限定时间内上线: %s", esp_err_to_name(online_error));
        return power_management_present_ota_server_unavailable(inout_network_held,
                                                               inout_restore_marked);
    }
    *inout_network_held = true;

    const esp_err_t request_error = firmware_ota_request_check();
    if (request_error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "提交 OTA 固件检查失败，按服务器不可用处理: %s",
                 esp_err_to_name(request_error));
        return power_management_present_ota_server_unavailable(inout_network_held,
                                                               inout_restore_marked);
    }
    *out_submitted = true;
    return ESP_OK;
}

/**
 * @brief 消费 OTA 检查完成事件并呈现稳定结果页
 *
 * @param[in] event OTA Task 按值复制的完成事件
 * @param[in,out] inout_network_held 是否持有 OTA 在线会话
 * @param[in,out] inout_restore_marked 是否已设置页面恢复标记
 * @param[out] out_update_available true 表示保持 Wi-Fi 并等待确认安装
 * @return ESP_OK 已进入稳定提示页；其他值表示事件、网络清理或显示失败
 */
static esp_err_t power_management_finish_ota_check(
    const firmware_ota_event_t &event,
    bool *inout_network_held,
    bool *inout_restore_marked,
    bool *out_update_available)
{
    *out_update_available = false;
    ESP_RETURN_ON_FALSE(event.type == FIRMWARE_OTA_EVENT_CHECK_COMPLETED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "收到的不是 OTA 检查完成事件");
    const firmware_ota_state_t expected_state =
        event.result == ESP_OK && event.check_result.update_available
            ? FIRMWARE_OTA_STATE_UPDATE_AVAILABLE
            : FIRMWARE_OTA_STATE_IDLE;
    ESP_RETURN_ON_FALSE(event.state == expected_state,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "OTA 检查完成事件状态不一致");
    if (event.result != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA 固件查询失败，按服务器不可用处理: %s", esp_err_to_name(event.result));
        return power_management_present_ota_server_unavailable(inout_network_held,
                                                               inout_restore_marked);
    }
    if (!event.check_result.update_available)
    {
        const esp_err_t cleanup_error = power_management_stop_ota_network();
        *inout_network_held = cleanup_error != ESP_OK;
        const esp_err_t page_error = power_management_present_up_to_date(inout_restore_marked);
        return cleanup_error != ESP_OK ? cleanup_error : page_error;
    }

    ESP_LOGI(TAG,
             "发现可安装固件，等待用户确认: version=%s, ota_version=%llu, artifact_id=%s",
             event.check_result.target_version,
             (unsigned long long) event.check_result.target_ota_version,
             event.check_result.target_artifact_id);
    ESP_RETURN_ON_ERROR(power_management_present_update_available(inout_restore_marked),
                        TAG,
                        "显示固件更新确认页失败");
    *out_update_available = true;
    return ESP_OK;
}

/**
 * @brief 显示写入页并异步提交不可取消的安装事务
 *
 * @return ESP_OK 安装请求已提交；其他值表示显示或命令提交失败
 */
static esp_err_t power_management_request_ota_install(bool *inout_restore_marked)
{
    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_INSTALLING, ESP_OK);
    ESP_RETURN_ON_ERROR(power_management_present_updating(inout_restore_marked),
                        TAG,
                        "显示固件写入状态页失败");
    return firmware_ota_request_install();
}

/**
 * @brief 清理安装失败状态并显示必须重新检查的结果页
 *
 * 安装事务失败事件已经把 OTA 状态恢复为 IDLE；命令提交失败则仍为 UPDATE_AVAILABLE，本函数
 * 先丢弃该目标，使两条失败路径统一收敛到 IDLE。
 */
static esp_err_t power_management_present_ota_install_failed(
    esp_err_t install_error,
    bool *inout_network_held,
    bool *inout_restore_marked)
{
    firmware_ota_state_t ota_state = FIRMWARE_OTA_STATE_STOPPED;
    ESP_RETURN_ON_ERROR(firmware_ota_get_state_copy(&ota_state), TAG, "读取固件安装失败状态失败");
    if (ota_state == FIRMWARE_OTA_STATE_UPDATE_AVAILABLE)
    {
        ESP_RETURN_ON_ERROR(firmware_ota_discard_pending_update(),
                            TAG,
                            "丢弃未提交的固件安装目标失败");
    }
    else
    {
        ESP_RETURN_ON_FALSE(ota_state == FIRMWARE_OTA_STATE_IDLE,
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "固件安装失败后仍处于不可取消状态");
    }

    ESP_LOGE(TAG, "固件安装失败，必须重新检查后才能重试: %s", esp_err_to_name(install_error));
    const esp_err_t cleanup_error = power_management_stop_ota_network();
    *inout_network_held = cleanup_error != ESP_OK;
    const esp_err_t page_error = power_management_present_update_failed(inout_restore_marked);
    return cleanup_error != ESP_OK ? cleanup_error : page_error;
}

/**
 * @brief 结束 OTA 交互：丢弃目标、关网、恢复照片、清标记并恢复普通按键
 *
 * 即使前一步失败也继续最佳努力恢复其余资源，并返回首个错误，确保调用方能进入
 * AUTO_SLEEP_BLOCKED 而不会遗漏可恢复步骤。
 */
static esp_err_t power_management_finish_ota_interaction(bool *inout_network_held,
                                                         bool *inout_restore_marked)
{
    firmware_ota_state_t ota_state = FIRMWARE_OTA_STATE_STOPPED;
    ESP_RETURN_ON_ERROR(firmware_ota_get_state_copy(&ota_state),
                        TAG,
                        "退出交互前读取 OTA 状态失败");
    ESP_RETURN_ON_FALSE(ota_state != FIRMWARE_OTA_STATE_CHECKING
                            && ota_state != FIRMWARE_OTA_STATE_DOWNLOADING
                            && ota_state != FIRMWARE_OTA_STATE_AWAITING_RESTART,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "OTA 事务仍不可取消，拒绝清理网络和页面");
    esp_err_t first_error = firmware_ota_discard_pending_update();
    if (first_error == ESP_ERR_INVALID_STATE)
    {
        if (ota_state == FIRMWARE_OTA_STATE_IDLE)
        {
            first_error = ESP_OK;
        }
    }
    if (*inout_network_held)
    {
        const esp_err_t network_error = power_management_stop_ota_network();
        if (network_error == ESP_OK)
        {
            *inout_network_held = false;
        }
        if (first_error == ESP_OK)
        {
            first_error = network_error;
        }
    }

    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_RESTORING, first_error);
    const esp_err_t restore_error = *inout_restore_marked
                                        ? photo_playback_app_restore_current_page()
                                        : ESP_OK;
    if (first_error == ESP_OK)
    {
        first_error = restore_error;
    }
    if (restore_error == ESP_OK && *inout_restore_marked)
    {
        const esp_err_t marker_error = system_storage_set_display_restore_pending(false);
        if (marker_error == ESP_OK)
        {
            *inout_restore_marked = false;
        }
        if (first_error == ESP_OK)
        {
            first_error = marker_error;
        }
    }
    const esp_err_t modal_error = photo_playback_app_end_modal();
    if (first_error == ESP_OK)
    {
        first_error = modal_error;
    }
    return first_error;
}

/** @brief 使用有符号差值安全判断短期 Tick 截止时间是否已到 */
static bool power_management_deadline_reached(TickType_t now, TickType_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

/** @brief 电源管理 Task 主循环 */
static void power_management_task(void *context)
{
    (void) context;
    const bool automatic_sleep =
        g_power_management_runtime.status.automatic_sleep_enabled;
    const bool timer_wakeup_boot = g_power_management_runtime.status.timer_wakeup_boot;
    const TickType_t awake_window_ticks =
        pdMS_TO_TICKS(g_power_management_runtime.interactive_awake_ms);

    bool round_ready = false;
    bool awake_window_active = false;
    bool interactive_override = false;
    TickType_t awake_deadline = 0U;
    PowerOtaInteraction ota_interaction = PowerOtaInteraction::Inactive;
    TickType_t ota_prompt_deadline = 0U;
    bool ota_network_held = false;
    bool ota_restore_marked = false;
    bool connectivity_restore_marked = false;
    bool connectivity_prompt_active = false;
    bool content_suspended_for_ota = false;
    content_refresh_app_round_event_t round_event = {};
    uint32_t wakeup_seconds = 0U;
    int64_t wakeup_at_utc = 0;
    ESP_LOGI(TAG,
             "电源管理 Task 已启动: 自动深睡=%s, 本次启动来源=%s",
             automatic_sleep ? "启用" : "关闭",
             timer_wakeup_boot ? "内部定时器唤醒" : "按键唤醒或冷启动");
    while (true)
    {
        TickType_t wait_ticks = portMAX_DELAY;
        if (ota_interaction == PowerOtaInteraction::UpdateAvailable
            || ota_interaction == PowerOtaInteraction::ResultPage)
        {
            const TickType_t now = xTaskGetTickCount();
            wait_ticks = power_management_deadline_reached(now, ota_prompt_deadline)
                             ? 0U
                             : ota_prompt_deadline - now;
        }
        else if (awake_window_active && ota_interaction == PowerOtaInteraction::Inactive)
        {
            const TickType_t now = xTaskGetTickCount();
            wait_ticks = power_management_deadline_reached(now, awake_deadline)
                             ? 0U
                             : awake_deadline - now;
        }

        uint32_t notification = 0U;
        (void) xTaskNotifyWait(0U, UINT32_MAX, &notification, wait_ticks);
        if ((notification & POWER_MANAGEMENT_NOTIFY_STOP) != 0U)
        {
            break;
        }

        if ((notification & POWER_MANAGEMENT_NOTIFY_PROVISIONING) != 0U
            && ota_interaction == PowerOtaInteraction::Inactive)
        {
            const esp_err_t intent_error = system_storage_set_provisioning_pending(true);
            if (intent_error != ESP_OK)
            {
                ESP_LOGE(TAG,
                         "保存一次性配网意图失败，保持当前页面: %s",
                         esp_err_to_name(intent_error));
            }
            else
            {
                power_management_restart_for_provisioning();
            }
        }

        if ((notification & POWER_MANAGEMENT_NOTIFY_FIRMWARE_CHECK) != 0U
            && ota_interaction == PowerOtaInteraction::Inactive)
        {
            const esp_err_t modal_error = power_management_app_begin_ota_modal_capture();
            awake_window_active = false;
            if (modal_error != ESP_OK)
            {
                ota_interaction = PowerOtaInteraction::Blocked;
                power_management_app_publish(
                    POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED, modal_error);
                ESP_LOGE(TAG, "启用固件检查模态按键失败，禁止自动深睡: %s",
                         esp_err_to_name(modal_error));
            }
            else
            {
                ota_interaction = PowerOtaInteraction::CheckPending;
                power_management_app_publish(POWER_MANAGEMENT_APP_STATE_CHECK_PENDING, ESP_OK);
                ESP_LOGI(TAG, "已接收一次性固件检查请求，等待内容、显示和网络资源收敛");
            }
        }

        if ((notification & POWER_MANAGEMENT_NOTIFY_MANUAL_REFRESH) != 0U
            && ota_interaction == PowerOtaInteraction::Inactive)
        {
            round_ready         = false;
            awake_window_active = false;
            if (automatic_sleep)
            {
                power_management_app_publish(POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH, ESP_OK);
            }
            ESP_LOGI(TAG, "确认键已提交完整内容刷新，暂停无活动休眠窗口并等待刷新收敛");
        }

        bool sleep_now = false;
        bool retry_sleep_requested = false;
        uint32_t sleep_wakeup_seconds = 0U;
        int64_t sleep_wakeup_at_utc = 0;
        if ((notification & POWER_MANAGEMENT_NOTIFY_REFRESH_START_FAILURE) != 0U
            && ota_interaction != PowerOtaInteraction::Inactive)
        {
            esp_err_t refresh_start_error;
            taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
            refresh_start_error = g_power_management_runtime.refresh_start_error;
            g_power_management_runtime.refresh_start_error = ESP_OK;
            taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
            (void) photo_playback_app_end_modal();
            ota_interaction = PowerOtaInteraction::Blocked;
            power_management_app_publish(
                POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED, refresh_start_error);
        }
        if ((notification & POWER_MANAGEMENT_NOTIFY_SLEEP) != 0U
            && ota_interaction == PowerOtaInteraction::Inactive)
        {
            sleep_now = true;
            round_ready = false;
            awake_window_active = false;
        }
        else if ((notification & POWER_MANAGEMENT_NOTIFY_REFRESH_START_FAILURE) != 0U
                 && ota_interaction == PowerOtaInteraction::Inactive)
        {
            esp_err_t refresh_start_error;
            taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
            refresh_start_error = g_power_management_runtime.refresh_start_error;
            g_power_management_runtime.refresh_start_error = ESP_OK;
            taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);

            const PowerSleepSchedule failure_schedule =
                power_management_update_retained_schedule(refresh_start_error);
            sleep_wakeup_seconds = failure_schedule.wakeup_seconds;
            sleep_wakeup_at_utc = failure_schedule.wakeup_at_utc;
            power_management_publish_schedule(
                sleep_wakeup_seconds, sleep_wakeup_at_utc, 0U);
            power_management_log_wakeup_schedule(
                sleep_wakeup_seconds, refresh_start_error, sleep_wakeup_at_utc);
            ESP_LOGW(TAG,
                     "内容刷新 Task 启动失败，立即进入退避深睡: error=%s",
                     esp_err_to_name(refresh_start_error));
            sleep_now = true;
            retry_sleep_requested = true;
            round_ready = false;
            awake_window_active = false;
        }
        else if (automatic_sleep)
        {
            if ((notification & POWER_MANAGEMENT_NOTIFY_REFRESH_ROUND) != 0U
                && power_management_take_round_event(&round_event))
            {
                ESP_LOGI(TAG,
                         "收到内容刷新轮次结果: generation=%llu, result=%u, error=%s, "
                         "network_cleanup=%s, next_refresh_at=%lld",
                         (unsigned long long) round_event.collection_generation,
                         (unsigned int) round_event.result,
                         esp_err_to_name(round_event.round_error),
                         round_event.network_cleanup_succeeded ? "完成" : "失败",
                         (long long) round_event.next_refresh_at_utc);
                awake_window_active = false;
                round_ready = round_event.network_cleanup_succeeded;
                if (!round_event.network_cleanup_succeeded)
                {
                    power_management_publish_schedule(0U,
                                                      0,
                                                      round_event.collection_generation);
                    power_management_app_publish(
                        POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                        round_event.round_error != ESP_OK ? round_event.round_error : ESP_FAIL);
                    ESP_LOGE(TAG, "本轮网络资源未完整关闭，禁止自动深睡");
                    if (ota_interaction == PowerOtaInteraction::CheckPending)
                    {
                        (void) photo_playback_app_end_modal();
                        ota_interaction = PowerOtaInteraction::Blocked;
                    }
                }
                else
                {
                    const PowerSleepSchedule failure_schedule =
                        power_management_update_retained_schedule(round_event.round_error);
                    wakeup_at_utc = round_event.round_error == ESP_OK
                                        ? round_event.next_refresh_at_utc
                                        : failure_schedule.wakeup_at_utc;
                    wakeup_seconds = round_event.round_error == ESP_OK
                                          ? power_management_resolve_wakeup_seconds(wakeup_at_utc)
                                          : failure_schedule.wakeup_seconds;
                    power_management_publish_schedule(
                        wakeup_seconds,
                        wakeup_at_utc,
                        round_event.collection_generation);
                    power_management_log_wakeup_schedule(
                        wakeup_seconds,
                        round_event.round_error,
                        wakeup_at_utc);
                    if (round_event.result == CONTENT_REFRESH_APP_RESULT_NETWORK_UNAVAILABLE
                        || round_event.result == CONTENT_REFRESH_APP_RESULT_SERVER_UNAVAILABLE)
                    {
                        const esp_err_t prompt_error = power_management_present_connectivity_prompt(
                            round_event.result, &connectivity_restore_marked);
                        if (prompt_error != ESP_OK)
                        {
                            round_ready = false;
                            power_management_app_publish(
                                POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED, prompt_error);
                            ESP_LOGE(TAG,
                                     "连接提示页未能稳定显示: %s",
                                     esp_err_to_name(prompt_error));
                        }
                        else
                        {
                            connectivity_prompt_active = true;
                        }
                    }
                }
            }

            if (round_ready && ota_interaction == PowerOtaInteraction::Inactive)
            {
                esp_err_t display_error = ESP_OK;
                const PowerDisplayReadiness readiness = connectivity_prompt_active
                                                            ? PowerDisplayReadiness::Ready
                                                            : power_management_check_display(
                                                                  round_event.collection_generation,
                                                                  &display_error);
                if (readiness == PowerDisplayReadiness::Blocked)
                {
                    awake_window_active = false;
                    power_management_app_publish(
                        POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED, display_error);
                    ESP_LOGE(TAG,
                             "目标集合显示未成功，禁止自动深睡: generation=%llu error=%s",
                             (unsigned long long) round_event.collection_generation,
                             esp_err_to_name(display_error));
                }
                else if (readiness == PowerDisplayReadiness::Waiting)
                {
                    power_management_app_publish(
                        POWER_MANAGEMENT_APP_STATE_WAIT_DISPLAY, ESP_OK);
                    ESP_LOGI(TAG,
                             "内容刷新已结束，等待目标集合显示完成: generation=%llu",
                             (unsigned long long) round_event.collection_generation);
                }
                else if (timer_wakeup_boot && !interactive_override)
                {
                    ESP_LOGI(TAG,
                             "目标集合显示状态已收敛，定时唤醒流程将立即再次进入深睡");
                    sleep_now = true;
                    sleep_wakeup_seconds = wakeup_seconds;
                    sleep_wakeup_at_utc = wakeup_at_utc;
                }
                else if (!awake_window_active)
                {
                    awake_window_active = true;
                    awake_deadline = xTaskGetTickCount() + awake_window_ticks;
                    power_management_app_publish(
                        POWER_MANAGEMENT_APP_STATE_AWAKE_WINDOW, ESP_OK);
                    ESP_LOGI(TAG,
                             "刷新与显示状态已收敛，进入 %lu 秒无活动窗口；"
                             "窗口结束后按既定计划深睡",
                             (unsigned long) (g_power_management_runtime.interactive_awake_ms
                                              / 1000U));
                }
            }

            if (awake_window_active && ota_interaction == PowerOtaInteraction::Inactive
                && (notification & POWER_MANAGEMENT_NOTIFY_USER_ACTIVITY) != 0U)
            {
                awake_deadline = xTaskGetTickCount() + awake_window_ticks;
                ESP_LOGI(TAG,
                         "检测到左右导航活动，重新计算 %lu 秒无活动窗口",
                         (unsigned long) (g_power_management_runtime.interactive_awake_ms / 1000U));
            }
            if (awake_window_active && ota_interaction == PowerOtaInteraction::Inactive
                && power_management_deadline_reached(xTaskGetTickCount(), awake_deadline))
            {
                sleep_now = true;
                sleep_wakeup_seconds = wakeup_seconds;
                sleep_wakeup_at_utc = wakeup_at_utc;
                awake_window_active = false;
            }
        }

        if (ota_interaction == PowerOtaInteraction::CheckPending)
        {
            esp_err_t                   readiness_error = ESP_OK;
            const PowerDisplayReadiness readiness = power_management_check_ota_start_ready(
                automatic_sleep,
                round_ready,
                round_event.collection_generation,
                &readiness_error);
            if (readiness == PowerDisplayReadiness::Blocked)
            {
                (void) photo_playback_app_end_modal();
                ota_interaction = PowerOtaInteraction::Blocked;
                power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                             readiness_error);
                ESP_LOGE(TAG,
                         "固件检查等待的内容或显示终态异常，禁止自动深睡: %s",
                         esp_err_to_name(readiness_error));
            }
            else if (readiness == PowerDisplayReadiness::Ready)
            {
                bool check_submitted = false;
                const esp_err_t check_error =
                    power_management_request_ota_check(&content_suspended_for_ota,
                                                       &ota_network_held,
                                                       &ota_restore_marked,
                                                       &check_submitted);
                if (check_error != ESP_OK)
                {
                    const esp_err_t finish_error =
                        power_management_finish_ota_interaction(&ota_network_held,
                                                                &ota_restore_marked);
                    ota_interaction = PowerOtaInteraction::Blocked;
                    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                                 check_error != ESP_OK ? check_error
                                                                       : finish_error);
                    ESP_LOGE(TAG,
                             "固件检查资源未安全收敛，禁止自动深睡: check=%s finish=%s",
                             esp_err_to_name(check_error),
                             esp_err_to_name(finish_error));
                }
                else if (check_submitted)
                {
                    ota_interaction = PowerOtaInteraction::Checking;
                    ESP_LOGI(TAG, "固件检查命令已提交，等待 OTA Task 完成事件");
                }
                else
                {
                    ota_interaction = PowerOtaInteraction::ResultPage;
                    ota_prompt_deadline =
                        xTaskGetTickCount() + pdMS_TO_TICKS(POWER_MANAGEMENT_OTA_PROMPT_TIMEOUT_MS);
                    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_RESULT_PAGE, ESP_OK);
                }
            }
        }

        if ((notification & POWER_MANAGEMENT_NOTIFY_FIRMWARE_OTA_EVENT) != 0U)
        {
            firmware_ota_event_t event = {};
            if (!power_management_take_firmware_ota_event(&event))
            {
                ota_interaction = PowerOtaInteraction::Blocked;
                power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                             ESP_ERR_INVALID_STATE);
                ESP_LOGE(TAG, "收到 OTA 完成通知但没有可消费的事件");
            }
            else if (ota_interaction == PowerOtaInteraction::Checking)
            {
                bool update_available = false;
                const esp_err_t check_error =
                    power_management_finish_ota_check(event,
                                                      &ota_network_held,
                                                      &ota_restore_marked,
                                                      &update_available);
                if (check_error != ESP_OK)
                {
                    const esp_err_t finish_error =
                        power_management_finish_ota_interaction(&ota_network_held,
                                                                &ota_restore_marked);
                    ota_interaction = PowerOtaInteraction::Blocked;
                    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                                 check_error != ESP_OK ? check_error
                                                                       : finish_error);
                    ESP_LOGE(TAG,
                             "固件检查完成事件未安全收敛，禁止自动深睡: check=%s finish=%s",
                             esp_err_to_name(check_error),
                             esp_err_to_name(finish_error));
                }
                else
                {
                    ota_interaction = update_available ? PowerOtaInteraction::UpdateAvailable
                                                       : PowerOtaInteraction::ResultPage;
                    ota_prompt_deadline =
                        xTaskGetTickCount() + pdMS_TO_TICKS(POWER_MANAGEMENT_OTA_PROMPT_TIMEOUT_MS);
                    power_management_app_publish(
                        update_available ? POWER_MANAGEMENT_APP_STATE_UPDATE_AVAILABLE
                                         : POWER_MANAGEMENT_APP_STATE_RESULT_PAGE,
                        ESP_OK);
                }
            }
            else if (ota_interaction == PowerOtaInteraction::Installing)
            {
                const bool valid_install_event =
                    event.type == FIRMWARE_OTA_EVENT_INSTALL_COMPLETED
                    && ((event.result == ESP_OK
                         && event.state == FIRMWARE_OTA_STATE_AWAITING_RESTART)
                        || (event.result != ESP_OK && event.state == FIRMWARE_OTA_STATE_IDLE));
                if (!valid_install_event)
                {
                    ota_interaction = PowerOtaInteraction::Blocked;
                    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_INSTALLING,
                                                 ESP_ERR_INVALID_STATE);
                    ESP_LOGE(TAG, "固件安装完成事件状态不一致，保持唤醒并等待人工恢复");
                }
                else if (event.result != ESP_OK)
                {
                    const esp_err_t failure_error =
                        power_management_present_ota_install_failed(event.result,
                                                                    &ota_network_held,
                                                                    &ota_restore_marked);
                    if (failure_error == ESP_OK)
                    {
                        ota_interaction = PowerOtaInteraction::ResultPage;
                        ota_prompt_deadline =
                            xTaskGetTickCount()
                            + pdMS_TO_TICKS(POWER_MANAGEMENT_OTA_PROMPT_TIMEOUT_MS);
                        power_management_app_publish(POWER_MANAGEMENT_APP_STATE_RESULT_PAGE,
                                                     event.result);
                    }
                    else
                    {
                        const esp_err_t finish_error =
                            power_management_finish_ota_interaction(&ota_network_held,
                                                                    &ota_restore_marked);
                        ota_interaction = PowerOtaInteraction::Blocked;
                        power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                                     failure_error != ESP_OK ? failure_error
                                                                             : finish_error);
                    }
                }
            }
            else
            {
                ota_interaction = PowerOtaInteraction::Blocked;
                power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                             ESP_ERR_INVALID_STATE);
                ESP_LOGE(TAG, "当前交互阶段不接受 OTA 完成事件");
            }
        }

        const bool ota_prompt_active = ota_interaction == PowerOtaInteraction::UpdateAvailable
                                       || ota_interaction == PowerOtaInteraction::ResultPage;
        const bool ota_prompt_timed_out =
            ota_prompt_active
            && power_management_deadline_reached(xTaskGetTickCount(), ota_prompt_deadline);
        if (ota_interaction == PowerOtaInteraction::UpdateAvailable
            && (notification & POWER_MANAGEMENT_NOTIFY_MODAL_CONFIRM) != 0U
            && (notification & POWER_MANAGEMENT_NOTIFY_MODAL_LEFT) == 0U && !ota_prompt_timed_out)
        {
            const esp_err_t request_error =
                power_management_request_ota_install(&ota_restore_marked);
            if (request_error == ESP_OK)
            {
                ota_interaction = PowerOtaInteraction::Installing;
                ESP_LOGI(TAG, "固件安装命令已提交，等待 OTA Task 完成事件");
            }
            else
            {
                const esp_err_t failure_error =
                    power_management_present_ota_install_failed(request_error,
                                                                &ota_network_held,
                                                                &ota_restore_marked);
                if (failure_error == ESP_OK)
                {
                    ota_interaction = PowerOtaInteraction::ResultPage;
                    ota_prompt_deadline =
                        xTaskGetTickCount() + pdMS_TO_TICKS(POWER_MANAGEMENT_OTA_PROMPT_TIMEOUT_MS);
                    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_RESULT_PAGE,
                                                 request_error);
                }
                else
                {
                    const esp_err_t finish_error =
                        power_management_finish_ota_interaction(&ota_network_held,
                                                                &ota_restore_marked);
                    ota_interaction = PowerOtaInteraction::Blocked;
                    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                                                 failure_error != ESP_OK ? failure_error
                                                                         : finish_error);
                }
            }
        }
        else if (ota_prompt_active
                 && (((notification & POWER_MANAGEMENT_NOTIFY_MODAL_LEFT) != 0U)
                     || ota_prompt_timed_out))
        {
            const bool manual_exit = !ota_prompt_timed_out;
            const esp_err_t finish_error = power_management_finish_ota_interaction(
                &ota_network_held, &ota_restore_marked);
            if (finish_error != ESP_OK)
            {
                ota_interaction = PowerOtaInteraction::Blocked;
                power_management_app_publish(
                    POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED, finish_error);
                ESP_LOGE(TAG, "退出固件交互时资源恢复失败，禁止自动深睡: %s",
                         esp_err_to_name(finish_error));
            }
            else
            {
                ota_interaction = PowerOtaInteraction::Inactive;
                if (manual_exit)
                {
                    interactive_override = true;
                    awake_window_active = true;
                    awake_deadline = xTaskGetTickCount() + awake_window_ticks;
                    power_management_app_publish(
                        POWER_MANAGEMENT_APP_STATE_AWAKE_WINDOW, ESP_OK);
                    ESP_LOGI(TAG, "已退出固件交互并恢复照片，重新开始正常无活动窗口");
                }
                else
                {
                    sleep_now = true;
                    sleep_wakeup_seconds = wakeup_seconds;
                    sleep_wakeup_at_utc = wakeup_at_utc;
                    ESP_LOGI(TAG, "固件提示页等待超时，恢复照片后按既定计划直接深睡");
                }
            }
        }

        esp_err_t failure_sleep_error = ESP_OK;
        if (power_management_get_failure_sleep_error(&failure_sleep_error))
        {
            const PowerSleepSchedule failure_schedule =
                power_management_update_retained_schedule(failure_sleep_error);
            sleep_wakeup_seconds = failure_schedule.wakeup_seconds;
            sleep_wakeup_at_utc = failure_schedule.wakeup_at_utc;
            power_management_publish_schedule(
                sleep_wakeup_seconds, sleep_wakeup_at_utc, 0U);
            power_management_log_wakeup_schedule(
                sleep_wakeup_seconds, failure_sleep_error, sleep_wakeup_at_utc);
            retry_sleep_requested = true;
            sleep_now = true;
            round_ready = false;
            awake_window_active = false;
            ESP_LOGW(TAG,
                     "休眠阻塞状态已收敛为失败退避深睡，不再无限保持唤醒: error=%s",
                     esp_err_to_name(failure_sleep_error));
        }

        if (sleep_now)
        {
            if (sleep_wakeup_at_utc > 0)
            {
                sleep_wakeup_seconds =
                    power_management_resolve_wakeup_seconds(sleep_wakeup_at_utc);
                power_management_publish_schedule(
                    sleep_wakeup_seconds,
                    sleep_wakeup_at_utc,
                    round_event.collection_generation);
                ESP_LOGI(TAG,
                         "实际休眠前已重新换算绝对唤醒目标: "
                         "wakeup_at=%lld, interval=%lu 秒",
                         (long long) sleep_wakeup_at_utc,
                         (unsigned long) sleep_wakeup_seconds);
            }
            if (sleep_wakeup_seconds > 0U)
            {
                ESP_LOGI(TAG,
                         "开始自动休眠；内部定时器将在实际进入深睡后计时 %lu 秒",
                         (unsigned long) sleep_wakeup_seconds);
            }
            else
            {
                ESP_LOGI(TAG, "开始手动休眠，本次仅保留按键唤醒");
            }
            power_management_app_publish(POWER_MANAGEMENT_APP_STATE_PREPARING_SLEEP, ESP_OK);
            bool cleanup_failed = false;
            esp_err_t error = power_management_prepare_and_sleep(
                sleep_wakeup_seconds, sleep_wakeup_at_utc, false, &cleanup_failed);
            if (error != ESP_OK && !retry_sleep_requested)
            {
                const PowerSleepSchedule failure_schedule =
                    power_management_update_retained_schedule(error);
                sleep_wakeup_seconds = failure_schedule.wakeup_seconds;
                sleep_wakeup_at_utc = failure_schedule.wakeup_at_utc;
                power_management_publish_schedule(
                    sleep_wakeup_seconds, sleep_wakeup_at_utc, 0U);
                power_management_log_wakeup_schedule(
                    sleep_wakeup_seconds, error, sleep_wakeup_at_utc);
                retry_sleep_requested = true;
                cleanup_failed = false;
                power_management_app_publish(
                    POWER_MANAGEMENT_APP_STATE_RETRY_SLEEP_REQUESTED, error);
                ESP_LOGW(TAG,
                         "首次深睡准备失败，按失败退避计划再执行一次完整停机: %s",
                         esp_err_to_name(error));
                error = power_management_prepare_and_sleep(
                    sleep_wakeup_seconds,
                    sleep_wakeup_at_utc,
                    false,
                    &cleanup_failed);
            }

            power_management_app_publish(
                cleanup_failed ? POWER_MANAGEMENT_APP_STATE_CLEANUP_FAILED
                               : POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED,
                error != ESP_OK ? error : ESP_FAIL);
            const esp_err_t forced_reason =
                failure_sleep_error != ESP_OK ? failure_sleep_error : error;
            power_management_enter_forced_failure_sleep(
                sleep_wakeup_seconds,
                sleep_wakeup_at_utc,
                forced_reason != ESP_OK ? forced_reason : ESP_FAIL);
        }
    }

    power_management_app_publish(POWER_MANAGEMENT_APP_STATE_STOPPED, ESP_OK);
    (void) xSemaphoreGive(g_power_management_runtime.task_stopped);
    vTaskSuspend(nullptr);
}

esp_err_t power_management_app_task_start(void)
{
    g_power_management_runtime.task = nullptr;
    ESP_RETURN_ON_FALSE(xTaskCreate(power_management_task,
                                    "power_management",
                                    POWER_MANAGEMENT_TASK_STACK_SIZE,
                                    nullptr,
                                    POWER_MANAGEMENT_TASK_PRIORITY,
                                    &g_power_management_runtime.task)
                            == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建电源管理 Task 失败");
    power_management_app_publish(
        g_power_management_runtime.status.automatic_sleep_enabled
            ? POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH
            : POWER_MANAGEMENT_APP_STATE_IDLE,
        ESP_OK);
    return ESP_OK;
}

esp_err_t power_management_app_task_stop(void)
{
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_INVALID_STATE, TAG, "电源管理 Task 尚未运行");
    (void) xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_STOP, eSetBits);
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_power_management_runtime.task_stopped,
                                       pdMS_TO_TICKS(POWER_MANAGEMENT_STOP_TIMEOUT_MS))
                            == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "等待电源管理 Task 停止超时");
    vTaskDelete(task);
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    g_power_management_runtime.task = nullptr;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    return ESP_OK;
}
