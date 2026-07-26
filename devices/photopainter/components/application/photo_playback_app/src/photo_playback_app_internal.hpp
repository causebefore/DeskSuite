/**
 * @file photo_playback_app_internal.hpp
 * @brief 照片播放 App 私有 Runtime、消息和 Task 接口
 */
#pragma once

#include <type_traits>

#include "button_service.h"
#include "display_collection_service.h"
#include "photo_playback_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** @brief 导航 Queue 长度 */
static constexpr UBaseType_t PHOTO_PLAYBACK_NAVIGATION_QUEUE_LENGTH = 4U;
/** @brief 串行显示控制 Queue 长度 */
static constexpr UBaseType_t PHOTO_PLAYBACK_CONTROL_QUEUE_LENGTH    = 2U;
/** @brief Task 栈大小，覆盖同步显示、局刷规划和 SD 缓存提交的完整调用链 */
static constexpr uint32_t PHOTO_PLAYBACK_TASK_STACK_SIZE            = 8192U;
/** @brief Task 优先级 */
static constexpr UBaseType_t PHOTO_PLAYBACK_TASK_PRIORITY           = 3U;
/** @brief 停止最长等待时间 */
static constexpr uint32_t PHOTO_PLAYBACK_STOP_TIMEOUT_MS            = 40000U;
/** @brief 状态页或页面恢复同步命令最长等待时间 */
static constexpr uint32_t PHOTO_PLAYBACK_CONTROL_TIMEOUT_MS         = 40000U;
/** @brief 集合变化通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_COLLECTION_CHANGED  = 1UL << 0U;
/** @brief 停止通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_STOP                = 1UL << 1U;
/** @brief 确认键长按完整内容刷新请求通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_REFRESH_REQUEST     = 1UL << 2U;
/** @brief 左键三秒长按固件检查请求通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_FIRMWARE_CHECK      = 1UL << 3U;
/** @brief 左键固件检查请求最短持续时间 */
static constexpr int64_t PHOTO_PLAYBACK_FIRMWARE_CHECK_HOLD_US      = 3000000LL;
/** @brief 串行显示控制命令通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_CONTROL             = 1UL << 4U;
/** @brief 模态左键单击通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_MODAL_LEFT          = 1UL << 5U;
/** @brief 模态确认键单击通知位 */
static constexpr uint32_t PHOTO_PLAYBACK_NOTIFY_MODAL_CONFIRM       = 1UL << 6U;

/** @brief 导航方向 */
enum class PhotoPlaybackNavigation : uint8_t
{
    Previous = 0,
    Next,
};

/** @brief 按值复制到 FreeRTOS Queue 的导航消息 */
struct PhotoPlaybackNavigationMessage
{
    PhotoPlaybackNavigation navigation;
};

/** @brief 串行显示控制命令类型 */
enum class PhotoPlaybackControlKind : uint8_t
{
    PresentStatus = 0,
    RestoreCurrent,
};

/** @brief 控制命令中拥有的 ASCII 行副本 */
struct PhotoPlaybackStatusLineCopy
{
    uint16_t x_pixels;
    uint16_t y_pixels;
    uint8_t scale;
    char text[PHOTO_PLAYBACK_APP_STATUS_TEXT_MAX + 1U];
};

/** @brief 按值复制到控制 Queue 的状态页或恢复命令 */
struct PhotoPlaybackControlMessage
{
    PhotoPlaybackControlKind kind;
    uint8_t line_count;
    PhotoPlaybackStatusLineCopy lines[PHOTO_PLAYBACK_APP_STATUS_LINE_MAX];
};

static_assert(std::is_trivially_copyable_v<PhotoPlaybackNavigationMessage>,
              "照片导航消息必须可按值复制");
static_assert(std::is_trivially_copyable_v<PhotoPlaybackControlMessage>,
              "照片显示控制消息必须可按值复制");

