/**
 * @file web_console_service_internal.hpp
 * @brief 网页控制台 Service 的内部安全边界接口
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "web_console_service.h"

/** @brief 六位数字访问码的有效字符数 */
#define WEB_FILE_ACCESS_CODE_LENGTH       6U
/** @brief 访问码缓冲区容量，含 NUL 终止符 */
#define WEB_FILE_ACCESS_CODE_BUFFER_SIZE  7U
/** @brief 会话 token 的原始随机字节数 */
#define WEB_FILE_TOKEN_BYTES              16U
/** @brief token 十六进制文本缓冲区容量，含 NUL 终止符 */
#define WEB_FILE_TOKEN_BUFFER_SIZE        33U
/** @brief 会话空闲超时时间，单位微秒（10 分钟） */
#define WEB_FILE_SESSION_IDLE_TIMEOUT_US  (10LL * 60LL * 1000LL * 1000LL)
/** @brief 连续认证失败后的锁定时长，单位微秒（30 秒） */
#define WEB_FILE_LOGIN_LOCKOUT_US         (30LL * 1000LL * 1000LL)
/** @brief 触发锁定前允许的最大连续认证失败次数 */
#define WEB_FILE_LOGIN_MAX_FAILURES       5U
#define WEB_CONSOLE_CORE_ROUTE_COUNT      4U
#if CONFIG_WEB_CONSOLE_FILES
#define WEB_CONSOLE_FILES_ROUTE_COUNT     6U
#else
#define WEB_CONSOLE_FILES_ROUTE_COUNT 0U
#endif
#if CONFIG_WEB_CONSOLE_SETTINGS
#define WEB_CONSOLE_SETTINGS_ROUTE_COUNT 3U
#else
#define WEB_CONSOLE_SETTINGS_ROUTE_COUNT 0U
#endif
#if CONFIG_WEB_CONSOLE_STATUS
#define WEB_CONSOLE_STATUS_ROUTE_COUNT 1U
#else
#define WEB_CONSOLE_STATUS_ROUTE_COUNT 0U
#endif
#if CONFIG_WEB_CONSOLE_ACTIONS
#define WEB_CONSOLE_ACTIONS_ROUTE_COUNT 2U
#else
#define WEB_CONSOLE_ACTIONS_ROUTE_COUNT 0U
#endif
#define WEB_CONSOLE_PROVIDER_ROUTE_COUNT \
    (WEB_CONSOLE_SETTINGS_ROUTE_COUNT + WEB_CONSOLE_STATUS_ROUTE_COUNT \
     + WEB_CONSOLE_ACTIONS_ROUTE_COUNT)
#define WEB_CONSOLE_ROUTE_COUNT \
    (WEB_CONSOLE_CORE_ROUTE_COUNT + WEB_CONSOLE_FILES_ROUTE_COUNT + WEB_CONSOLE_PROVIDER_ROUTE_COUNT)

/**
 * @brief 认证流程的状态转换结果
 */
enum web_file_auth_result_t
{
    WEB_FILE_AUTH_OK = 0,       /**< 认证或授权成功 */
    WEB_FILE_AUTH_BAD_CODE,     /**< 访问码格式或内容不正确 */
    WEB_FILE_AUTH_LOCKED,       /**< 连续失败过多，处于锁定期 */
    WEB_FILE_AUTH_SESSION_BUSY, /**< 已存在未过期的活动会话 */
    WEB_FILE_AUTH_UNAUTHORIZED, /**< token 校验未通过 */
    WEB_FILE_AUTH_EXPIRED,      /**< 会话因空闲超时已过期 */
};

/**
 * @brief 访问码、会话 token 与登录防护的聚合状态
 */
struct web_file_auth_state_t
{
    char    access_code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE]; /**< 当前有效的六位数字访问码 */
    uint8_t token[WEB_FILE_TOKEN_BYTES];                   /**< 当前会话的 128 位随机 token */
    bool    session_active;                                /**< 是否存在活动会话 */
    uint8_t failed_attempts;                               /**< 当前锁定周期内的连续失败次数 */
    int64_t lockout_until_us;                              /**< 锁定解除的单调时间，单位微秒 */
    int64_t last_activity_us;                              /**< 最近一次成功授权或刷新的单调时间 */
};

/**
 * @brief 组件内部的领域路由描述
 *
 * Core 把本描述转换为 ESP-IDF `httpd_uri_t`，并在统一 dispatcher 中完成准入与 handler
 * 记账。内部模块只提供 URI、Method 和领域处理函数，不接触注册状态。
 */
