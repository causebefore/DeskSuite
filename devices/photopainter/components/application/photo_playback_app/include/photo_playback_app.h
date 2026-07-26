/**
 * @file photo_playback_app.h
 * @brief 本地照片选择、按键导航与墨水屏刷新 Application
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 固件状态页允许的最大 ASCII 行数 */
#define PHOTO_PLAYBACK_APP_STATUS_LINE_MAX 4U
/** @brief 单行 ASCII 文本最大字符数，不含结尾空字符 */
#define PHOTO_PLAYBACK_APP_STATUS_TEXT_MAX 32U

/** @brief 照片播放 App 状态 */
typedef enum
{
    PHOTO_PLAYBACK_APP_STATE_STOPPED = 0, /**< 已停止 */
    PHOTO_PLAYBACK_APP_STATE_NO_CONTENT,  /**< 当前没有活动集合 */
    PHOTO_PLAYBACK_APP_STATE_IDLE,        /**< 可接受导航 */
    PHOTO_PLAYBACK_APP_STATE_PRESENTING,  /**< 正在刷新墨水屏 */
    PHOTO_PLAYBACK_APP_STATE_MODAL_PAGE,  /**< 正在显示模态 ASCII 状态页 */
    PHOTO_PLAYBACK_APP_STATE_STOPPING,    /**< 正在停止 */
    PHOTO_PLAYBACK_APP_STATE_ERROR,       /**< 运行错误，需要新集合或重启恢复 */
    PHOTO_PLAYBACK_APP_STATE_CLEANUP_FAILED, /**< 停止清理失败 */
} photo_playback_app_state_t;

/**
 * @brief 第一张页面物理刷新成功后的确认回调
 *
 * 回调在照片播放 Task 上下文、状态锁外执行。返回 ESP_OK 后回调自动失效；返回错误时，
 * App 会在下一张页面成功刷新后重试。回调必须快速返回，不得同步停止或反初始化播放 App。
 *
 * @param[in] context 初始化时传入的借用上下文
 * @return ESP_OK 已确认成功；其他错误码表示需要保留回调并重试
 */
typedef esp_err_t (*photo_playback_app_first_presented_cb_t)(void *context);

/** @brief 照片播放 App 启动策略 */
typedef struct
{
    bool present_active_on_start; /**< true 启动即显示本地集合；false 保留面板现有画面 */
    photo_playback_app_first_presented_cb_t
        first_presented_callback; /**< 第一张页面物理刷新成功后的可选确认回调 */
    void *first_presented_context; /**< 第一张页面确认回调的借用上下文 */
} photo_playback_app_config_t;

/** @brief 照片播放 App 状态快照 */
typedef struct
{
    photo_playback_app_state_t state; /**< 当前状态 */
    bool      has_current;            /**< 是否已有成功呈现页面 */
    bool      collection_settled;     /**< 是否已收敛到一个稳定集合或明确无内容 */
    uint8_t   current_index;          /**< 当前页面索引 */
    uint64_t  settled_collection_generation; /**< 已完成呈现或无内容确认的集合代数 */
    char      current_page_id[DISPLAY_PROTOCOL_PAGE_ID_MAX]; /**< 当前稳定页面 ID */
    char      current_content_version[DISPLAY_PROTOCOL_VERSION_MAX]; /**< 当前内容版本 */
    uint32_t  rejected_navigation_count; /**< 被状态或队列拒绝的导航次数 */
    esp_err_t last_error;                 /**< 最近错误 */
} photo_playback_app_status_t;

/**
 * @brief 确认键长按结束后的完整内容刷新请求回调
 *
 * 回调在照片播放 Task 上下文执行，必须快速提交请求并返回，不得同步执行网络或显示事务，
 * 也不得同步停止 photo_playback_app。返回 ESP_OK 表示请求已被异步接收，其他错误码表示
 * 本次请求被拒绝。
 *
 * @param[in] context 注册时传入的上下文
 * @return ESP_OK 请求已接收；或拒绝原因
 */
typedef esp_err_t (*photo_playback_app_refresh_request_cb_t)(void *context);

/**
 * @brief 左键持续三秒松开后的一次性固件检查请求回调
 *
 * 回调在照片播放 Task 上下文执行，必须快速提交请求并返回，不得同步停止播放 App。
 *
 * @param[in] context 注册时传入的借用上下文
 * @return ESP_OK 请求已接收；或拒绝原因
 */
