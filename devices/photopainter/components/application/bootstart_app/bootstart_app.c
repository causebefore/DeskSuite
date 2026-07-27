/**
 * @file bootstart_app.c
 * @brief PhotoPainter 设备端启动用例编排
 */
#include "bootstart_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "button_service.h"
#include "content_refresh_app.h"
#include "device_battery.h"
#include "device_buzzer.h"
#include "device_button.h"
#include "device_display.h"
#include "device_environment.h"
#include "device_led.h"
#include "device_power.h"
#include "device_rtc.h"
#include "device_sd.h"
#include "display_collection_service.h"
#include "display_present_service.h"
#include "environment_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "firmware_ota_build.h"
#include "firmware_ota.h"
#include "network_manager.h"
#include "photo_playback_app.h"
#include "power_management_app.h"
#include "protocol_backend_context.h"
#include "provisioning_app.h"
#include "remote_log.h"
#include "sd_card_service.h"
#include "system_clock.h"
#include "system_storage.h"

/** @brief 日志标签 */
static const char *TAG = "bootstart_app";

/** @brief 内容刷新单次 HTTP 超时 */
#define BOOTSTART_APP_CONTENT_REFRESH_TIMEOUT_MS       180000
/** @brief OTA 检查接口超时 */
#define BOOTSTART_APP_FIRMWARE_OTA_CHECK_TIMEOUT_MS    10000
/** @brief OTA 固件下载超时 */
#define BOOTSTART_APP_FIRMWARE_OTA_DOWNLOAD_TIMEOUT_MS 180000
/** @brief 按键唤醒或冷启动后的无活动保持时长 */
#define BOOTSTART_APP_INTERACTIVE_AWAKE_MS             180000U
/** @brief 星期日志名称，索引与 RTC 的 0=星期日 约定一致 */
static const char *s_bootstart_app_weekday_names[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
};

/**
 * @brief 从已生效网络配置构造本机完整后端上下文
 *
 * 设备 ID 统一由共享 protocols 组件基于 Wi-Fi Station 基础 MAC 生成；本函数不保存上下文。
 *
 * @param[out] out_context 后端上下文副本
 * @return ESP_OK 已构造；ESP_ERR_NOT_FOUND 服务地址为空；或存储、配置、硬件身份错误码
 */
static esp_err_t bootstart_app_load_backend_context_copy(protocol_backend_context_t *out_context)
{
    ESP_RETURN_ON_FALSE(out_context != NULL, ESP_ERR_INVALID_ARG, TAG, "后端上下文输出为空");
    system_storage_network_config_t network_config;
    ESP_RETURN_ON_ERROR(system_storage_get_network_config_copy(&network_config),
                        TAG,
                        "读取后端服务配置失败");
    ESP_RETURN_ON_FALSE(network_config.service_url[0] != '\0',
                        ESP_ERR_NOT_FOUND,
                        TAG,
                        "后端服务地址为空");
    const protocol_backend_context_config_t config = {
        .base_url        = network_config.service_url,
        .token           = network_config.device_token,
        .device_id       = NULL,
        .product_id      = DESKSUITE_PRODUCT_ID,
        .firmware_target = DESKSUITE_FIRMWARE_TARGET,
    };
    return protocol_backend_context_build_copy(&config, out_context);
}

/**
 * @brief 把持久化网络配置转换为 Network Manager 配置
 *
 * @param[out] out_config Network Manager 配置输出
 * @param[in] context 未使用的提供者上下文
 * @return ESP_OK 已加载；或 system_storage 原始错误码
 */
