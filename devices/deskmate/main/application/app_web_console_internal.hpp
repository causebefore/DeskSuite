/*
 * 文件职责：声明 app_web_console C++ 状态所有者与一次性 Task 之间的组件私有协作接口。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_web_console.h"

/**
 * @brief 使用 DeskMate 唯一固定配置初始化网页控制台 Service
 *
 * 本函数是产品存储 Provider、Files 限额和 HTTP 端口的唯一装配入口。仅在 Service 为
 * `UNINITIALIZED` 时调用；配置字符串与回调在 Service 内复制，空 Provider 上下文无需借用
 * 产品对象。
 *
 * @return ESP_OK Service 已初始化；其他值来自 `web_console_service_init_borrow()`
 */
esp_err_t app_web_console_internal_initialize_service(void);

/**
 * @brief 为一次启动原子检查状态与展示版本容量并发布 CHECKING_STORAGE
 *
 * @return ESP_OK 已进入 CHECKING_STORAGE；ESP_ERR_INVALID_STATE 状态、资源所有权或展示版本
 *         容量不允许启动
 */
esp_err_t app_web_console_internal_prepare_start(void);

/**
 * @brief 在记录停止序列前确认 Application 已初始化
 *
 * @return ESP_OK 停止请求可以进入 Task 私有序列；ESP_ERR_INVALID_STATE Application 尚未初始化
 */
esp_err_t app_web_console_internal_validate_stop_request(void);

/**
 * @brief 为无活动 Task 的停止请求收敛状态或准备清理重试
 *
 * @param[out] out_needs_task true 表示需要创建 Task 清理资源或可靠发布停止终态
 * @return ESP_OK 已决定处理方式；ESP_ERR_INVALID_ARG 输出为空；
 *         ESP_ERR_INVALID_STATE Application 尚未初始化
 */
esp_err_t app_web_console_internal_prepare_stop_retry(bool *out_needs_task);

/** @brief 原子更新产品状态，并在锁外推送 Presenter 副本和呈现事件 */
void app_web_console_internal_publish_state(app_web_console_state_t state, esp_err_t error, bool clear_runtime);

/**
 * @brief 发布由 UI 调用栈直接处理的 Task 创建同步拒绝
 *
 * 调用方必须仍独占 Task 创建门，且 `s_task` 尚未发布。本函数在 Presenter 推送互斥量内写入
 * 最新 ERROR View Model；被接受时清除准备态遗留的事件 pending，不为同步拒绝新增异步终态
 * 事件。公共 request API 返回非 `ESP_OK` 后，调用方应直接读取快照并呈现该错误。
 *
 * @param[in] error Task 创建失败错误
 */
void app_web_console_internal_publish_synchronous_rejection(esp_err_t error);

/**
 * @brief 重试发布已由 Presenter 接受但尚未进入默认 Event Loop 的状态刷新
 *
 * 本函数与 Presenter 更新共用同一互斥量；成功后清除持久 pending，失败时保留。调用方不得
 * 据此轮询网络或其他业务事实。
 *
 * @return ESP_OK 当前没有 pending 或事件已经入队；其他值表示默认 Event Loop 暂未接收
 */
esp_err_t app_web_console_internal_retry_status_update(void);

/**
 * @brief 读取当前链路和 Service 访问码并一次性发布完整 RUNNING 快照
 *
 * 本函数只从网页控制台 Application Task 调用。链路与 Service 快照在产品状态锁外读取；
 * Service 必须已经处于 RUNNING，访问码必须是六位数字。当前链路不可用时 URL 为空，但
 * Service 仍可进入 RUNNING 等待 Network Manager 自动重连。
 *
 * @return ESP_OK 已原子写入 URL、访问码与 RUNNING 状态并推送 Presenter；
 *         ESP_ERR_INVALID_STATE 产品或 Service 状态发生竞态；其他值为快照读取错误
 */
esp_err_t app_web_console_internal_publish_running_snapshot(void);

/**
 * @brief 按当前内存链路事实刷新 RUNNING URL 并按需推送 Presenter
 *
 * 本函数不访问磁盘或 Service，不修改访问码；断线或链路读取失败时把 URL 置空。同一 URL
 * 不重复发布。只允许网页控制台 Application Task 在无待处理停止请求时调用。
 *
 * @return ESP_OK 已刷新或无需变化；ESP_ERR_INVALID_STATE 当前不在 RUNNING
 */
esp_err_t app_web_console_internal_refresh_running_link(void);

/** @brief 更新已确认的文件系统容量；下一次状态发布会携带该副本 */
void app_web_console_internal_set_capacity(uint64_t total_bytes, uint64_t free_bytes);

/** @brief 记录或清除当前 Application 持有的网页控制台网络租约代次 */
void app_web_console_internal_set_lease_generation(uint32_t generation);

/** @brief 读取当前 Application 持有的网页控制台网络租约代次 */
uint32_t app_web_console_internal_get_lease_generation(void);

/** @brief 标记 Service 是否仍需停止或反初始化 */
void app_web_console_internal_set_service_cleanup_required(bool required);

/** @brief 读取当前产品状态，供一次性 Task 选择启动或清理入口 */
app_web_console_state_t app_web_console_internal_get_state(void);

/** @brief 创建或通知一次性 Task，异步处理启动意图 */
esp_err_t app_web_console_task_request_start(void);

/** @brief 为每个停止意图分配序列，并在需要时创建或通知一次性清理 Task */
esp_err_t app_web_console_task_request_stop(void);

/**
 * @brief 合并 Network Manager 链路变化事实并唤醒活动网页控制台 Task
 *
 * 本函数可从 `app_network_task` 调用，只执行有界静态状态更新和 Task notification；无活动
 * Task 时忽略通知，启动流程随后会主动读取当前链路。
 */
void app_web_console_task_notify_link_change(void);