typedef esp_err_t (*photo_playback_app_firmware_check_request_cb_t)(void *context);

/** @brief 模态状态页允许提交给协调方的按键动作 */
typedef enum
{
    PHOTO_PLAYBACK_APP_MODAL_ACTION_LEFT = 0, /**< 左键单击取消或返回 */
    PHOTO_PLAYBACK_APP_MODAL_ACTION_CONFIRM, /**< 确认键单击确认安装 */
} photo_playback_app_modal_action_t;

/**
 * @brief 模态状态页按键动作回调
 *
 * 回调在照片播放 Task 上下文执行，必须只提交快速通知，不得同步刷新显示、启停网络或停止 App。
 * 模态捕获期间右键及所有其他长按事件会被消费，不触发普通导航或内容刷新。
 *
 * @param[in] action 用户动作
 * @param[in] context 注册时传入的借用上下文
 * @return ESP_OK 动作已接收；或拒绝原因
 */
typedef esp_err_t (*photo_playback_app_modal_action_cb_t)(
    photo_playback_app_modal_action_t action, void *context);

/** @brief 模态 ASCII 状态页中的一行坐标式文本 */
typedef struct
{
    uint16_t x_pixels; /**< 文本左上角横坐标 */
    uint16_t y_pixels; /**< 文本左上角纵坐标 */
    const char *text;  /**< 调用期间借用并复制的 ASCII 文本 */
    uint8_t scale;     /**< 5x7 字模整数缩放倍数 */
} photo_playback_app_status_line_t;

/** @brief 一次集合显示收敛尝试的完成事实 */
typedef struct
{
    uint64_t collection_generation; /**< 本次尝试对应的集合代数 */
    bool has_content; /**< true 表示尝试呈现页面，false 表示明确没有活动内容 */
    esp_err_t result; /**< ESP_OK 已显示；ESP_ERR_NOT_FOUND 明确无内容；或呈现错误 */
} photo_playback_app_collection_settled_event_t;

/**
 * @brief 集合显示收敛事件回调
 *
 * 回调在照片播放 Task 上下文、状态锁外执行，必须快速复制事实并返回。
 *
 * @param[in] event 收敛事件，仅在回调期间借用
 * @param[in] context 注册时传入的借用上下文
 */
typedef void (*photo_playback_app_collection_settled_cb_t)(
    const photo_playback_app_collection_settled_event_t *event, void *context);

/**
 * @brief 用户成功完成一次左右导航后的活动回调
 *
 * @param[in] context 注册时传入的借用上下文
 */
typedef void (*photo_playback_app_activity_cb_t)(void *context);

/**
 * @brief 初始化播放 App 的启动策略、队列、停止信号和长期回调借用
 *
 * 调用前必须已初始化 display_collection_service、display_present_service 和已停止的
 * button_service。本函数复制配置和回调地址，不保存配置结构指针，也不启动 Task 或按键扫描。
 * 回调上下文借用持续到回调返回 ESP_OK 或 deinit，以先发生者为准。
 *
 * @param[in] config 启动策略；常规启动应将 `present_active_on_start` 设为 false 以保留面板画面
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置为空；ESP_ERR_INVALID_STATE 生命周期不满足；
 *         ESP_ERR_NO_MEM 资源不足；或回调注册错误码
 */
esp_err_t photo_playback_app_init(const photo_playback_app_config_t *config);

/**
 * @brief 启动播放 Task，并按初始化策略呈现本地集合或保留面板现有画面
 *
 * @return ESP_OK Task 已启动；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM 创建失败
 */
esp_err_t photo_playback_app_start(void);

/**
 * @brief 请求显示上一张照片
 *
 * 请求非阻塞入队；只有 IDLE 状态接受。页面首尾循环。
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 当前不可导航；ESP_ERR_TIMEOUT 队列已满
 */
esp_err_t photo_playback_app_request_previous(void);

/**
 * @brief 请求显示下一张照片
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 当前不可导航；ESP_ERR_TIMEOUT 队列已满
 */
esp_err_t photo_playback_app_request_next(void);