static esp_err_t bootstart_app_load_network_config_copy(network_manager_config_t *out_config,
                                                        void                     *context)
{
    (void) context;
    if (out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    system_storage_network_config_t stored;
    const esp_err_t                 error = system_storage_get_network_config_copy(&stored);
    if (error != ESP_OK)
    {
        return error;
    }

    memset(out_config, 0, sizeof(*out_config));
    (void) snprintf(out_config->ssid, sizeof(out_config->ssid), "%s", stored.ssid);
    (void) snprintf(out_config->password, sizeof(out_config->password), "%s", stored.password);
    (void) snprintf(out_config->service_url,
                    sizeof(out_config->service_url),
                    "%s",
                    stored.service_url);
    (void) snprintf(out_config->device_token,
                    sizeof(out_config->device_token),
                    "%s",
                    stored.device_token);
    return ESP_OK;
}

/**
 * @brief 把 Network Manager 配置转换并提交到 system_storage
 *
 * @param[in] config 调用期间借用的 Network Manager 配置
 * @param[in] context 未使用的提供者上下文
 * @return ESP_OK 已保存；或 system_storage 原始错误码
 */
static esp_err_t bootstart_app_save_network_config_borrow(const network_manager_config_t *config,
                                                          void                           *context)
{
    (void) context;
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    system_storage_network_config_t stored = {
        .version = SYSTEM_STORAGE_NETWORK_CONFIG_VERSION,
    };
    (void) snprintf(stored.ssid, sizeof(stored.ssid), "%s", config->ssid);
    (void) snprintf(stored.password, sizeof(stored.password), "%s", config->password);
    (void) snprintf(stored.service_url, sizeof(stored.service_url), "%s", config->service_url);
    (void) snprintf(stored.device_token, sizeof(stored.device_token), "%s", config->device_token);
    return system_storage_set_network_config_borrow(&stored);
}

/**
 * @brief 清除 system_storage 中的网络配置
 *
 * @param[in] context 未使用的提供者上下文
 * @return ESP_OK 已清除；ESP_ERR_NOT_FOUND 原本不存在；或 system_storage 原始错误码
 */
static esp_err_t bootstart_app_erase_network_config(void *context)
{
    (void) context;
    return system_storage_erase_network_config();
}

/** @brief 由启动 Application 长期提供的 Network Manager 配置持久化回调 */
static const network_manager_config_store_t s_bootstart_app_network_config_store = {
    .load_config_copy   = bootstart_app_load_network_config_copy,
    .save_config_borrow = bootstart_app_save_network_config_borrow,
    .erase_config       = bootstart_app_erase_network_config,
    .ctx                = NULL,
};

/** @brief 在正常页面恢复后清除一次性 OTA 提示画面标记 */
static esp_err_t bootstart_app_confirm_ota_display_restored(void *context)
{
    (void) context;
    const esp_err_t error = system_storage_set_ota_display_restore_pending(false);
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "正常页面已覆盖 OTA 更新提示，清除一次性画面恢复标记");
    }
    return error;
}

/** @brief 读取 OTA 提示画面待恢复状态，读取异常时保守地要求恢复正常页面 */
static bool bootstart_app_should_restore_ota_display(void)
{
    bool            pending = false;
    const esp_err_t error   = system_storage_get_ota_display_restore_pending(&pending);
    if (error == ESP_ERR_NOT_FOUND)
    {
        return false;
    }
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "读取 OTA 提示画面恢复标记失败，本轮保守执行全屏刷新: %s",
                 esp_err_to_name(error));
        return true;
    }
    return pending;
}

/** @brief 启动阶段致命故障发生在待验证镜像上时立即触发回滚 */
void bootstart_app_reject_pending_image_on_fatal_error(esp_err_t startup_error)
{
    if (startup_error == ESP_OK)
    {
        return;
    }
    const esp_err_t rollback_error = firmware_ota_reject_running_image_and_reboot();
    if (rollback_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "启动失败且无法处理待验证镜像: startup=%s, rollback=%s",
                 esp_err_to_name(startup_error),
                 esp_err_to_name(rollback_error));
    }
}

esp_err_t bootstart_app_get_wakeup_context_copy(bootstart_app_wakeup_context_t *out_context)
{
    ESP_RETURN_ON_FALSE(out_context != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "启动唤醒上下文输出指针为空");
    bootstart_app_wakeup_context_t context = { 0 };
    context.read_error = device_power_was_woken_by_button(&context.woken_by_button);
    if (context.read_error == ESP_OK)
    {
        context.read_error = device_power_was_woken_by_timer(&context.woken_by_timer);
    }
    *out_context = context;
    return ESP_OK;
}

/** @brief 在系统时间校准后记录本轮深睡唤醒来源 */
static void bootstart_app_log_wakeup_context(const bootstart_app_wakeup_context_t *context)
{
    if (context->read_error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取深睡唤醒来源失败: %s", esp_err_to_name(context->read_error));
    }
    else if (context->woken_by_button)
    {
        ESP_LOGI(TAG, "设备由按键从深睡唤醒");
    }
    else if (context->woken_by_timer)
    {
        ESP_LOGI(TAG, "设备由内部定时器从深睡唤醒");
    }
    else
    {
        ESP_LOGI(TAG, "设备由冷启动或非深睡复位启动");
    }
}

