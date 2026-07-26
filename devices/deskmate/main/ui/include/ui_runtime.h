/*
 * 文件职责：声明 UI Runtime 的公共生命周期契约。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "presentation_page.h"

/** @brief UI Runtime 唯一运行状态 */
typedef enum
{
    UI_RUNTIME_STATE_UNINIT = 0, /*!< 尚未初始化生命周期资源 */
    UI_RUNTIME_STATE_STARTING,   /*!< 正在创建 Task 和控件树 */
    UI_RUNTIME_STATE_RUNNING,    /*!< 正在接收并渲染呈现事件 */
    UI_RUNTIME_STATE_STOPPING,   /*!< 正在关闭消息入口并等待图形传输静止 */
    UI_RUNTIME_STATE_STOPPED,    /*!< 已停止执行，运行时资源保留，可再次启动 */
    UI_RUNTIME_STATE_FAILED,     /*!< 启停失败，需反初始化 */
} ui_runtime_state_t;

/** @brief UI 向产品层上报的用户意图或生命周期事实 */
typedef enum
{
    UI_USER_INTENT_SCREEN_LOADED = 0,       /*!< 顶层 Screen 已完成加载 */
    UI_USER_INTENT_SETTINGS_MENU_CLOSED,    /*!< 设置菜单已关闭 */
    UI_USER_INTENT_SETTINGS_START_PORTAL,   /*!< 用户请求启动配网 Portal */
    UI_USER_INTENT_SETTINGS_OTA_CHECK,      /*!< 用户请求手动检查更新 */
    UI_USER_INTENT_SETTINGS_OTA_INSTALL,    /*!< 用户确认安装待更新固件 */
    UI_USER_INTENT_SETTINGS_OTA_DISCARD,    /*!< 用户放弃并清除待安装目标 */
    UI_USER_INTENT_SETTINGS_START_WEB_FILE, /*!< 用户进入子页并请求启动网页文件管理 */
    UI_USER_INTENT_SETTINGS_STOP_WEB_FILE,  /*!< 用户离开子页并请求停止网页文件管理 */
    UI_USER_INTENT_COUNT,                   /*!< 意图数量（哨兵值） */
} ui_user_intent_id_t;

/** @brief UI 用户意图的按值载荷 */
typedef struct
{
    ui_user_intent_id_t    id;   /*!< 意图类型 */
    presentation_page_id_t page; /*!< Screen 生命周期意图对应的页面 */
} ui_user_intent_t;

/**
 * @brief UI 用户意图借用回调
 *
 * 回调在持有 LVGL 锁的 UI Task 上下文执行，只允许投递非阻塞产品命令或更新短临界区状态；
 * 禁止调用 LVGL 或重新进入 UI Runtime。
 *
 * @param[in] intent 按值意图的只读地址
 * @param[in] context 注册时长期借用的上下文
 * @return ESP_OK 意图已接收；其他值表示产品层拒绝或命令投递失败
 */
typedef esp_err_t (*ui_user_intent_callback_t)(const ui_user_intent_t *intent, void *context);

/**
 * @brief 初始化 UI Runtime 生命周期资源
 *
 * 重复调用保持幂等；同时注册 Presentation 事件入口。LVGL、字体和控件树在启动时初始化。
 *
 * @return ESP_OK 初始化成功；其他值表示队列创建失败
 */
esp_err_t ui_runtime_init(void);

/**
 * @brief 注册或清除 UI 用户意图的长期借用回调
 *
 * 仅允许在 UI Runtime 处于 STOPPED 时调用。传入 NULL 可清除回调，此时 context 也必须为 NULL。
 *
 * @param[in] callback 回调函数；NULL 表示清除
 * @param[in] context 回调上下文，由调用方保证借用期有效
 * @return ESP_OK 已更新；ESP_ERR_INVALID_ARG 参数组合无效；
 *         ESP_ERR_INVALID_STATE UI Runtime 当前状态不允许修改
 */
esp_err_t ui_runtime_set_user_intent_callback_borrow(ui_user_intent_callback_t callback, void *context);

/**
 * @brief 从 UI 内部同步上报一条用户意图
 *
 * 本函数只复制回调和上下文后同步调用，不持有 Runtime 状态锁执行外部代码。
 *
 * @param[in] intent 按值意图的只读地址
 * @return ESP_OK 已接收；ESP_ERR_INVALID_ARG 意图无效；
 *         ESP_ERR_INVALID_STATE 尚未注册回调；其他值由回调返回
 */
esp_err_t ui_runtime_emit_user_intent(const ui_user_intent_t *intent);

/**
 * @brief 启动 UI Runtime 并等待平台、字体、控件树和恢复画面 READY
 *
 * 从 STOPPED 恢复时，返回 ESP_OK 前会把 Presenter 最新状态同步到保留控件树，并完成一次
 * 显示传输；首次启动时等待 Task 创建、初始化和首屏控件树 READY。
 *
 * @param[in] timeout_ms 启动或恢复与 READY 回执的总超时
 * @return ESP_OK 已进入 RUNNING；其他值表示状态、超时或初始化失败
 */
esp_err_t ui_runtime_start(uint32_t timeout_ms);

/**
 * @brief 同步关闭业务入口并可逆停止 UI Runtime
 *
 * 返回 ESP_OK 后 LVGL 定时执行和显示提交均已停止，但 UI Task、控件树、字体映射、
 * 绘制缓冲、显示 Task、SPI 和面板控制器仍保留；后续可通过 ui_runtime_start() 恢复。
 *
 * @param[in] timeout_ms 控制命令、在途发送者和停止完成的总超时
 * @return ESP_OK 已进入 STOPPED；其他值表示状态或超时错误
 */
esp_err_t ui_runtime_stop(uint32_t timeout_ms);

/**
 * @brief 在 UI Runtime 停止后释放全部运行时和生命周期资源并恢复 UNINIT
 *
 * 仅允许从 STOPPED 或 FAILED 调用。若保留的 UI Task 仍存在，本函数会让该 Task
 * 在内部 SRAM 栈上释放控件树、字体、LVGL 和显示资源后退出。
 *
 * @return ESP_OK 已反初始化；其他值表示仍有 Task 或控制请求正在使用资源
 */
esp_err_t ui_runtime_deinit(void);

/**
 * @brief 获取 UI Runtime 状态机快照
 *
 * @return 当前状态
 */
ui_runtime_state_t ui_runtime_get_state(void);