/**
 * @brief 串行显示一次多行 ASCII 模态状态页
 *
 * 本函数把全部文本复制到内部命令后同步等待照片播放 Task 完成一次物理全刷，因而不会与照片
 * 导航或集合收敛并发。调用方应先启用模态按键捕获。
 *
 * @param[in] lines 一至 PHOTO_PLAYBACK_APP_STATUS_LINE_MAX 行文本
 * @param[in] line_count 文本行数
 * @return ESP_OK 已显示；ESP_ERR_INVALID_ARG 行或文本无效；ESP_ERR_INVALID_STATE 生命周期不允许；
 *         ESP_ERR_TIMEOUT 命令或显示等待超时；或显示错误码
 */
esp_err_t photo_playback_app_present_status_page_copy(
    const photo_playback_app_status_line_t *lines, size_t line_count);

/**
 * @brief 串行恢复当前缓存照片页面
 *
 * 优先恢复最近成功页面；尚未保存当前页时恢复活动集合默认页。函数同步等待物理全刷完成，成功
 * 后回到普通 IDLE 状态。
 *
 * @return ESP_OK 已恢复；ESP_ERR_NOT_FOUND 没有可恢复照片；ESP_ERR_INVALID_STATE 生命周期不允许；
 *         ESP_ERR_TIMEOUT 等待超时；或集合、显示错误码
 */
esp_err_t photo_playback_app_restore_current_page(void);

/**
 * @brief 复制播放 App 当前状态
 *
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t photo_playback_app_get_status_copy(photo_playback_app_status_t *out_status);

/**
 * @brief 设置或清除确认键长按完整内容刷新请求回调
 *
 * 回调借用持续到下一次设置、传入 NULL 清除或 photo_playback_app_deinit()，以先发生者
 * 为准。允许在播放 Task 运行期间替换；实际回调始终在照片播放 Task 上下文、状态锁外执行。
 *
 * @param[in] callback 回调；传入 NULL 表示清除
 * @param[in] context 回调上下文；callback 为 NULL 时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t photo_playback_app_set_refresh_request_callback_borrow(
    photo_playback_app_refresh_request_cb_t callback, void *context);

/**
 * @brief 设置或清除左键三秒长按固件检查请求回调
 *
 * 回调借用持续到下一次设置、传入 NULL 清除或 deinit，以先发生者为准。
 *
 * @param[in] callback 回调；NULL 表示清除
 * @param[in] context 回调上下文；callback 为 NULL 时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t photo_playback_app_set_firmware_check_request_callback_borrow(
    photo_playback_app_firmware_check_request_cb_t callback, void *context);

/**
 * @brief 开始模态按键捕获并借用动作回调
 *
 * 启用后左键和确认键单击只提交模态动作，右键、普通导航和所有长按语义均被消费。允许在播放
 * Task 运行期间启用；重复启用会替换回调借用。
 *
 * @param[in] callback 非空快速动作回调
 * @param[in] context 回调上下文
 * @return ESP_OK 已启用；ESP_ERR_INVALID_ARG 回调为空；ESP_ERR_INVALID_STATE App 未运行
 */
esp_err_t photo_playback_app_begin_modal_borrow(
    photo_playback_app_modal_action_cb_t callback, void *context);

/**
 * @brief 结束模态按键捕获并恢复普通按键语义
 *
 * @return ESP_OK 已结束或原本未启用；ESP_ERR_INVALID_STATE App 未初始化
 */
esp_err_t photo_playback_app_end_modal(void);

/**
 * @brief 设置或清除集合显示收敛回调
 *
 * @param[in] callback 回调；NULL 表示清除
 * @param[in] context 回调上下文；callback 为 NULL 时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t photo_playback_app_set_collection_settled_callback_borrow(
    photo_playback_app_collection_settled_cb_t callback, void *context);

/**
 * @brief 设置或清除用户导航活动回调
 *
 * @param[in] callback 回调；NULL 表示清除
 * @param[in] context 回调上下文；callback 为 NULL 时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t photo_playback_app_set_activity_callback_borrow(
    photo_playback_app_activity_cb_t callback, void *context);

/**
 * @brief 同步停止播放 Task 和按键扫描
 *
 * 最长等待 40 秒，以容纳一次已经开始的照片或状态页刷新。回调借用保持到 deinit。
 *
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 未运行；ESP_ERR_TIMEOUT Task 未退出；
 *         或按键停止错误码
 */
esp_err_t photo_playback_app_stop(void);

/**
 * @brief 清除回调并释放播放 App 资源
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化、仍在运行或清理失败；
 *         或回调清除错误码
 */
esp_err_t photo_playback_app_deinit(void);

#ifdef __cplusplus
}
#endif