esp_err_t bootstart_app_enforce_absolute_wakeup_gate(const bootstart_app_wakeup_context_t *context)
{
    ESP_RETURN_ON_FALSE(context != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "启动绝对时间门禁缺少唤醒上下文");
    bootstart_app_log_wakeup_context(context);
    return power_management_app_enforce_absolute_wakeup_gate(context->woken_by_button,
                                                             context->woken_by_timer);
}

/**
 * @brief 把编译器提供的日期时间转换为 RTC 日历时间
 *
 * @param[out] out_datetime 转换结果
 * @return true 成功；false 编译日期无法解析
 */
static bool bootstart_app_get_compile_datetime(device_rtc_datetime_t *out_datetime)
{
    if (out_datetime == NULL)
    {
        return false;
    }
    static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    uint8_t           month         = 0U;
    for (uint8_t index = 0U; index < 12U; ++index)
    {
        if (strncmp(__DATE__, &month_names[index * 3U], 3U) == 0)
        {
            month = (uint8_t) (index + 1U);
            break;
        }
    }
    if (month == 0U)
    {
        return false;
    }
    out_datetime->year  = (uint16_t) ((__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100
                                      + (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0'));
    out_datetime->month = month;
    out_datetime->day =
        (uint8_t) ((__DATE__[4] == ' ' ? 0 : (__DATE__[4] - '0') * 10) + (__DATE__[5] - '0'));
    out_datetime->hour   = (uint8_t) ((__TIME__[0] - '0') * 10 + (__TIME__[1] - '0'));
    out_datetime->minute = (uint8_t) ((__TIME__[3] - '0') * 10 + (__TIME__[4] - '0'));
    out_datetime->second = (uint8_t) ((__TIME__[6] - '0') * 10 + (__TIME__[7] - '0'));
    return true;
}

/**
 * @brief 初始化 RTC，在必要时写入编译时间，并校准系统墙上时钟
 *
 * @return ESP_OK RTC 已可用；或 RTC 初始化、读取、写入错误码
 */
esp_err_t bootstart_app_init_rtc(void)
{
    ESP_RETURN_ON_ERROR(device_rtc_init(), TAG, "RTC 初始化失败");

    bool voltage_low = false;
    ESP_RETURN_ON_ERROR(device_rtc_get_voltage_low(&voltage_low), TAG, "读取 RTC 电压状态失败");
    if (voltage_low)
    {
        device_rtc_datetime_t compile_datetime;
        ESP_RETURN_ON_FALSE(bootstart_app_get_compile_datetime(&compile_datetime),
                            ESP_FAIL,
                            TAG,
                            "解析固件编译时间失败，RTC 保持原值");
        ESP_RETURN_ON_ERROR(device_rtc_set_datetime(&compile_datetime),
                            TAG,
                            "使用固件编译时间校准 RTC 失败");
    }

    device_rtc_snapshot_t snapshot;
    ESP_RETURN_ON_ERROR(device_rtc_get_snapshot_copy(&snapshot), TAG, "读取 RTC 快照失败");

    /* 日志墙上时钟校准失败不影响 RTC 自身可用性。 */
    ESP_ERROR_CHECK_WITHOUT_ABORT(system_clock_sync_from_rtc());
    ESP_LOGI(TAG,
             "RTC 时间: %04u-%02u-%02u %s %02u:%02u:%02u，VL=%s",
             (unsigned) snapshot.datetime.year,
             (unsigned) snapshot.datetime.month,
             (unsigned) snapshot.datetime.day,
             s_bootstart_app_weekday_names[snapshot.weekday],
             (unsigned) snapshot.datetime.hour,
             (unsigned) snapshot.datetime.minute,
             (unsigned) snapshot.datetime.second,
             snapshot.voltage_low ? "置位" : "正常");
    return ESP_OK;
}

/**
 * @brief 初始化温湿度、电池 Device 与按需环境采样 Service
 *
 * @return ESP_OK 能力已初始化；或首个初始化错误码
 */
esp_err_t bootstart_app_start_environment(void)
{
    bool      environment_initialized = false;
    bool      battery_initialized     = false;
    esp_err_t ret                     = ESP_OK;

    ESP_GOTO_ON_ERROR(device_environment_init(), cleanup, TAG, "初始化温湿度设备失败");
    environment_initialized = true;
    ESP_GOTO_ON_ERROR(device_battery_init(), cleanup, TAG, "初始化电池设备失败");
    battery_initialized = true;
    ESP_GOTO_ON_ERROR(environment_service_init(), cleanup, TAG, "初始化按需环境采样服务失败");
    return ESP_OK;

cleanup:
    if (battery_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(device_battery_deinit());
    }
    if (environment_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(device_environment_deinit());
    }
    return ret;
}

/**
 * @brief 初始化 SD Device 并启动插拔自动挂载 Service
 *
 * @return ESP_OK 能力已启动；或首个初始化、启动错误码
 */
esp_err_t bootstart_app_start_sd(void)
{
    esp_err_t ret        = ESP_OK;
    esp_err_t stop_error = ESP_OK;

    ESP_RETURN_ON_ERROR(device_sd_init(), TAG, "初始化 SD 设备失败");
    ESP_GOTO_ON_ERROR(sd_card_service_start(), cleanup, TAG, "启动 SD 卡服务失败");
    return ESP_OK;

cleanup:
    stop_error = sd_card_service_stop();
    if (stop_error != ESP_OK && stop_error != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(stop_error);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_sd_deinit());
    return ret;
}

/**
 * @brief 初始化四灰阶墨水屏、网络管理器、远端日志捕获和独立 OTA Task
 *
 * @return ESP_OK 本地关键通信能力已就绪；或初始化错误码
 */
esp_err_t bootstart_app_init_local_communication(void)
{
    ESP_LOGI(TAG, "开始初始化墨水屏与网络能力");
    bool      display_initialized    = false;
    bool      remote_log_initialized = false;
    bool      ota_initialized        = false;
    bool      ota_started            = false;
    esp_err_t ret                    = ESP_OK;

    ESP_GOTO_ON_ERROR(device_display_init(DEVICE_DISPLAY_MODE_GRAYSCALE_4),
                      cleanup,
                      TAG,
                      "初始化配网显示能力失败");
    display_initialized = true;
    ESP_GOTO_ON_ERROR(network_manager_init_borrow(&s_bootstart_app_network_config_store),
                      cleanup,
                      TAG,
                      "初始化网络管理器失败");

    remote_log_config_t remote_log_config;
    esp_err_t           remote_log_error = remote_log_config_set_defaults(&remote_log_config);
    if (remote_log_error == ESP_OK)
    {
        remote_log_error = remote_log_init(&remote_log_config);
    }
    if (remote_log_error == ESP_OK)
    {
        remote_log_initialized = true;
        ESP_LOGI(TAG, "远端日志缓存与 Log V2 捕获初始化完成");
    }
    else
    {
        ESP_LOGW(TAG,
                 "远端日志初始化失败，本轮继续使用本地串口日志: %s",
                 esp_err_to_name(remote_log_error));
    }

    const firmware_ota_config_t ota_config = {
        .check_timeout_ms    = BOOTSTART_APP_FIRMWARE_OTA_CHECK_TIMEOUT_MS,
        .download_timeout_ms = BOOTSTART_APP_FIRMWARE_OTA_DOWNLOAD_TIMEOUT_MS,
    };
    ESP_GOTO_ON_ERROR(firmware_ota_init(&ota_config), cleanup, TAG, "初始化固件 OTA 工具失败");
    ota_initialized = true;
    ESP_GOTO_ON_ERROR(firmware_ota_start(), cleanup, TAG, "启动固件 OTA Task 失败");
    ota_started = true;
    ESP_GOTO_ON_ERROR(firmware_ota_confirm_running_image(),
                      cleanup,
                      TAG,
                      "确认 OTA 镜像本地健康状态失败");
    ESP_LOGI(TAG, "本地通信能力与 OTA Task 已就绪，启动镜像健康确认完成");
    return ESP_OK;

cleanup:
    if (ota_started)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(firmware_ota_stop());
    }
    if (ota_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(firmware_ota_deinit());
    }
    if (remote_log_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(remote_log_deinit());
    }
    if (display_initialized)
    {
        esp_err_t cleanup_error = device_display_sleep();
        if (cleanup_error == ESP_OK)
        {
            cleanup_error = device_display_deinit();
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(cleanup_error);
    }
    return ret;
}

/** @brief 读取已生效的服务配置并尽力启动远端日志上传 */
static void bootstart_app_start_remote_log(void)
{
    protocol_backend_context_t backend;
    esp_err_t                  error = bootstart_app_load_backend_context_copy(&backend);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "缺少远端日志服务配置，本轮只保留串口日志: %s",
                 esp_err_to_name(error == ESP_OK ? ESP_ERR_NOT_FOUND : error));
        return;
    }

    error = remote_log_configure_copy(&backend);
    if (error == ESP_OK)
    {
        error = remote_log_start();
    }
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动远端日志上传失败，本轮继续使用串口日志: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "远端日志上传 Task 已启动");
}