/** @brief 照片播放 App 进程期唯一 Runtime */
class PhotoPlaybackRuntime final {
  public:
    PhotoPlaybackRuntime()                                        = default;
    PhotoPlaybackRuntime(const PhotoPlaybackRuntime &)            = delete;
    PhotoPlaybackRuntime &operator=(const PhotoPlaybackRuntime &) = delete;
    PhotoPlaybackRuntime(PhotoPlaybackRuntime &&)                 = delete;
    PhotoPlaybackRuntime &operator=(PhotoPlaybackRuntime &&)      = delete;

    bool              initialized             = false;   /**< 生命周期初始化标记 */
    bool              present_active_on_start = true;    /**< 启动时是否立即呈现本地活动集合 */
    photo_playback_app_first_presented_cb_t first_presented_callback =
        nullptr; /**< 第一张页面成功刷新确认回调 */
    void *first_presented_context = nullptr; /**< 第一张页面成功刷新确认回调上下文 */
    bool              button_running          = false;   /**< 按键扫描是否由本 App 启动 */
    QueueHandle_t     navigation_queue        = nullptr; /**< 导航消息队列 */
    QueueHandle_t     control_queue           = nullptr; /**< 状态页与恢复命令队列 */
    SemaphoreHandle_t control_mutex           = nullptr; /**< 串行同步控制调用 */
    SemaphoreHandle_t control_done            = nullptr; /**< 控制命令完成握手 */
    SemaphoreHandle_t task_stopped            = nullptr; /**< Task 退出握手 */
    TaskHandle_t      task                    = nullptr; /**< 播放 Task */
    portMUX_TYPE      state_lock              = portMUX_INITIALIZER_UNLOCKED; /**< 状态短临界区 */
    photo_playback_app_status_t           status                 = {};        /**< 对外状态 */
    display_collection_page_t             current_page           = {};      /**< 当前成功呈现页面 */
    photo_playback_app_refresh_request_cb_t refresh_request_callback = nullptr; /**< 刷新请求回调 */
    void                                *refresh_request_context = nullptr; /**< 刷新请求回调上下文 */
    photo_playback_app_firmware_check_request_cb_t firmware_check_request_callback =
        nullptr; /**< 固件检查请求回调 */
    void *firmware_check_request_context = nullptr; /**< 固件检查请求回调上下文 */
    bool modal_active = false; /**< 是否消费普通按键并提交模态动作 */
    bool collection_change_deferred = false; /**< 模态期间是否延后集合收敛 */
    photo_playback_app_modal_action_cb_t modal_action_callback = nullptr; /**< 模态动作回调 */
    void *modal_action_context = nullptr; /**< 模态动作回调上下文 */
    esp_err_t control_result = ESP_OK; /**< 最近一次同步控制命令结果 */
    int64_t                             left_press_started_at_us = 0; /**< 左键有效按下的单调时间 */
    photo_playback_app_collection_settled_cb_t collection_settled_callback =
        nullptr;                                                           /**< 收敛回调 */
    void                            *collection_settled_context = nullptr; /**< 收敛回调上下文 */
    photo_playback_app_activity_cb_t activity_callback          = nullptr; /**< 用户活动回调 */
    void                            *activity_context = nullptr; /**< 用户活动回调上下文 */
    esp_err_t                        stop_result      = ESP_OK;  /**< Task 停止清理结果 */
};

/** @brief 播放 App 唯一 Runtime */
extern PhotoPlaybackRuntime g_photo_playback_runtime;

/** @brief 启动播放 Task */
esp_err_t photo_playback_app_task_start(void);

/** @brief 同步停止播放 Task */
esp_err_t photo_playback_app_task_stop(void);

/** @brief 注册到 button_service 的快速事件入口 */
void photo_playback_app_on_button_event(device_button_id_t button, device_button_event_t event,
                                        uint8_t click_count, void *context);

/** @brief 注册到集合 Service 的快速提交入口 */
void photo_playback_app_on_collection_committed(const display_collection_snapshot_t *snapshot,
                                                void                                *context);
