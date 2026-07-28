/**
 * @file web_file_service_transaction.cpp
 * @brief 网页文件服务的上传事务提交与启动恢复实现
 */
#include "web_file_service_internal.hpp"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "web_file_service";

struct web_file_transaction_artifacts_t
{
    bool journal_exists;
    bool new_exists;
    bool part_exists;
    bool backup_exists;
};

/**
 * @brief 按事务阶段返回 journal 中的固定文本
 *
 * @param[in] phase 事务阶段
 * @return 静态阶段文本；无效阶段返回 NULL
 */
static const char *web_file_transaction_phase_name(web_file_transaction_phase_t phase)
{
    switch (phase)
    {
        case WEB_FILE_TRANSACTION_PREPARED:
            return "PREPARED";
        case WEB_FILE_TRANSACTION_BACKUP_MOVED:
            return "BACKUP_MOVED";
        case WEB_FILE_TRANSACTION_TARGET_COMMITTED:
            return "TARGET_COMMITTED";
        default:
            return NULL;
    }
}

/**
 * @brief 把 journal 阶段文本解析为固定枚举
 *
 * @param[in] text 不含换行的阶段文本
 * @param[out] out_phase 解析结果
 * @return true 文本有效；false 文本未知
 */
static bool web_file_transaction_parse_phase(const char *text, web_file_transaction_phase_t *out_phase)
{
    if (strcmp(text, "PREPARED") == 0)
    {
        *out_phase = WEB_FILE_TRANSACTION_PREPARED;
        return true;
    }
    if (strcmp(text, "BACKUP_MOVED") == 0)
    {
        *out_phase = WEB_FILE_TRANSACTION_BACKUP_MOVED;
        return true;
    }
    if (strcmp(text, "TARGET_COMMITTED") == 0)
    {
        *out_phase = WEB_FILE_TRANSACTION_TARGET_COMMITTED;
        return true;
    }
    return false;
}

/**
 * @brief 严格解析不带符号和空白的十进制 uint64_t
 *
 * @param[in] text NUL 结尾的十进制文本
 * @param[out] out_value 解析结果
 * @return true 文本非空、仅含数字且未溢出；false 格式或范围无效
 */
static bool web_file_transaction_parse_length(const char *text, uint64_t *out_value)
{
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }

    uint64_t value = 0U;
    for (const unsigned char *cursor = (const unsigned char *) text; *cursor != '\0'; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return false;
        }
        const uint64_t digit = (uint64_t) (*cursor - '0');
        if (value > (UINT64_MAX - digit) / 10U)
        {
            return false;
        }
        value = value * 10U + digit;
    }
    *out_value = value;
    return true;
}

/**
 * @brief 以 transaction.new 同步落盘并切换当前 journal
 *
 * @param[in] transaction 有界事务记录
 * @return ESP_OK journal 已提交；其他错误表示写入、同步、关闭或重命名失败
 */