/**
 * @brief 使用已初始化的显示与网络能力运行配网用例
 *
 * @param[in] wakeup_context 本次唤醒来源
 * @return ESP_OK 网络在线；或配网错误码
 */
esp_err_t bootstart_app_run_provisioning(const bootstart_app_wakeup_context_t *wakeup_context)
{
    ESP_RETURN_ON_FALSE(wakeup_context != NULL, ESP_ERR_INVALID_ARG, TAG, "配网启动缺少唤醒上下文");

    const provisioning_app_config_t provisioning_config = {
        .woken_by_button = wakeup_context->woken_by_button,
        .woken_by_timer  = wakeup_context->woken_by_timer,
    };
    ESP_RETURN_ON_ERROR(provisioning_app_run_until_online(&provisioning_config),
                        TAG,
                        "配网启动或运行失败");
    ESP_LOGI(TAG, "网络已可用，配网检查流程完成");
    bootstart_app_start_remote_log();
    return ESP_OK;
}

/**
 * @brief 初始化尚未启动扫描的按键 Device 与 Service
 *
 * @return ESP_OK 成功；或按键初始化错误码
 */
static esp_err_t bootstart_app_init_buttons(void)
{
    ESP_RETURN_ON_ERROR(device_button_init(), TAG, "初始化按键设备失败");

    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(button_service_init(), cleanup, TAG, "初始化按键服务失败");
    return ESP_OK;

cleanup:
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_button_deinit());
    return ret;
}