struct web_console_route_t
{
    const char    *uri;
    httpd_method_t method;
    esp_err_t (*handle)(httpd_req_t *request);
};

/**
 * @brief 网页控制台 Service 唯一运行期上下文及其资源所有权
 *
 * `lock` 保护生命周期、认证和 handler 计数等短时内存状态；任何网络、文件、
 * 等待或 HTTPD 外部调用都必须在锁外执行。`server` 在 HTTPD 清理成功前始终归 Service
 * 所有，清理失败时连同 `CLEANUP_FAILED` 状态保留。URI handler 注销必须在 HTTPD Task
 * 上下文执行，`ingress_closed` 只通知准入屏障完成；`handlers_drained` 只负责唤醒停止调用，
 * `active_handlers == 0` 才是排空完成条件。一次性 `httpd_stop_task` 独占无超时的 HTTPD
 * 销毁调用，完成后通知等待方、发布结果并挂起，直到生命周期调用显式删除它。
 * `lifecycle_active` 串行化可能阻塞或失败的生命周期操作。
 */
struct web_console_service_context_t
{
    SemaphoreHandle_t        lock;                    /**< `init()` 至 `deinit()` 持有的状态锁 */
    SemaphoreHandle_t        handlers_drained;        /**< handler 归零时发出的二值唤醒信号 */
    SemaphoreHandle_t        ingress_closed;          /**< URI handler 注销完成时发出的二值唤醒信号 */
    SemaphoreHandle_t        httpd_stop_completed;    /**< HTTPD 销毁调用返回时的二值唤醒信号 */
    httpd_handle_t           server;                  /**< Service 独占的 HTTPD 句柄 */
    TaskHandle_t             httpd_stop_task;         /**< 等待回收的一次性 HTTPD 清理 Task */
    web_console_service_state_t state;                /**< 当前生命周期状态 */
    web_file_auth_state_t    auth;                    /**< 锁内访问的访问码、token 与会话状态 */
    uint16_t                 server_port;             /**< 初始化时复制的 HTTP 监听端口 */
    uint32_t                 active_handlers;         /**< 已进入且尚未离开的 handler 数 */
    bool                     accepting_requests;      /**< 是否允许新 handler 使用运行期资源 */
    esp_err_t                last_error;              /**< 最近一次生命周期错误 */
    bool                     lifecycle_active;        /**< 是否已有生命周期操作占用 HTTPD 所有权 */
    uint32_t                 registered_route_count; /**< HTTPD 中仍注册的项目路由数 */
    esp_err_t                ingress_close_error;     /**< 最近一次 URI 准入关闭结果 */
    bool                     ingress_close_queued;    /**< 是否已有 HTTPD Task 注销工作排队 */
    esp_err_t                httpd_stop_result;       /**< 一次性清理 Task 发布的 HTTPD 结果 */
    bool                     httpd_stop_in_progress;  /**< 是否已有 Task 独占 `server` 销毁 */
    bool                     httpd_stop_result_ready; /**< 是否有待生命周期调用收取的销毁结果 */
};

/**
 * @brief 网页控制台 Service 的唯一运行期上下文
 *
 * Core 生命周期与认证实现共享此对象；Files 模块只借用其中的锁、认证和准入事实，其传输
 * 所有权保存在独立的 Files 上下文。
 */
extern web_console_service_context_t s_context;

/**
 * @brief 创建一次性 HTTPD 清理 Task
 *
 * 调用前，调用方必须在 Service 锁内发布有效 `server` 并置
 * `httpd_stop_in_progress = true`。Task 只调用一次合法 `httpd_stop()`；调用返回后先发出
 * `httpd_stop_completed` 信号，再发布结果并永久挂起，不自行删除。调用方观察到结果后必须
 * 使用返回的 Task 句柄显式删除它，再清空结果和服务器所有权。
 *
 * @param[in,out] context 生命周期长于清理 Task 的 Service 上下文
 * @param[out] out_task 创建成功时返回由 Service 独占的 Task 句柄
 * @return ESP_OK Task 已创建；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 创建失败
 */
esp_err_t web_file_httpd_stop_task_create(web_console_service_context_t *context, TaskHandle_t *out_task);