static esp_err_t web_file_transaction_write_journal(const web_file_transaction_t *transaction)
{
    if (transaction == NULL || web_file_transaction_phase_name(transaction->phase) == NULL
        || transaction->expected_length > WEB_FILE_UPLOAD_MAX_SIZE_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char target_filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    if (web_file_path_map_logical(transaction->target_path, target_filesystem, sizeof(target_filesystem)) != ESP_OK
        || strcmp(transaction->target_path, "/") == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *journal = fopen(WEB_FILE_TRANSACTION_NEW, "wb");
    if (journal == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t error = ESP_OK;
    if (fprintf(journal,
                "version=1\nphase=%s\nlength=%" PRIu64 "\ntarget=%s\n",
                web_file_transaction_phase_name(transaction->phase),
                transaction->expected_length,
                transaction->target_path)
        < 0)
    {
        error = ESP_FAIL;
    }
    if (error == ESP_OK && fflush(journal) != 0)
    {
        error = ESP_FAIL;
    }
    const int descriptor = fileno(journal);
    if (error == ESP_OK && (descriptor < 0 || fsync(descriptor) != 0))
    {
        error = ESP_FAIL;
    }
    if (fclose(journal) != 0 && error == ESP_OK)
    {
        error = ESP_FAIL;
    }
    if (error != ESP_OK)
    {
        (void) remove(WEB_FILE_TRANSACTION_NEW);
        return error;
    }

    if (remove(WEB_FILE_TRANSACTION_JOURNAL) != 0 && errno != ENOENT)
    {
        return ESP_FAIL;
    }
    if (rename(WEB_FILE_TRANSACTION_NEW, WEB_FILE_TRANSACTION_JOURNAL) != 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 去除 `fgets()` 结果末尾唯一的 LF
 *
 * @param[in,out] line 待处理的 journal 行
 * @return true 行完整且正文不再含 CR/LF；false 行被截断或含非法分隔
 */
static bool web_file_transaction_strip_newline(char *line)
{
    const size_t size = strlen(line);
    if (size == 0U || line[size - 1U] != '\n')
    {
        return false;
    }
    line[size - 1U] = '\0';
    return strchr(line, '\r') == NULL && strchr(line, '\n') == NULL;
}

/**
 * @brief 严格读取四行 journal 并验证目标路径仍属于固定挂载点
 *
 * @param[in] path journal 或 transaction.new 固定路径
 * @param[out] out_transaction 解析成功后的有界事务记录
 * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE journal 格式无效；
 *         ESP_ERR_NOT_FOUND journal 不存在；其他错误表示文件系统失败
 */
static esp_err_t web_file_transaction_read_journal_at(const char *path, web_file_transaction_t *out_transaction)
{
    if (path == NULL || out_transaction == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *journal = fopen(path, "rb");
    if (journal == NULL)
    {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const auto close_journal = [journal](esp_err_t result) {
        return fclose(journal) != 0 && result == ESP_OK ? ESP_FAIL : result;
    };

    const int   descriptor = fileno(journal);
    struct stat journal_info;
    if (descriptor < 0 || fstat(descriptor, &journal_info) != 0)
    {
        return close_journal(ESP_FAIL);
    }
    if (!S_ISREG(journal_info.st_mode) || journal_info.st_size <= 0
        || journal_info.st_size > (off_t) (WEB_FILE_LOGICAL_PATH_BUFFER_SIZE + 128U))
    {
        return close_journal(ESP_ERR_INVALID_RESPONSE);
    }

    char version_line[32];
    char phase_line[64];
    char length_line[64];
    char target_line[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE + 16U];
    if (fgets(version_line, sizeof(version_line), journal) == NULL
        || fgets(phase_line, sizeof(phase_line), journal) == NULL
        || fgets(length_line, sizeof(length_line), journal) == NULL
        || fgets(target_line, sizeof(target_line), journal) == NULL || !web_file_transaction_strip_newline(version_line)
        || !web_file_transaction_strip_newline(phase_line) || !web_file_transaction_strip_newline(length_line)
        || !web_file_transaction_strip_newline(target_line) || strcmp(version_line, "version=1") != 0
        || strncmp(phase_line, "phase=", sizeof("phase=") - 1U) != 0
        || strncmp(length_line, "length=", sizeof("length=") - 1U) != 0
        || strncmp(target_line, "target=", sizeof("target=") - 1U) != 0)
    {
        return close_journal(ESP_ERR_INVALID_RESPONSE);
    }

    const int trailing = fgetc(journal);
    if (trailing != EOF || ferror(journal) != 0)
    {
        return close_journal(ESP_ERR_INVALID_RESPONSE);
    }

    web_file_transaction_t transaction{};
    if (!web_file_transaction_parse_phase(phase_line + sizeof("phase=") - 1U, &transaction.phase)
        || !web_file_transaction_parse_length(length_line + sizeof("length=") - 1U, &transaction.expected_length)
        || transaction.expected_length > WEB_FILE_UPLOAD_MAX_SIZE_BYTES)
    {
        return close_journal(ESP_ERR_INVALID_RESPONSE);
    }

    const char  *target      = target_line + sizeof("target=") - 1U;
    const size_t target_size = strlen(target);
    if (target_size == 0U || target_size >= sizeof(transaction.target_path))
    {
        return close_journal(ESP_ERR_INVALID_RESPONSE);
    }
    memcpy(transaction.target_path, target, target_size + 1U);

    char target_filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    if (strcmp(transaction.target_path, "/") == 0
        || web_file_path_map_logical(transaction.target_path, target_filesystem, sizeof(target_filesystem)) != ESP_OK)
    {
        return close_journal(ESP_ERR_INVALID_RESPONSE);
    }

    *out_transaction = transaction;
    return close_journal(ESP_OK);
}

/**
 * @brief 检查固定事务产物存在性并拒绝未知名称
 *
 * @param[out] out_artifacts 固定产物存在性
 * @return ESP_OK 事务目录有效；ESP_ERR_NOT_FOUND 目录不存在；
 *         ESP_ERR_INVALID_RESPONSE 类型或名称冲突；其他错误表示目录读取失败
 */
static esp_err_t web_file_transaction_inspect_directory(web_file_transaction_artifacts_t *out_artifacts)
{
    struct stat directory_info;
    if (stat(WEB_FILE_TRANSACTION_DIR, &directory_info) != 0)
    {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISDIR(directory_info.st_mode))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    DIR *directory = opendir(WEB_FILE_TRANSACTION_DIR);
    if (directory == NULL)
    {
        return ESP_FAIL;
    }

    web_file_transaction_artifacts_t artifacts{};
    esp_err_t                        error     = ESP_OK;
    for (;;)
    {
        errno                = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                error = ESP_FAIL;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        if (strcmp(entry->d_name, "transaction") == 0)
        {
            artifacts.journal_exists = true;
        }
        else if (strcmp(entry->d_name, "transaction.new") == 0)
        {
            artifacts.new_exists = true;
        }
        else if (strcmp(entry->d_name, "upload.part") == 0)
        {
            artifacts.part_exists = true;
        }
        else if (strcmp(entry->d_name, "upload.bak") == 0)
        {
            artifacts.backup_exists = true;
        }
        else
        {
            error = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (closedir(directory) != 0 && error == ESP_OK)
    {
        error = ESP_FAIL;
    }
    if (error != ESP_OK)
    {
        return error;
    }

    *out_artifacts = artifacts;
    return ESP_OK;
}

/**
 * @brief 校验事务文件是常规文件并读取长度
 *
 * @param[in] path 固定事务文件或目标文件
 * @param[out] out_exists 文件是否存在
 * @param[out] out_length 文件存在时的非负长度
 * @return ESP_OK 不存在或常规文件有效；ESP_ERR_INVALID_RESPONSE 类型非法；
 *         其他错误表示文件系统失败
 */
static esp_err_t web_file_transaction_stat_regular(const char *path, bool *out_exists, uint64_t *out_length)
{
    struct stat info;
    if (stat(path, &info) != 0)
    {
        if (errno == ENOENT)
        {
            *out_exists = false;
            *out_length = 0U;
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_exists = true;
    *out_length = (uint64_t) info.st_size;
    return ESP_OK;
}

web_file_recovery_action_t web_file_transaction_decide_recovery(web_file_transaction_phase_t phase, bool target_exists,
                                                                bool backup_exists, bool part_exists,
                                                                bool target_matches_expected_length)
{
    switch (phase)
    {
        case WEB_FILE_TRANSACTION_PREPARED:
            if (target_exists && !backup_exists)
            {
                return WEB_FILE_RECOVERY_REMOVE_PART;
            }
            if (!target_exists && backup_exists)
            {
                return WEB_FILE_RECOVERY_RESTORE_BACKUP;
            }
            break;
        case WEB_FILE_TRANSACTION_BACKUP_MOVED:
            if (!target_exists && backup_exists)
            {
                return WEB_FILE_RECOVERY_RESTORE_BACKUP;
            }
            if (target_exists && backup_exists && !part_exists && target_matches_expected_length)
            {
                return WEB_FILE_RECOVERY_ACCEPT_COMMIT;
            }
            break;
        case WEB_FILE_TRANSACTION_TARGET_COMMITTED:
            if (target_exists && !part_exists && target_matches_expected_length)
            {
                return WEB_FILE_RECOVERY_ACCEPT_COMMIT;
            }
            if (!target_exists && backup_exists && !part_exists)
            {
                return WEB_FILE_RECOVERY_RESTORE_BACKUP;
            }
            break;
        default:
            break;
    }
    return WEB_FILE_RECOVERY_AMBIGUOUS;
}

/**
 * @brief 删除固定事务产物，不把“不存在”视为错误
 *
 * @param[in] path Service 自有固定路径
 * @return ESP_OK 已不存在；其他错误表示删除失败
 */
static esp_err_t web_file_transaction_remove_owned(const char *path)
{
    return remove(path) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 删除两个 journal 并在目录为空时移除事务目录
 *
 * @return ESP_OK 元数据已清理；其他错误表示可重试的清理失败
 */
static esp_err_t web_file_transaction_remove_metadata(void)
{
    if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_JOURNAL) != ESP_OK
        || web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_NEW) != ESP_OK)
    {
        return ESP_FAIL;
    }
    return rmdir(WEB_FILE_TRANSACTION_DIR) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 校验两个同时存在的 journal 属于同一相邻阶段事务
 *
 * FAT 的 rename 不覆盖现有名称，阶段切换会先同步 `transaction.new` 再移除旧 journal。
 * 断电可留下两份记录；只有目标、长度相同且新记录阶段相同或只前进一级时才可继续选择恢复记录。
 *
 * @param[in] current 当前 journal
 * @param[in] next transaction.new
 * @return true 两份记录一致；false 状态冲突
 */
static bool web_file_transaction_journals_are_consistent(const web_file_transaction_t *current,
                                                         const web_file_transaction_t *next)
{
    const int current_phase = (int) current->phase;
    const int next_phase    = (int) next->phase;
    return current->expected_length == next->expected_length && strcmp(current->target_path, next->target_path) == 0
           && next_phase >= current_phase && next_phase <= current_phase + 1;
}

esp_err_t web_file_transaction_recover(void)
{
    web_file_transaction_artifacts_t artifacts;
    esp_err_t                        error = web_file_transaction_inspect_directory(&artifacts);
    if (error == ESP_ERR_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "上传事务目录状态非法，拒绝启动");
        return error;
    }

    if (!artifacts.journal_exists && !artifacts.new_exists)
    {
        if (artifacts.backup_exists)
        {
            ESP_LOGE(TAG, "上传事务缺少 journal 且存在备份，拒绝猜测恢复");
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (artifacts.part_exists)
        {
            bool     part_exists;
            uint64_t part_length;
            error = web_file_transaction_stat_regular(WEB_FILE_TRANSACTION_PART, &part_exists, &part_length);
            if (error != ESP_OK || !part_exists)
            {
                ESP_LOGE(TAG, "孤立上传临时产物不是常规文件，拒绝删除");
                return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
            }
            if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_PART) != ESP_OK)
            {
                return ESP_FAIL;
            }
        }
        return web_file_transaction_remove_metadata();
    }

    if (!artifacts.journal_exists && artifacts.new_exists && !artifacts.part_exists && !artifacts.backup_exists)
    {
        bool     new_exists;
        uint64_t new_length;
        error = web_file_transaction_stat_regular(WEB_FILE_TRANSACTION_NEW, &new_exists, &new_length);
        if (error != ESP_OK || !new_exists)
        {
            ESP_LOGE(TAG, "孤立上传新 journal 不是常规文件，拒绝删除");
            return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
        }
        if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_NEW) != ESP_OK)
        {
            return ESP_FAIL;
        }
        return web_file_transaction_remove_metadata();
    }

    web_file_transaction_t transaction;
    const char *journal_path = artifacts.journal_exists ? WEB_FILE_TRANSACTION_JOURNAL : WEB_FILE_TRANSACTION_NEW;
    error                    = web_file_transaction_read_journal_at(journal_path, &transaction);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "上传事务 journal 格式非法，拒绝启动");
        return error;
    }
    if (artifacts.journal_exists && artifacts.new_exists)
    {
        web_file_transaction_t next_transaction;
        error = web_file_transaction_read_journal_at(WEB_FILE_TRANSACTION_NEW, &next_transaction);
        if (error != ESP_OK || !web_file_transaction_journals_are_consistent(&transaction, &next_transaction))
        {
            ESP_LOGE(TAG, "上传事务双 journal 状态冲突，拒绝启动");
            return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
        }
        /*
         * transaction 是已经发布的旧阶段，transaction.new 是已同步的同一事务下一阶段。
         * PREPARED -> BACKUP_MOVED 仍使用旧记录做保守回滚；BACKUP_MOVED ->
         * TARGET_COMMITTED 必须使用已验证的后继记录，使恢复删除备份后再次掉电仍可幂等接受
         * 已精确提交的目标。相同阶段记录等价，保留旧记录即可。
         */
        if (transaction.phase == WEB_FILE_TRANSACTION_BACKUP_MOVED
            && next_transaction.phase == WEB_FILE_TRANSACTION_TARGET_COMMITTED)
        {
            transaction = next_transaction;
        }
    }

    uint64_t ignored_length;
    bool     part_exists;
    bool     backup_exists;
    if (web_file_transaction_stat_regular(WEB_FILE_TRANSACTION_PART, &part_exists, &ignored_length) != ESP_OK
        || web_file_transaction_stat_regular(WEB_FILE_TRANSACTION_BACKUP, &backup_exists, &ignored_length) != ESP_OK
        || part_exists != artifacts.part_exists || backup_exists != artifacts.backup_exists)
    {
        ESP_LOGE(TAG, "上传事务产物类型或存在性冲突，拒绝启动");
        return ESP_ERR_INVALID_RESPONSE;
    }

    char target_filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    if (web_file_path_map_logical(transaction.target_path, target_filesystem, sizeof(target_filesystem)) != ESP_OK)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool     target_exists;
    uint64_t target_length;
    error = web_file_transaction_stat_regular(target_filesystem, &target_exists, &target_length);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "上传事务目标类型非法，拒绝启动");
        return error;
    }

    const web_file_recovery_action_t action =
        web_file_transaction_decide_recovery(transaction.phase,
                                             target_exists,
                                             backup_exists,
                                             part_exists,
                                             target_exists && target_length == transaction.expected_length);
    if (action == WEB_FILE_RECOVERY_AMBIGUOUS)
    {
        ESP_LOGE(TAG, "上传事务状态无法唯一恢复，拒绝启动");
        return ESP_ERR_INVALID_RESPONSE;
    }

    switch (action)
    {
        case WEB_FILE_RECOVERY_REMOVE_PART:
            if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_PART) != ESP_OK)
            {
                return ESP_FAIL;
            }
            error = web_file_transaction_remove_metadata();
            if (error == ESP_OK)
            {
                ESP_LOGW(TAG, "已清理未提交的上传事务");
            }
            return error;

        case WEB_FILE_RECOVERY_RESTORE_BACKUP:
            if (rename(WEB_FILE_TRANSACTION_BACKUP, target_filesystem) != 0)
            {
                ESP_LOGE(TAG, "恢复上传事务备份失败");
                return ESP_FAIL;
            }
            if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_PART) != ESP_OK)
            {
                return ESP_FAIL;
            }
            error = web_file_transaction_remove_metadata();
            if (error == ESP_OK)
            {
                ESP_LOGW(TAG, "已恢复上传覆盖事务的原文件");
            }
            return error;

        case WEB_FILE_RECOVERY_ACCEPT_COMMIT:
            if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_BACKUP) != ESP_OK
                || web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_PART) != ESP_OK)
            {
                return ESP_FAIL;
            }
            error = web_file_transaction_remove_metadata();
            if (error == ESP_OK)
            {
                ESP_LOGW(TAG, "已确认并清理提交完成的上传事务");
            }
            return error;

        case WEB_FILE_RECOVERY_AMBIGUOUS:
        default:
            return ESP_ERR_INVALID_RESPONSE;
    }
}