/**
 * @brief 装配本地显示链路，并按一次性 OTA 标记决定是否恢复正常页面
 *
 * @param[out] out_refresh_ready true 表示内容刷新 App 已初始化，等待事件订阅后启动
 * @return ESP_OK 本地播放已启动；或关键显示链路错误码
 */
esp_err_t bootstart_app_start_photo_pipeline(bool *out_refresh_ready)
{
    ESP_RETURN_ON_FALSE(out_refresh_ready != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "照片链路就绪状态输出指针为空");

    *out_refresh_ready                                = false;
    bool       buttons_initialized                    = false;
    bool       collection_initialized                 = false;
    bool       present_initialized                    = false;
    bool       playback_initialized                   = false;
    esp_err_t  ret                                    = ESP_OK;
    const bool ota_display_restore_pending            = bootstart_app_should_restore_ota_display();
    const photo_playback_app_config_t playback_config = {
        .present_active_on_start = ota_display_restore_pending,
        .first_presented_callback =
            ota_display_restore_pending ? bootstart_app_confirm_ota_display_restored : NULL,
        .first_presented_context = NULL,
    };

    if (ota_display_restore_pending)
    {
        ESP_LOGI(TAG, "检测到 OTA 提示画面待恢复，本轮启动将全屏呈现当前页面");
    }

    ESP_GOTO_ON_ERROR(bootstart_app_init_buttons(), cleanup, TAG, "初始化照片播放按键链路失败");
    buttons_initialized = true;
    ESP_GOTO_ON_ERROR(display_collection_service_init(), cleanup, TAG, "初始化照片集合服务失败");
    collection_initialized = true;
    ESP_GOTO_ON_ERROR(display_present_service_init(), cleanup, TAG, "初始化照片呈现服务失败");
    present_initialized = true;

    ESP_GOTO_ON_ERROR(photo_playback_app_init(&playback_config),
                      cleanup,
                      TAG,
                      "初始化照片播放 App 失败");
    playback_initialized = true;
    ESP_GOTO_ON_ERROR(photo_playback_app_start(), cleanup, TAG, "启动照片播放 App 失败");

    ESP_LOGI(TAG, "本地照片集合、呈现与按键播放链路已启动");

    protocol_backend_context_t backend;
    ret = bootstart_app_load_backend_context_copy(&backend);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "缺少内容刷新服务配置，本地照片与按键轮播继续运行: %s",
                 esp_err_to_name(ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret));
        return ESP_OK;
    }
    const esp_err_t ota_config_error = firmware_ota_configure_copy(&backend);
    if (ota_config_error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "固件 OTA 服务配置失败，本轮仅继续内容刷新: %s",
                 esp_err_to_name(ota_config_error));
    }
    const content_refresh_app_config_t refresh_config = {
        .backend    = &backend,
        .timeout_ms = BOOTSTART_APP_CONTENT_REFRESH_TIMEOUT_MS,
    };
    ret = content_refresh_app_init(&refresh_config);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "后台内容刷新初始化失败，本地照片继续运行: %s", esp_err_to_name(ret));
    }
    else
    {
        *out_refresh_ready = true;
        ESP_LOGI(TAG,
                 "内容刷新链路初始化完成: device_id=%s, HTTP 超时=%d ms",
                 backend.device_id,
                 BOOTSTART_APP_CONTENT_REFRESH_TIMEOUT_MS);
    }
    return ESP_OK;

