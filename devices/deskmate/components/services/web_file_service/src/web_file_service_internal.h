/**
 * @file web_file_service_internal.h
 * @brief 网页文件服务的内部安全边界接口
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
#include "web_file_service.h"

#define WEB_FILE_ACCESS_CODE_LENGTH          6U
#define WEB_FILE_ACCESS_CODE_BUFFER_SIZE     7U
#define WEB_FILE_TOKEN_BYTES                 16U
#define WEB_FILE_TOKEN_BUFFER_SIZE           33U
#define WEB_FILE_LOGICAL_PATH_BUFFER_SIZE    512U
#define WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE 544U
#define WEB_FILE_SESSION_IDLE_TIMEOUT_US     (10LL * 60LL * 1000LL * 1000LL)
#define WEB_FILE_LOGIN_LOCKOUT_US            (30LL * 1000LL * 1000LL)
#define WEB_FILE_LOGIN_MAX_FAILURES          5U
#define WEB_FILE_TRANSFER_BUFFER_SIZE        (32U * 1024U)
#define WEB_FILE_UPLOAD_MAX_SIZE_BYTES       (500U * 1024U * 1024U)
#define WEB_FILE_TRANSACTION_DIR             "/sdcard/.deskmate-web"
#define WEB_FILE_TRANSACTION_PART            "/sdcard/.deskmate-web/upload.part"
#define WEB_FILE_TRANSACTION_BACKUP          "/sdcard/.deskmate-web/upload.bak"
#define WEB_FILE_TRANSACTION_JOURNAL         "/sdcard/.deskmate-web/transaction"
#define WEB_FILE_TRANSACTION_NEW             "/sdcard/.deskmate-web/transaction.new"

typedef enum
{
    WEB_FILE_AUTH_OK = 0,
    WEB_FILE_AUTH_BAD_CODE,
    WEB_FILE_AUTH_LOCKED,
    WEB_FILE_AUTH_SESSION_BUSY,
    WEB_FILE_AUTH_UNAUTHORIZED,
    WEB_FILE_AUTH_EXPIRED,
} web_file_auth_result_t;

typedef struct
{
    char    access_code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE];
    uint8_t token[WEB_FILE_TOKEN_BYTES];
    bool    session_active;
    uint8_t failed_attempts;
    int64_t lockout_until_us;
    int64_t last_activity_us;
} web_file_auth_state_t;

typedef enum
{
    WEB_FILE_TRANSACTION_PREPARED = 0,
    WEB_FILE_TRANSACTION_BACKUP_MOVED,
    WEB_FILE_TRANSACTION_TARGET_COMMITTED,
} web_file_transaction_phase_t;

typedef enum
{
    WEB_FILE_RECOVERY_REMOVE_PART = 0,
    WEB_FILE_RECOVERY_RESTORE_BACKUP,
    WEB_FILE_RECOVERY_ACCEPT_COMMIT,
    WEB_FILE_RECOVERY_AMBIGUOUS,
} web_file_recovery_action_t;

typedef struct
{
    web_file_transaction_phase_t phase;
    uint64_t                     expected_length;
    char                         target_path[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
} web_file_transaction_t;

/**
 * @brief 网页文件服务唯一运行期上下文及其资源所有权
 *
 * `lock` 保护生命周期、认证、handler 计数和传输所有权等短时内存状态；任何网络、文件、
 * 等待或 HTTPD 外部调用都必须在锁外执行。`server` 在 HTTPD 清理成功前始终归 Service
 * 所有，清理失败时连同 `CLEANUP_FAILED` 状态保留。URI handler 注销必须在 HTTPD Task
 * 上下文执行，`ingress_closed` 只通知准入屏障完成；`handlers_drained` 只负责唤醒停止调用，
 * `active_handlers == 0` 才是排空完成条件。一次性 `httpd_stop_task` 独占无超时的 HTTPD
 * 销毁调用，完成后通知等待方、发布结果并挂起，直到生命周期调用显式删除它。
 * `lifecycle_active` 串行化可能阻塞或失败的生命周期操作。
 */