/**
 * @brief 记账当前 HTTP handler 并读取请求接纳状态
 *
 * 在 Service 锁可用时，本函数先在锁内增加活动 handler 计数，再读取请求接纳状态，使
 * `stop()` 能可靠等待所有已经进入的 handler。handler 必须在最外层无条件将每次调用与一次
 * `web_console_handler_leave()` 配对，包括返回 false 的路径；锁尚未创建时 leave 是安全空操作。
 * 返回 false 时 handler 必须立即配对 leave 并返回错误，不得发送响应或访问认证、文件和传输
 * 资源，避免停止期的新请求引入额外网络等待。
 *
 * @return true 请求已记账且进入时允许继续；false Service 锁不可用或当前不接纳请求
 */
bool web_console_handler_enter(void);

/**
 * @brief 结束当前 HTTP handler 记账
 *
 * handler 必须在所有返回路径恰好调用一次，且调用时不得持有 Service 状态锁。计数归零时
 * 在锁外发出唤醒信号；停止调用仍会在锁内复查计数，不把信号量本身视为完成条件。
 */
void web_console_handler_leave(void);

/**
 * @brief 注册网页控制台 Service 的全部 HTTP URI
 *
 * 每项注册成功后立即标记对应固定槽，启动失败回滚据此只注销已经注册的入口。
 *
 * @param[in] server 已启动且由 Service 持有的 HTTPD 句柄
 * @return ESP_OK 全部注册完成；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_register_handlers(httpd_handle_t server);

/**
 * @brief 在绝对期限内注销全部 HTTP URI，关闭新请求入口
 *
 * @param[in] server Service 当前持有的 HTTPD 句柄
 * @param[in] deadline_us 单调时钟绝对期限
 * @return ESP_OK 入口已关闭；ESP_ERR_TIMEOUT 等待超时；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_close_ingress(httpd_handle_t server, int64_t deadline_us);

/**
 * @brief 在持有 Core 锁时检查全部固定路由槽是否已经注销
 *
 * @return true 所有槽均未注册；false 至少一个槽仍由 HTTPD 持有
 */
bool web_console_http_routes_are_released_locked(void);

/**
 * @brief 清空认证状态并设置六位数字访问码
 *
 * 无效访问码会使状态保持不可登录的全零访问码，避免沿用旧凭据。
 *
 * @param[out] state 认证状态
 * @param[in] access_code 以 NUL 结尾的六位 ASCII 数字访问码；NULL 表示只清空认证状态
 */
void web_file_auth_reset(web_file_auth_state_t *state, const char access_code[7]);

/**
 * @brief 校验访问码并创建唯一会话
 *
 * 访问码以固定六字节循环比较。连续第五次失败会进入 30 秒锁定；存在未过期会话时拒绝
 * 创建新会话。调用方负责提供 128 位安全随机数和单调时间。
 *
 * @param[in,out] state 认证状态
 * @param[in] code 待校验的六位 ASCII 数字访问码
 * @param[in] random_token 调用方生成的 128 位安全随机数
 * @param[in] now_us 当前单调时间，单位微秒
 * @param[out] out_token 33 字节缓冲区，成功时写入 32 位小写十六进制 token
 * @return 认证状态转换结果
 */
web_file_auth_result_t web_file_auth_create_session(web_file_auth_state_t *state, const char *code,
                                                    const uint8_t random_token[16], int64_t now_us, char out_token[33]);

/**
 * @brief 校验会话 token 并刷新空闲活动时间
 *
 * token 解码后以固定十六字节循环比较。非活动传输在空闲十分钟后失效；活动传输不因空闲
 * 时长失效，并在每次成功授权时刷新活动时间。
 *
 * @param[in,out] state 认证状态
 * @param[in] bearer 以 NUL 结尾的 32 位十六进制 token
 * @param[in] now_us 当前单调时间，单位微秒
 * @param[in] transfer_active 当前请求是否属于已经开始的活动传输
 * @return 认证状态转换结果
 */
web_file_auth_result_t web_file_auth_authorize(web_file_auth_state_t *state, const char *bearer, int64_t now_us,
                                               bool transfer_active);

/**
 * @brief 以传输完成时刻刷新仍然存在的会话空闲起点
 *
 * 本函数只推进既有活动会话的单调活动时间；会话已由停止流程清空时保持清空，不创建、不恢复
 * token 或其他认证状态。调用方必须持有 Service 锁。
 *
 * @param[in,out] state 认证状态
 * @param[in] now_us 当前单调时间，单位微秒
 */
void web_file_auth_touch_active_session(web_file_auth_state_t *state, int64_t now_us);