esp_err_t web_file_upload_validate_length(size_t content_length)
{
    return content_length <= WEB_FILE_UPLOAD_MAX_SIZE_BYTES ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t web_file_transaction_prepare_upload(void)
{
    if (mkdir(WEB_FILE_TRANSACTION_DIR, 0700) == 0)
    {
        return ESP_OK;
    }
    if (errno != EEXIST)
    {
        return ESP_FAIL;
    }

    web_file_transaction_artifacts_t artifacts;
    const esp_err_t                  error = web_file_transaction_inspect_directory(&artifacts);
    if (error != ESP_OK || artifacts.journal_exists || artifacts.new_exists || artifacts.part_exists
        || artifacts.backup_exists)
    {
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    return ESP_OK;
}

esp_err_t web_file_transaction_abort_upload(void)
{
    web_file_transaction_artifacts_t artifacts;
    esp_err_t                        error = web_file_transaction_inspect_directory(&artifacts);
    if (error == ESP_ERR_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (error != ESP_OK || artifacts.journal_exists || artifacts.new_exists || artifacts.backup_exists)
    {
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    if (artifacts.part_exists && web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_PART) != ESP_OK)
    {
        return ESP_FAIL;
    }
    return rmdir(WEB_FILE_TRANSACTION_DIR) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 检查停止流程是否已经取消提交
 *
 * @return true Service 不再接纳请求；false 当前提交可继续
 */
static bool web_file_transaction_is_cancelled(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool cancelled = !s_context.accepting_requests || s_context.state != WEB_FILE_SERVICE_STATE_RUNNING;
    xSemaphoreGive(s_context.lock);
    return cancelled;
}

esp_err_t web_file_transaction_commit_new(const web_file_transaction_t *transaction)
{
    if (transaction == NULL || transaction->phase != WEB_FILE_TRANSACTION_PREPARED
        || transaction->expected_length > WEB_FILE_UPLOAD_MAX_SIZE_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char target_filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    if (web_file_path_map_logical(transaction->target_path, target_filesystem, sizeof(target_filesystem)) != ESP_OK
        || strcmp(transaction->target_path, "/") == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    web_file_transaction_artifacts_t artifacts;
    if (web_file_transaction_inspect_directory(&artifacts) != ESP_OK || !artifacts.part_exists
        || artifacts.backup_exists || artifacts.journal_exists || artifacts.new_exists)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool     part_exists;
    uint64_t part_length;
    bool     target_exists;
    uint64_t target_length;
    if (web_file_transaction_stat_regular(WEB_FILE_TRANSACTION_PART, &part_exists, &part_length) != ESP_OK
        || !part_exists || part_length != transaction->expected_length
        || web_file_transaction_stat_regular(target_filesystem, &target_exists, &target_length) != ESP_OK
        || target_exists)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (web_file_transaction_is_cancelled())
    {
        const esp_err_t cleanup_error = web_file_transaction_abort_upload();
        return cleanup_error == ESP_OK ? ESP_ERR_INVALID_STATE : cleanup_error;
    }

    if (rename(WEB_FILE_TRANSACTION_PART, target_filesystem) != 0)
    {
        const esp_err_t cleanup_error = web_file_transaction_abort_upload();
        return cleanup_error == ESP_OK ? ESP_FAIL : cleanup_error;
    }

    bool      committed_exists;
    uint64_t  committed_length;
    esp_err_t error = web_file_transaction_stat_regular(target_filesystem, &committed_exists, &committed_length);
    if (error != ESP_OK || !committed_exists || committed_length != transaction->expected_length)
    {
        if (rename(target_filesystem, WEB_FILE_TRANSACTION_PART) != 0)
        {
            ESP_LOGE(TAG, "新文件上传长度复核失败，且无法收回临时文件");
            return ESP_FAIL;
        }
        const esp_err_t cleanup_error = web_file_transaction_abort_upload();
        return cleanup_error == ESP_OK ? ESP_FAIL : cleanup_error;
    }

    const bool cancelled = web_file_transaction_is_cancelled();
    if (rmdir(WEB_FILE_TRANSACTION_DIR) != 0 && errno != ENOENT)
    {
        return ESP_FAIL;
    }
    return cancelled || web_file_transaction_is_cancelled() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/**
 * @brief 提交失败后立即执行同一恢复内核
 *
 * @param[in] primary_error 提交主错误
 * @return 恢复成功时保留主错误；恢复失败时返回恢复错误
 */
static esp_err_t web_file_transaction_recover_failure(esp_err_t primary_error)
{
    const esp_err_t recovery_error = web_file_transaction_recover();
    if (recovery_error != ESP_OK)
    {
        ESP_LOGE(TAG, "上传提交失败且未能立即恢复，已保留事务产物");
        return recovery_error;
    }
    return primary_error;
}

/**
 * @brief 核对覆盖提交前固定产物和目标文件仍与预检一致
 *
 * @param[in] transaction 待提交事务
 * @param[out] out_target_filesystem 最终物理路径
 * @return ESP_OK 状态一致；其他错误表示竞态或文件系统异常
 */
static esp_err_t
    web_file_transaction_validate_commit_input(const web_file_transaction_t *transaction,
                                               char out_target_filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE])
{
    if (transaction == NULL || transaction->phase != WEB_FILE_TRANSACTION_PREPARED
        || transaction->expected_length > WEB_FILE_UPLOAD_MAX_SIZE_BYTES
        || web_file_path_map_logical(transaction->target_path,
                                     out_target_filesystem,
                                     WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE)
               != ESP_OK
        || strcmp(transaction->target_path, "/") == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    web_file_transaction_artifacts_t artifacts;
    if (web_file_transaction_inspect_directory(&artifacts) != ESP_OK || !artifacts.part_exists
        || artifacts.backup_exists || artifacts.journal_exists || artifacts.new_exists)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool     part_exists;
    uint64_t part_length;
    bool     target_exists;
    uint64_t target_length;
    if (web_file_transaction_stat_regular(WEB_FILE_TRANSACTION_PART, &part_exists, &part_length) != ESP_OK
        || !part_exists || part_length != transaction->expected_length
        || web_file_transaction_stat_regular(out_target_filesystem, &target_exists, &target_length) != ESP_OK
        || !target_exists)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t web_file_transaction_commit(const web_file_transaction_t *transaction)
{
    char      target_filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    esp_err_t error = web_file_transaction_validate_commit_input(transaction, target_filesystem);
    if (error != ESP_OK)
    {
        return error;
    }
    if (web_file_transaction_is_cancelled())
    {
        return ESP_ERR_INVALID_STATE;
    }

    web_file_transaction_t durable = *transaction;
    error                          = web_file_transaction_write_journal(&durable);
    if (error != ESP_OK)
    {
        return error;
    }
    if (web_file_transaction_is_cancelled())
    {
        return web_file_transaction_recover_failure(ESP_ERR_INVALID_STATE);
    }

    if (rename(target_filesystem, WEB_FILE_TRANSACTION_BACKUP) != 0)
    {
        return web_file_transaction_recover_failure(ESP_FAIL);
    }
    durable.phase = WEB_FILE_TRANSACTION_BACKUP_MOVED;
    error         = web_file_transaction_write_journal(&durable);
    if (error != ESP_OK)
    {
        return web_file_transaction_recover_failure(error);
    }
    if (web_file_transaction_is_cancelled())
    {
        return web_file_transaction_recover_failure(ESP_ERR_INVALID_STATE);
    }

    if (rename(WEB_FILE_TRANSACTION_PART, target_filesystem) != 0)
    {
        if (rename(WEB_FILE_TRANSACTION_BACKUP, target_filesystem) != 0)
        {
            ESP_LOGE(TAG, "上传新文件提交失败，且旧文件即时恢复失败");
            return ESP_FAIL;
        }
        return web_file_transaction_recover_failure(ESP_FAIL);
    }

    bool     committed_exists;
    uint64_t committed_length;
    error = web_file_transaction_stat_regular(target_filesystem, &committed_exists, &committed_length);
    if (error != ESP_OK || !committed_exists || committed_length != transaction->expected_length)
    {
        if (rename(target_filesystem, WEB_FILE_TRANSACTION_PART) == 0
            && rename(WEB_FILE_TRANSACTION_BACKUP, target_filesystem) == 0)
        {
            return web_file_transaction_recover_failure(ESP_FAIL);
        }
        ESP_LOGE(TAG, "上传目标长度复核失败，且无法立即恢复旧文件");
        return ESP_FAIL;
    }

    bool cancelled_after_commit = web_file_transaction_is_cancelled();
    durable.phase               = WEB_FILE_TRANSACTION_TARGET_COMMITTED;
    error                       = web_file_transaction_write_journal(&durable);
    if (error != ESP_OK)
    {
        return web_file_transaction_recover_failure(error);
    }
    if (web_file_transaction_is_cancelled())
    {
        cancelled_after_commit = true;
    }

    if (web_file_transaction_remove_owned(WEB_FILE_TRANSACTION_BACKUP) != ESP_OK)
    {
        return ESP_FAIL;
    }
    error = web_file_transaction_remove_metadata();
    if (error != ESP_OK)
    {
        return error;
    }
    return cancelled_after_commit || web_file_transaction_is_cancelled() ? ESP_ERR_INVALID_STATE : ESP_OK;
}