typedef struct
{
    SemaphoreHandle_t        lock;                    /**< `init()` 至 `deinit()` 持有的状态锁 */
    SemaphoreHandle_t        handlers_drained;        /**< handler 归零时发出的二值唤醒信号 */
    SemaphoreHandle_t        ingress_closed;          /**< URI handler 注销完成时发出的二值唤醒信号 */
    SemaphoreHandle_t        httpd_stop_completed;    /**< HTTPD 销毁调用返回时的二值唤醒信号 */
    httpd_handle_t           server;                  /**< Service 独占的 HTTPD 句柄 */
    TaskHandle_t             httpd_stop_task;         /**< 等待回收的一次性 HTTPD 清理 Task */
    web_file_service_state_t state;                   /**< 当前生命周期状态 */
    web_file_auth_state_t    auth;                    /**< 锁内访问的访问码、token 与会话状态 */
    uint32_t                 active_handlers;         /**< 已进入且尚未离开的 handler 数 */
    bool                     accepting_requests;      /**< 是否允许新 handler 使用运行期资源 */
    bool                     transfer_active;         /**< 是否已有文件请求持有传输所有权 */
    int                      active_transfer_socket;  /**< 活动传输 socket；无活动传输时为 -1 */
    uint8_t                 *transfer_buffer;         /**< Service 持有的文件传输缓冲区 */
    esp_err_t                last_error;              /**< 最近一次生命周期错误 */
    bool                     lifecycle_active;        /**< 是否已有生命周期操作占用 HTTPD 所有权 */
    uint32_t                 registered_handler_mask; /**< HTTPD 中仍注册的项目 handler */
    esp_err_t                ingress_close_error;     /**< 最近一次 URI 准入关闭结果 */
    bool                     ingress_close_queued;    /**< 是否已有 HTTPD Task 注销工作排队 */
    esp_err_t                httpd_stop_result;       /**< 一次性清理 Task 发布的 HTTPD 结果 */
    bool                     httpd_stop_in_progress;  /**< 是否已有 Task 独占 `server` 销毁 */
    bool                     httpd_stop_result_ready; /**< 是否有待生命周期调用收取的销毁结果 */
} web_file_service_context_t;

/**
 * @brief 网页文件 Service 的唯一运行期上下文
 *
 * 生命周期实现与文件传输实现共享此对象；所有并发字段都必须在 `lock` 内访问。
 */
extern web_file_service_context_t s_context;

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
esp_err_t web_file_httpd_stop_task_create(web_file_service_context_t *context, TaskHandle_t *out_task);

/**
 * @brief 记账当前 HTTP handler 并读取请求接纳状态
 *
 * 在 Service 锁可用时，本函数先在锁内增加活动 handler 计数，再读取请求接纳状态，使
 * `stop()` 能可靠等待所有已经进入的 handler。handler 必须在最外层无条件将每次调用与一次
 * `web_file_handler_leave()` 配对，包括返回 false 的路径；锁尚未创建时 leave 是安全空操作。
 * 返回 false 时 handler 必须立即配对 leave 并返回错误，不得发送响应或访问认证、文件和传输
 * 资源，避免停止期的新请求引入额外网络等待。
 *
 * @return true 请求已记账且进入时允许继续；false Service 锁不可用或当前不接纳请求
 */
bool web_file_handler_enter(void);

/**
 * @brief 结束当前 HTTP handler 记账
 *
 * handler 必须在所有返回路径恰好调用一次，且调用时不得持有 Service 状态锁。计数归零时
 * 在锁外发出唤醒信号；停止调用仍会在锁内复查计数，不把信号量本身视为完成条件。
 */
void web_file_handler_leave(void);

/**
 * @brief 对逻辑路径执行一次百分号解码、严格校验并映射到文件系统路径
 *
 * 解码后的路径必须以 `/` 开头且使用最短形式的有效 UTF-8 标量；输入中的路径分隔符既可
 * 保持为 `/`，也可编码为 `%2F`。函数拒绝空段、`.`、`..`、控制字符、反斜杠、超长段以及
 * 首段保留目录 `.deskmate-web`。输出仅在成功时有效。
 *
 * @param[in] encoded 百分号编码的逻辑路径
 * @param[out] logical 解码后的规范逻辑路径
 * @param[in] logical_size 逻辑路径缓冲区容量
 * @param[out] filesystem 映射到固定挂载点后的文件系统路径
 * @param[in] filesystem_size 文件系统路径缓冲区容量
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 路径或参数无效；
 *         ESP_ERR_INVALID_SIZE 路径、段或输出缓冲区长度不合法
 */
esp_err_t web_file_path_decode_and_map(const char *encoded, char *logical, size_t logical_size, char *filesystem,
                                       size_t filesystem_size);

/**
 * @brief 重新校验规范逻辑路径并映射到固定文件系统挂载点
 *
 * 本函数不执行百分号解码，供已经通过路径内核的事务记录在提交和恢复时重新建立物理路径。
 *
 * @param[in] logical 规范逻辑路径
 * @param[out] filesystem 映射到固定挂载点后的文件系统路径
 * @param[in] filesystem_size 文件系统路径缓冲区容量
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 路径或参数无效；
 *         ESP_ERR_INVALID_SIZE 路径、段或输出缓冲区长度不合法
 */
esp_err_t web_file_path_map_logical(const char *logical, char *filesystem, size_t filesystem_size);

/**
 * @brief 把字符串转义为可安全放入 JSON 字符串值的内容
 *
 * @param[in] input 输入字符串
 * @param[out] output 转义结果，不包含外围双引号
 * @param[in] output_size 输出缓冲区容量
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_SIZE 输出缓冲区不足
 */
esp_err_t web_file_json_escape(const char *input, char *output, size_t output_size);

/**
 * @brief 按 Content-Disposition filename* 的 UTF-8 百分号形式编码字符串
 *
 * @param[in] input 输入字符串
 * @param[out] output 百分号编码结果
 * @param[in] output_size 输出缓冲区容量
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_SIZE 输出缓冲区不足
 */
