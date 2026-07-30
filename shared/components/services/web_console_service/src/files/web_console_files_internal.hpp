/**
 * @file web_console_files_internal.hpp
 * @brief 网页控制台 Files 内部模块的状态、路由与文件安全接口
 */
#pragma once

#include "web_console_files.h"
#include "web_console_service_internal.hpp"

#define WEB_FILE_LOGICAL_PATH_BUFFER_SIZE     512U
#define WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE  (WEB_CONSOLE_FILES_MOUNT_ROOT_MAX_LENGTH + WEB_FILE_LOGICAL_PATH_BUFFER_SIZE)
#define WEB_FILE_TRANSFER_BUFFER_SIZE         (32U * 1024U)
#define WEB_FILE_TRANSACTION_DIR_BUFFER_SIZE  (WEB_CONSOLE_FILES_MOUNT_ROOT_MAX_LENGTH + 1U \
                                               + WEB_CONSOLE_FILES_WORKSPACE_NAME_MAX_LENGTH + 1U)
#define WEB_FILE_TRANSACTION_PATH_BUFFER_SIZE (WEB_FILE_TRANSACTION_DIR_BUFFER_SIZE + sizeof("/transaction.new"))

enum web_file_transaction_phase_t
{
    WEB_FILE_TRANSACTION_PREPARED = 0,
    WEB_FILE_TRANSACTION_BACKUP_MOVED,
    WEB_FILE_TRANSACTION_TARGET_COMMITTED,
};

enum web_file_recovery_action_t
{
    WEB_FILE_RECOVERY_REMOVE_PART = 0,
    WEB_FILE_RECOVERY_RESTORE_BACKUP,
    WEB_FILE_RECOVERY_ACCEPT_COMMIT,
    WEB_FILE_RECOVERY_AMBIGUOUS,
};

struct web_file_transaction_t
{
    web_file_transaction_phase_t phase;
    uint64_t                     expected_length;
    char                         target_path[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
};

/**
 * @brief Files 模块唯一运行期上下文
 *
 * 配置字段在 `configure_borrow()` 完成后保持只读，直到 handler 全部排空后由 `reset_context()`
 * 清除；传输运行期字段由 Console Core 的 `s_context.lock` 保护。Files 不拥有独立生命周期锁。
 */
struct web_console_files_context_t
{
    bool                                     configured;
    bool                                     transfer_active;
    int                                      active_transfer_socket;
    uint8_t                                 *transfer_buffer;
    web_console_files_get_capacity_copy_cb_t get_capacity_copy;
    void                                    *storage_context;
    uint64_t                                 upload_max_bytes;
    uint64_t                                 reserved_free_bytes;
    char                                     mount_root[WEB_CONSOLE_FILES_MOUNT_ROOT_MAX_LENGTH + 1U];
    char                                     workspace_name[WEB_CONSOLE_FILES_WORKSPACE_NAME_MAX_LENGTH + 1U];
    char                                     transaction_directory[WEB_FILE_TRANSACTION_DIR_BUFFER_SIZE];
    char                                     transaction_part[WEB_FILE_TRANSACTION_PATH_BUFFER_SIZE];
    char                                     transaction_backup[WEB_FILE_TRANSACTION_PATH_BUFFER_SIZE];
    char                                     transaction_journal[WEB_FILE_TRANSACTION_PATH_BUFFER_SIZE];
    char                                     transaction_new[WEB_FILE_TRANSACTION_PATH_BUFFER_SIZE];
};

extern web_console_files_context_t s_files_context;

#define WEB_FILE_TRANSACTION_DIR     (s_files_context.transaction_directory)
#define WEB_FILE_TRANSACTION_PART    (s_files_context.transaction_part)
#define WEB_FILE_TRANSACTION_BACKUP  (s_files_context.transaction_backup)
#define WEB_FILE_TRANSACTION_JOURNAL (s_files_context.transaction_journal)
#define WEB_FILE_TRANSACTION_NEW     (s_files_context.transaction_new)

/** @brief 返回 Files 模块固定的六个领域路由。 */
const web_console_route_t *web_console_files_get_routes(size_t *out_count);

/** @brief 在 HTTPD 启动前恢复 Files 持有的残留事务。 */
esp_err_t web_console_files_prepare_start(void);

/** @brief 验证并复制 Files 配置，长期借用容量回调上下文至 reset。 */
esp_err_t web_console_files_configure_borrow(const web_console_files_config_t *config);

/** @brief 在 handler 排空后释放 Files 运行期传输资源。 */
void web_console_files_cleanup_after_handlers(void);

/** @brief 在持有 Core 锁时判断 Files 是否已释放全部运行期资源。 */
bool web_console_files_is_idle_locked(void);

/** @brief 在 init/deinit 边界把 Files 上下文恢复为无活动 socket 的初始值。 */
void web_console_files_reset_context(void);

/** @brief 锁内复制容量回调及上下文，锁外查询当前存储容量。 */
esp_err_t web_console_files_get_capacity_copy(web_console_files_capacity_t *out_capacity);

esp_err_t web_file_path_decode_and_map(const char *encoded, char *logical, size_t logical_size, char *filesystem,
                                       size_t filesystem_size);
esp_err_t web_file_path_map_logical(const char *logical, char *filesystem, size_t filesystem_size);
esp_err_t web_file_json_escape(const char *input, char *output, size_t output_size);
esp_err_t web_file_percent_encode(const char *input, char *output, size_t output_size);
esp_err_t web_file_handle_files_get(httpd_req_t *request);
esp_err_t web_file_handle_file_get(httpd_req_t *request);
esp_err_t web_file_handle_file_put(httpd_req_t *request);
esp_err_t web_file_handle_mutation(httpd_req_t *request);
esp_err_t web_file_upload_validate_length(size_t content_length);
web_file_recovery_action_t web_file_transaction_decide_recovery(web_file_transaction_phase_t phase, bool target_exists,
                                                                bool backup_exists, bool part_exists,
                                                                bool target_matches_expected_length);
esp_err_t web_file_transaction_commit(const web_file_transaction_t *transaction);
esp_err_t web_file_transaction_prepare_upload(void);
esp_err_t web_file_transaction_abort_upload(void);
esp_err_t web_file_transaction_commit_new(const web_file_transaction_t *transaction);
esp_err_t web_file_transaction_recover(void);