cleanup:
    if (playback_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(photo_playback_app_deinit());
    }
    if (present_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(display_present_service_deinit());
    }
    if (collection_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(display_collection_service_deinit());
    }
    if (buttons_initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(button_service_deinit());
        ESP_ERROR_CHECK_WITHOUT_ABORT(device_button_deinit());
    }
    return ret;
}

void bootstart_app_init_feedback_devices(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_led_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_buzzer_init());
}

/**
 * @brief 启动手动与周期深睡协调 App，并在首轮刷新前完成回调订阅
 *
 * @param[in] automatic_sleep_enabled 是否启用自动深睡流程
 * @return ESP_OK 已启动；或初始化、启动错误码
 */
esp_err_t bootstart_app_start_power_management(bool automatic_sleep_enabled)
{
    const power_management_app_config_t config = {
        .automatic_sleep_enabled = automatic_sleep_enabled,
        .interactive_awake_ms    = BOOTSTART_APP_INTERACTIVE_AWAKE_MS,
    };
    bool      initialized = false;
    esp_err_t ret         = ESP_OK;
    ESP_GOTO_ON_ERROR(power_management_app_init(&config), cleanup, TAG, "初始化整机深睡协调失败");
    initialized = true;
    ESP_GOTO_ON_ERROR(power_management_app_start(), cleanup, TAG, "启动整机深睡协调失败");
    ESP_LOGI(TAG,
             "整机深睡协调已启动: 自动深睡=%s, 无活动窗口=%lu 秒",
             automatic_sleep_enabled ? "启用" : "关闭",
             (unsigned long) (BOOTSTART_APP_INTERACTIVE_AWAKE_MS / 1000U));
    return ESP_OK;

cleanup:
    if (initialized)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(power_management_app_deinit());
    }
    return ret;
}

/** @brief 在电源管理 App 完成事件订阅后启动首轮刷新，启动失败时提交退避休眠事实 */
void bootstart_app_start_content_refresh(bool refresh_ready)
{
    if (!refresh_ready)
    {
        return;
    }
    const esp_err_t error = content_refresh_app_start();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "后台内容刷新启动失败，准备按退避计划休眠: %s", esp_err_to_name(error));
        const esp_err_t deinit_error = content_refresh_app_deinit();
        if (deinit_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚内容刷新初始化资源失败: %s", esp_err_to_name(deinit_error));
        }
        const esp_err_t report_error = power_management_app_report_refresh_start_failure(error);
        if (report_error != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "提交内容刷新启动失败事实失败，无法进入退避休眠: %s",
                     esp_err_to_name(report_error));
        }
    }
    else
    {
        ESP_LOGI(TAG, "首轮后台内容刷新已启动");
    }
}

/**
 * @brief 初始化网络配置持久化和可信系统时钟基础资源
 *
 * @return ESP_OK 成功；或存储、时钟初始化错误码
 */
esp_err_t bootstart_app_init_system_services(void)
{
    ESP_LOGI(TAG, "开始初始化持久化与系统时钟");
    ESP_RETURN_ON_ERROR(system_storage_init(), TAG, "初始化网络配置持久化失败");
    ESP_RETURN_ON_ERROR(system_clock_init(), TAG, "初始化系统时钟失败");
    return ESP_OK;
}