esp_err_t web_file_percent_encode(const char *input, char *output, size_t output_size);

/**
 * @brief 认证后浏览指定逻辑目录
 *
 * @param[in] request HTTPD 请求
 * @return ESP_OK 响应成功；其他错误码表示拒绝或传输中止
 */
esp_err_t web_file_handle_files_get(httpd_req_t *request);

/**
 * @brief 认证后以固定 Content-Length 流式下载指定常规文件
 *
 * 实现仅用于固定 `/sdcard` FatFs 挂载；打开后会再次复核文件类型和长度，使用
 * `httpd_send()` 发送完整响应头和原始正文，不使用 chunked response API。
 *
 * @param[in] request HTTPD 请求
 * @return ESP_OK 响应成功；其他错误码表示拒绝或传输中止
 */
esp_err_t web_file_handle_file_get(httpd_req_t *request);

/**
 * @brief 认证后以可恢复事务接收并提交原始 HTTP PUT 文件
 *
 * @param[in] request HTTPD 请求
 * @return ESP_OK 响应成功；其他错误码表示拒绝或传输中止
 */
esp_err_t web_file_handle_file_put(httpd_req_t *request);

/**
 * @brief 认证后创建一个新目录
 *
 * @param[in] request HTTPD 请求
 * @return ESP_OK 响应成功；其他错误码表示拒绝或文件系统操作失败
 */
esp_err_t web_file_handle_directory_put(httpd_req_t *request);

/**
 * @brief 认证后把一个常规文件重命名或移动到目标路径
 *
 * @param[in] request HTTPD 请求
 * @return ESP_OK 响应成功；其他错误码表示拒绝或文件系统操作失败
 */
esp_err_t web_file_handle_file_patch(httpd_req_t *request);

/**
 * @brief 认证后删除一个常规文件或空目录
 *
 * @param[in] request HTTPD 请求
 * @return ESP_OK 响应成功；其他错误码表示拒绝或文件系统操作失败
 */
esp_err_t web_file_handle_file_delete(httpd_req_t *request);

/**
 * @brief 校验上传长度不超过固定的 500 MiB 上限
 *
 * @param[in] content_length 上传正文长度
 * @return ESP_OK 长度有效；ESP_ERR_INVALID_SIZE 超过上限
 */
esp_err_t web_file_upload_validate_length(size_t content_length);

/**
 * @brief 根据 journal 阶段和持久化产物选择唯一恢复动作
 *
 * @param[in] phase journal 阶段
 * @param[in] target_exists 目标路径是否存在
 * @param[in] backup_exists 固定备份是否存在
 * @param[in] part_exists 固定临时文件是否存在
 * @param[in] target_matches_expected_length 目标是否为预期长度的常规文件
 * @return 唯一恢复动作；产物集合无法唯一解释时返回 `WEB_FILE_RECOVERY_AMBIGUOUS`
 */
web_file_recovery_action_t web_file_transaction_decide_recovery(web_file_transaction_phase_t phase, bool target_exists,
                                                                bool backup_exists, bool part_exists,
                                                                bool target_matches_expected_length);

/**
 * @brief 提交已完整同步的覆盖上传事务
 *
 * journal 和重命名按恢复安全顺序串行执行。失败时尽力立即恢复；无法安全收敛的持久化产物
 * 会保留给下一次启动恢复。
 *
 * @param[in] transaction 目标逻辑路径、预期长度和初始阶段
 * @return ESP_OK 新目标已提交且事务元数据已清理；其他错误表示提交或清理失败
 */
esp_err_t web_file_transaction_commit(const web_file_transaction_t *transaction);

/**
 * @brief 在全部上传预检通过后创建空的 Service 事务目录
 *
 * @return ESP_OK 目录已创建或原本为空；其他错误表示目录类型、残留产物或创建失败
 */
esp_err_t web_file_transaction_prepare_upload(void);

/**
 * @brief 清理尚未写入 journal 的临时上传
 *
 * 仅在目录中不存在 journal 和备份时删除 `.part`，状态不符时保留现场并返回错误。
 *
 * @return ESP_OK 未提交临时文件已清理；其他错误表示状态冲突或删除失败
 */
esp_err_t web_file_transaction_abort_upload(void);

/**
 * @brief 把已同步临时文件直接提交到不存在的新目标
 *
 * @param[in] transaction 目标逻辑路径和预期长度
 * @return ESP_OK 新文件已提交且元数据已清理；其他错误表示提交、取消或清理失败
 */
esp_err_t web_file_transaction_commit_new(const web_file_transaction_t *transaction);

/**
 * @brief 在 HTTPD 启动前恢复唯一可解释的残留上传事务
 *
 * 状态不唯一、journal 非法或出现未知产物时返回错误，且不猜测或删除用户可见文件。
 *
 * @return ESP_OK 无残留或恢复完成；其他错误表示必须拒绝启动
 */
esp_err_t web_file_transaction_recover(void);

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
