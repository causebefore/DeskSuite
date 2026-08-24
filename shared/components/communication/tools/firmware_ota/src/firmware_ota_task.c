/**
 * @file firmware_ota_task.c
 * @brief 独立应用固件 OTA Task 与不可取消写入事务
 */
#include "firmware_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "firmware_ota_build.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "transport_http.h"
#include "utils.h"

#define FIRMWARE_OTA_URL_MAX               384U
#define FIRMWARE_OTA_RESPONSE_MAX          4096U
#define FIRMWARE_OTA_TASK_STACK_SIZE       10240U
#define FIRMWARE_OTA_TASK_PRIORITY         3U
/** @brief 停止或等待事务时附加在检查、下载超时之外的宽限时间（毫秒） */
#define FIRMWARE_OTA_STOP_GRACE_MS         30000U
#define FIRMWARE_OTA_DOWNLOAD_BUFFER_BYTES 4096U
#define FIRMWARE_OTA_SHA256_BYTES          32U
/** @brief 2^53-1；cJSON 以 double 存储数值，超过该值会丢失精度，用于约束
 * ota_version 上限 */
#define FIRMWARE_OTA_JSON_INTEGER_MAX      9007199254740991.0
#define FIRMWARE_OTA_CHECK_PATH            "/api/v1/ota/check"

static const char *TAG = "firmware_ota";

/** @brief 投递到 OTA Task 命令队列的指令 */
typedef enum
{
    FIRMWARE_OTA_COMMAND_CHECK = 0, /**< 查询并缓存一次可安装目标 */
    FIRMWARE_OTA_COMMAND_INSTALL,   /**< 安装已经缓存的不可变目标 */
    FIRMWARE_OTA_COMMAND_STOP,      /**< 请求 Task 在当前事务结束后自删除并退出 */
} firmware_ota_command_t;

/**
 * @brief 从检查响应解析并校验通过的目标固件描述
 *
 * 全部字段已经过格式与单调性校验，可直接用于下载、校验与去重判断。
 */
typedef struct
{
    char     version[FIRMWARE_OTA_VERSION_MAX];          /**< 目标诊断版本字符串 */
    uint64_t ota_version;                                /**< 目标单调 OTA 版本 */
    char     artifact_id[FIRMWARE_OTA_ARTIFACT_ID_SIZE]; /**< 目标镜像 Validation
                                                      SHA-256（十六进制） */
    char     file_sha256[FIRMWARE_OTA_ARTIFACT_ID_SIZE]; /**< 目标固件文件
                                                      SHA-256（十六进制） */
    char     url[FIRMWARE_OTA_URL_MAX];                  /**< 相对服务端根的下载路径，以 '/' 开头 */
    size_t   size;                                       /**< 目标固件字节数 */
} firmware_ota_target_t;

/**
 * @brief OTA 工具运行时单例状态
 *
 * 汇总生命周期标志、Task 与同步原语、服务连接配置、完成回调和待安装目标。
 * 除 OTA Task 内部外，所有共享字段访问都应通过 mutex 保护；task_stopped 仅供
 * 同步 stop 等待 Task 退出。检查与安装结果通过 OTA Task
 * 上下文的完成回调异步返回。
 */
typedef struct
{
    bool                       initialized;            /**< 已 init 且未 deinit */
    bool                       started;                /**< Task 已启动且尚未停止 */
    bool                       stopping;               /**< STOP 已提交，拒绝新的控制请求 */
    bool                       configured;             /**< 已配置有效的服务连接信息 */
    firmware_ota_state_t       state;                  /**< Task 当前状态机阶段 */
    QueueHandle_t              commands;               /**< 投递检查、安装和停止命令的队列 */
    SemaphoreHandle_t          mutex;                  /**< 保护本结构体共享字段的互斥量 */
    SemaphoreHandle_t          task_stopped;           /**< Task 自删除完成时释放的二值信号量 */
    TaskHandle_t               task;                   /**< OTA Task 句柄，停止后置空 */
    protocol_backend_context_t backend;                /**< 后端连接与身份上下文 */
    int                        check_timeout_ms;       /**< 检查接口 HTTP 超时（毫秒） */
    int                        download_timeout_ms;    /**< 固件下载 HTTP 超时（毫秒） */
    firmware_ota_event_cb_t    event_callback;         /**< 事务完成回调，长期借用至替换或 deinit */
    void                      *event_context;          /**< 事务完成回调借用上下文 */
    bool                       event_callback_running; /**< OTA Task 正在执行完成回调 */
    firmware_ota_target_t      pending_target;         /**< 检查成功后缓存的不可变安装目标 */
} firmware_ota_runtime_t;

/**
 * @brief 单次下载事务的累积上下文
 *
 * 在 transport_http 下载回调与主事务之间共享：回调据此把数据写入 OTA
 * 分区并更新摘要， 主事务在下载结束后用 received_bytes 与摘要完成最终校验。
 */
typedef struct
{
    esp_ota_handle_t     ota_handle;     /**< esp_ota_begin 返回的写入句柄，0 表示未开始或已结束 */
    size_t               received_bytes; /**< 已写入备用分区的字节数 */
    size_t               expected_size;  /**< 清单声明的目标固件字节数 */
    mbedtls_md_context_t digest;         /**< 增量计算的文件 SHA-256 上下文 */
} firmware_ota_download_context_t;

static firmware_ota_runtime_t s_runtime;

/** @brief 占用运行时互斥量，阻塞等待直到获取成功 */
static void firmware_ota_lock(void)
{
    (void) xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
}

/** @brief 释放运行时互斥量 */
static void firmware_ota_unlock(void)
{
    (void) xSemaphoreGive(s_runtime.mutex);
}

/** @brief 把 32 字节摘要格式化为小写十六进制字符串 */
static void firmware_ota_format_sha256(const uint8_t digest[FIRMWARE_OTA_SHA256_BYTES],
                                       char          out[FIRMWARE_OTA_ARTIFACT_ID_SIZE])
{
    for (size_t index = 0U; index < FIRMWARE_OTA_SHA256_BYTES; ++index)
    {
        (void) snprintf(&out[index * 2U], 3U, "%02x", digest[index]);
    }
}

/** @brief 判断字符串是否为 64 位小写十六进制 SHA-256 摘要 */
static bool firmware_ota_is_sha256(const char *value)
{
    if (value == NULL || strlen(value) != FIRMWARE_OTA_ARTIFACT_ID_SIZE - 1U)
    {
        return false;
    }
    for (size_t index = 0U; index < FIRMWARE_OTA_ARTIFACT_ID_SIZE - 1U; ++index)
    {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 读取指定应用分区的 Validation SHA-256 并格式化为十六进制字符串
 *
 * @param[in] partition 目标应用分区
 * @param[out] out 输出缓冲区，容量为 FIRMWARE_OTA_ARTIFACT_ID_SIZE
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 分区为空；或分区摘要读取错误码
 */
static esp_err_t firmware_ota_partition_identity(const esp_partition_t *partition,
                                                 char                   out[FIRMWARE_OTA_ARTIFACT_ID_SIZE])
{
    uint8_t digest[FIRMWARE_OTA_SHA256_BYTES];
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "应用分区不存在");
    ESP_RETURN_ON_ERROR(esp_partition_get_sha256(partition, digest), TAG, "读取应用镜像 Validation SHA-256 失败");
    firmware_ota_format_sha256(digest, out);
    return ESP_OK;
}

/**
 * @brief 读取当前固件身份并复制到调用方缓冲区
 *
 * 实现要点：从应用描述读取版本，从运行分区读取 Validation SHA-256；
 * 上次无效分区读取失败时仅记录告警并省略该字段，不阻断身份读取。
 */
esp_err_t firmware_ota_get_identity_copy(firmware_ota_identity_t *out_identity)
{
    ESP_RETURN_ON_FALSE(out_identity != NULL, ESP_ERR_INVALID_ARG, TAG, "固件身份输出指针为空");
    memset(out_identity, 0, sizeof(*out_identity));
    const esp_app_desc_t *description = esp_app_get_description();
    ESP_RETURN_ON_FALSE(description != NULL, ESP_ERR_INVALID_STATE, TAG, "读取应用描述失败");
    utils_copy_string(out_identity->current_version, sizeof(out_identity->current_version), description->version);
    out_identity->current_ota_version = FIRMWARE_OTA_BUILD_VERSION;
    ESP_RETURN_ON_ERROR(
        firmware_ota_partition_identity(esp_ota_get_running_partition(), out_identity->current_artifact_id),
        TAG,
        "读取当前固件身份失败");
    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
    if (last_invalid != NULL)
    {
        const esp_err_t error = firmware_ota_partition_identity(last_invalid, out_identity->last_invalid_artifact_id);
        if (error == ESP_OK)
        {
            out_identity->has_last_invalid_artifact = true;
        }
        else
        {
            ESP_LOGW(TAG, "读取上次无效固件身份失败，本轮不携带该字段: %s", esp_err_to_name(error));
            out_identity->last_invalid_artifact_id[0] = '\0';
        }
    }
    return ESP_OK;
}

/**
 * @brief 构造 OTA 检查请求的 JSON 请求体
 *
 * @param[in] identity 当前固件身份快照
 * @param[out] out_body 成功时输出 cJSON 分配的字符串，调用方需用 cJSON_free
 * 释放
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 内存分配失败
 */
static esp_err_t firmware_ota_make_request_body(const firmware_ota_identity_t *identity, char **out_body)
{
    cJSON *root      = cJSON_CreateObject();
    cJSON *artifacts = cJSON_CreateObject();
    cJSON *app       = cJSON_CreateObject();
    if (root == NULL || artifacts == NULL || app == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(artifacts);
        cJSON_Delete(app);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "protocol_version", 2);
    cJSON_AddNumberToObject(root, "product_id", (double) s_runtime.backend.product_id);
    cJSON_AddStringToObject(root, "firmware_target", s_runtime.backend.firmware_target);
    cJSON_AddStringToObject(root, "device_id", s_runtime.backend.device_id);
    cJSON_AddStringToObject(app, "current_version", identity->current_version);
    cJSON_AddNumberToObject(app, "ota_version", (double) identity->current_ota_version);
    cJSON_AddStringToObject(app, "current_artifact_id", identity->current_artifact_id);
    if (identity->has_last_invalid_artifact)
    {
        cJSON_AddStringToObject(app, "last_invalid_artifact_id", identity->last_invalid_artifact_id);
    }
    else
    {
        cJSON_AddNullToObject(app, "last_invalid_artifact_id");
    }
    cJSON_AddItemToObject(artifacts, "app", app);
    cJSON_AddItemToObject(root, "artifacts", artifacts);
    *out_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_body != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * @brief 从 JSON 对象拷贝非空字符串字段到定长缓冲区
 *
 * 字段缺失、非字符串、空串或长度溢出缓冲区均视为非法响应。
 *
 * @param[in] object JSON 对象
 * @param[in] name 字段名
 * @param[out] out 目标缓冲区
 * @param[in] capacity 缓冲区容量
 * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 字段缺失或非法
 */
static esp_err_t firmware_ota_copy_json_string(const cJSON *object, const char *name, char *out, size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0'
        || strlen(item->valuestring) >= capacity)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    utils_copy_string(out, capacity, item->valuestring);
    return ESP_OK;
}

/**
 * @brief 解析并严格校验 OTA 检查响应中的目标固件
 *
 * 校验规则：protocol_version 必须为 2、updates 必须为对象；app 子对象存在时，
 * size 与 ota_version 必须是合法正数（ota_version 还需落在 double
 * 精度安全范围内， 即不超过 FIRMWARE_OTA_JSON_INTEGER_MAX 且可无损转回
 * uint64_t）， version/artifact_id/file_sha256/url
 * 必须为非空字符串且不溢出缓冲区， artifact_id 与 file_sha256 必须是合法
 * SHA-256，url 必须以 '/' 开头。 任何一项不满足都按非法响应处理。
 *
 * @param[in] body 响应体字符串
 * @param[out] out_target 解析成功时输出目标描述
 * @param[out] out_has_update 是否存在合法的可安装目标
 * @return ESP_OK 解析成功；ESP_ERR_INVALID_RESPONSE 响应非法
 */
static esp_err_t firmware_ota_parse_target(const char *body, firmware_ota_target_t *out_target, bool *out_has_update)
{
    *out_has_update = false;
    memset(out_target, 0, sizeof(*out_target));
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "OTA 检查响应不是合法 JSON");
    const cJSON *protocol_version = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *updates          = cJSON_GetObjectItemCaseSensitive(root, "updates");
    const cJSON *app              = cJSON_IsObject(updates) ? cJSON_GetObjectItemCaseSensitive(updates, "app") : NULL;
    esp_err_t    error            = ESP_OK;
    if (!cJSON_IsNumber(protocol_version) || protocol_version->valueint != 2 || !cJSON_IsObject(updates))
    {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    else if (app == NULL)
    {
        *out_has_update = false;
    }
    else if (!cJSON_IsObject(app))
    {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    else
    {
        const cJSON *size              = cJSON_GetObjectItemCaseSensitive(app, "size");
        const cJSON *ota_version       = cJSON_GetObjectItemCaseSensitive(app, "ota_version");
        const double ota_version_value = cJSON_IsNumber(ota_version) ? ota_version->valuedouble : 0.0;
        if (!cJSON_IsNumber(size) || size->valuedouble <= 0 || size->valuedouble > (double) SIZE_MAX
            || !cJSON_IsNumber(ota_version) || ota_version_value <= 0.0
            || ota_version_value > FIRMWARE_OTA_JSON_INTEGER_MAX
            || (double) (uint64_t) ota_version_value != ota_version_value
            || firmware_ota_copy_json_string(app, "version", out_target->version, sizeof(out_target->version)) != ESP_OK
            || firmware_ota_copy_json_string(app,
                                             "artifact_id",
                                             out_target->artifact_id,
                                             sizeof(out_target->artifact_id))
                   != ESP_OK
            || firmware_ota_copy_json_string(app,
                                             "file_sha256",
                                             out_target->file_sha256,
                                             sizeof(out_target->file_sha256))
                   != ESP_OK
            || firmware_ota_copy_json_string(app, "url", out_target->url, sizeof(out_target->url)) != ESP_OK
            || !firmware_ota_is_sha256(out_target->artifact_id) || !firmware_ota_is_sha256(out_target->file_sha256)
            || out_target->url[0] != '/')
        {
            error = ESP_ERR_INVALID_RESPONSE;
        }
        else
        {
            out_target->size        = (size_t) size->valuedouble;
            out_target->ota_version = (uint64_t) ota_version_value;
            *out_has_update         = true;
        }
    }
    cJSON_Delete(root);
    return error;
}

/**
 * @brief 填充 HTTP 请求头数组
 *
 * 依序写入可选 Content-Type，以及统一后端上下文中的 Bearer Token 和稳定设备 ID。
 * bearer 缓冲区用于承载拼接后的 Authorization 值，其生命周期需覆盖后续 HTTP 请求。
 *
 * @param[out] headers 头数组输出
 * @param[out] bearer 承载 "Bearer <token>" 的临时缓冲区
 * @param[in] include_content_type 是否写入 Content-Type 头
 * @return 实际写入的头数量
 */
static size_t firmware_ota_make_headers(transport_http_header_t headers[3],
                                        char                    bearer[PROTOCOL_BACKEND_TOKEN_MAX + sizeof("Bearer ")],
                                        bool                    include_content_type)
{
    size_t count = 0U;
    if (include_content_type)
    {
        headers[count++] = (transport_http_header_t) {
            .name  = "Content-Type",
            .value = "application/json",
        };
    }
    protocol_identity_add_headers(headers,
                                  &count,
                                  s_runtime.backend.token,
                                  s_runtime.backend.device_id,
                                  bearer,
                                  PROTOCOL_BACKEND_TOKEN_MAX + sizeof("Bearer "));
    return count;
}

/**
 * @brief 向服务端发起 OTA 检查请求并解析目标固件
 *
 * @param[in] identity 当前固件身份快照
 * @param[out] out_target 解析得到的目标描述
 * @param[out] out_has_update 是否存在可安装目标
 * @return ESP_OK 请求与解析成功（不论是否返回更新）；其他值表示网络或响应错误
 */
static esp_err_t firmware_ota_check_target(const firmware_ota_identity_t *identity, firmware_ota_target_t *out_target,
                                           bool *out_has_update)
{
    char url[FIRMWARE_OTA_URL_MAX];
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), s_runtime.backend.base_url, FIRMWARE_OTA_CHECK_PATH),
                        TAG,
                        "构造 OTA 检查地址失败");
    char *body = NULL;
    ESP_RETURN_ON_ERROR(firmware_ota_make_request_body(identity, &body), TAG, "创建 OTA 检查请求失败");
    transport_http_header_t        headers[3];
    char                           bearer[PROTOCOL_BACKEND_TOKEN_MAX + sizeof("Bearer ")] = { 0 };
    const size_t                   header_count = firmware_ota_make_headers(headers, bearer, true);
    const transport_http_request_t request      = {
        .url                  = url,
        .method               = TRANSPORT_HTTP_POST,
        .headers              = headers,
        .header_count         = header_count,
        .body                 = body,
        .body_len             = strlen(body),
        .timeout_ms           = s_runtime.check_timeout_ms,
        .max_response_bytes   = FIRMWARE_OTA_RESPONSE_MAX,
        .suppress_success_log = true,
    };
    transport_http_response_t response = { 0 };
    esp_err_t                 error    = transport_http_perform_borrow(&request, &response);
    cJSON_free(body);
    if (error == ESP_OK)
    {
        error = firmware_ota_parse_target(response.body, out_target, out_has_update);
    }
    transport_http_response_release(&response);
    return error;
}

/**
 * @brief 下载回调：把分片数据写入 OTA 分区并更新文件摘要
 *
 * 由 transport_http 在流式下载数据时回调，运行在下载调用方的上下文中。
 * 累计写入量不得超过清单声明的 expected_size，否则视为数据超限并中止下载。
 *
 * @param[in] data 本次接收的数据
 * @param[in] length 数据长度
 * @param[in,out] context firmware_ota_download_context_t
 * 指针，累积写入句柄与摘要
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_SIZE
 * 超出清单大小； 其他值表示 OTA 写入或摘要更新失败
 */
static esp_err_t firmware_ota_on_download_data(const uint8_t *data, size_t length, void *context)
{
    firmware_ota_download_context_t *download = context;
    ESP_RETURN_ON_FALSE(download != NULL && data != NULL, ESP_ERR_INVALID_ARG, TAG, "OTA 下载回调参数无效");
    ESP_RETURN_ON_FALSE(download->received_bytes + length <= download->expected_size,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "OTA 下载数据超过清单大小");
    ESP_RETURN_ON_ERROR(esp_ota_write(download->ota_handle, data, length), TAG, "写入备用 OTA 分区失败");
    ESP_RETURN_ON_FALSE(mbedtls_md_update(&download->digest, data, length) == 0, ESP_FAIL, TAG, "更新固件文件摘要失败");
    download->received_bytes += length;
    return ESP_OK;
}

/**
 * @brief 下载目标固件、校验并切换启动分区（不可取消写入事务）
 *
 * 流程：构造下载地址 -> 校验目标适配备用分区 -> 初始化 SHA-256 摘要与
 * esp_ota_begin -> 流式下载写入 -> 校验已接收字节数与 Content-Length ->
 * 计算并比对文件 SHA-256 -> esp_ota_end -> 比对写入镜像 Validation SHA-256 ->
 * esp_ota_set_boot_partition 切换启动分区。
 *
 * 资源契约：失败路径必须调用 esp_ota_abort 释放未完成的写入句柄，并释放 mbedtls
 * 摘要上下文。 file_sha256 校验的是下载字节流，artifact_id
 * 校验的是写入分区后的镜像摘要， 两者共同保证下载内容完整且写入正确。
 *
 * @param[in] target 已通过校验的目标固件描述
 * @return ESP_OK 已完成写入并切换启动分区；其他值表示地址、分区、下载或校验失败
 */
static esp_err_t firmware_ota_download_and_activate(const firmware_ota_target_t *target)
{
    char url[FIRMWARE_OTA_URL_MAX];
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), s_runtime.backend.base_url, target->url),
                        TAG,
                        "构造 OTA 下载地址失败");
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "未找到备用 OTA 分区");
    ESP_RETURN_ON_FALSE(target->size <= partition->size, ESP_ERR_INVALID_SIZE, TAG, "目标固件超过备用 OTA 分区容量");

    firmware_ota_download_context_t download = {
        .ota_handle     = 0,
        .received_bytes = 0U,
        .expected_size  = target->size,
    };
    mbedtls_md_init(&download.digest);
    const mbedtls_md_info_t *digest_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    esp_err_t                error = digest_info != NULL && mbedtls_md_setup(&download.digest, digest_info, 0) == 0
                                             && mbedtls_md_starts(&download.digest) == 0
                                         ? ESP_OK
                                         : ESP_FAIL;
    if (error == ESP_OK)
    {
        error = esp_ota_begin(partition, target->size, &download.ota_handle);
    }
    transport_http_download_result_t result = { 0 };
    if (error == ESP_OK)
    {
        transport_http_header_t                 headers[3];
        char                                    bearer[PROTOCOL_BACKEND_TOKEN_MAX + sizeof("Bearer ")] = { 0 };
        const size_t                            header_count = firmware_ota_make_headers(headers, bearer, false);
        const transport_http_download_request_t request      = {
            .url               = url,
            .headers           = headers,
            .header_count      = header_count,
            .read_buffer_bytes = FIRMWARE_OTA_DOWNLOAD_BUFFER_BYTES,
            .timeout_ms        = s_runtime.download_timeout_ms,
            .on_response_data  = firmware_ota_on_download_data,
            .should_continue   = NULL,
            .ctx               = &download,
        };
        error = transport_http_download_borrow(&request, &result);
    }

    uint8_t file_digest[FIRMWARE_OTA_SHA256_BYTES]     = { 0 };
    char    file_sha256[FIRMWARE_OTA_ARTIFACT_ID_SIZE] = { 0 };
    if (error == ESP_OK && download.received_bytes != target->size)
    {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK && result.content_length >= 0 && (size_t) result.content_length != target->size)
    {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK && mbedtls_md_finish(&download.digest, file_digest) != 0)
    {
        error = ESP_FAIL;
    }
    if (error == ESP_OK)
    {
        firmware_ota_format_sha256(file_digest, file_sha256);
        if (strcmp(file_sha256, target->file_sha256) != 0)
        {
            ESP_LOGE(TAG, "OTA 完整文件 SHA-256 与清单不一致");
            error = ESP_ERR_INVALID_CRC;
        }
    }
    if (error == ESP_OK)
    {
        error               = esp_ota_end(download.ota_handle);
        download.ota_handle = 0;
    }
    if (error == ESP_OK)
    {
        char validation_sha256[FIRMWARE_OTA_ARTIFACT_ID_SIZE];
        error = firmware_ota_partition_identity(partition, validation_sha256);
        if (error == ESP_OK && strcmp(validation_sha256, target->artifact_id) != 0)
        {
            ESP_LOGE(TAG, "写入镜像 Validation SHA-256 与目标制品不一致");
            error = ESP_ERR_INVALID_CRC;
        }
    }
    if (error == ESP_OK)
    {
        error = esp_ota_set_boot_partition(partition);
    }
    if (error != ESP_OK && download.ota_handle != 0)
    {
        (void) esp_ota_abort(download.ota_handle);
    }
    mbedtls_md_free(&download.digest);
    return error;
}

/**
 * @brief 查询并校验一次 OTA 目标，但不开始下载
 *
 * 流程：读取当前身份 -> 检查目标 -> 版本回退判断 -> 去重判断 ->
 * 输出完整目标与公开结果。
 * 服务端没有返回目标或目标版本不高于当前版本时按“无更新”成功返回。
 *
 * @param[out] out_result 公开检查结果
 * @param[out] out_target 可供后续安装的完整不可变目标
 * @return ESP_OK 有效响应；ESP_ERR_INVALID_STATE
 * 目标镜像与当前或上次无效镜像重复； 其他值表示身份读取、网络或响应错误
 */
static esp_err_t firmware_ota_run_check(firmware_ota_check_result_t *out_result, firmware_ota_target_t *out_target)
{
    memset(out_result, 0, sizeof(*out_result));
    memset(out_target, 0, sizeof(*out_target));
    firmware_ota_identity_t identity;
    ESP_RETURN_ON_ERROR(firmware_ota_get_identity_copy(&identity), TAG, "获取当前固件身份失败");
    firmware_ota_target_t target;
    bool                  has_update = false;
    ESP_RETURN_ON_ERROR(firmware_ota_check_target(&identity, &target, &has_update), TAG, "检查 OTA 目标失败");
    if (!has_update)
    {
        return ESP_OK;
    }
    if (target.ota_version <= identity.current_ota_version)
    {
        return ESP_OK;
    }
    if (strcmp(target.artifact_id, identity.current_artifact_id) == 0
        || (identity.has_last_invalid_artifact && strcmp(target.artifact_id, identity.last_invalid_artifact_id) == 0))
    {
        ESP_LOGW(TAG, "拒绝安装当前镜像或上次回滚镜像: %s", target.artifact_id);
        return ESP_ERR_INVALID_STATE;
    }

    out_result->update_available   = true;
    out_result->target_ota_version = target.ota_version;
    out_result->target_size        = target.size;
    utils_copy_string(out_result->target_version, sizeof(out_result->target_version), target.version);
    utils_copy_string(out_result->target_artifact_id, sizeof(out_result->target_artifact_id), target.artifact_id);
    *out_target = target;
    return ESP_OK;
}

/**
 * @brief 安装已经缓存的不可变 OTA 目标
 *
 * @param[in] target 检查阶段保存且用户确认前未改变的目标副本
 * @return ESP_OK 已切换启动分区；或下载、校验、写入错误码
 */
static esp_err_t firmware_ota_run_install(const firmware_ota_target_t *target)
{
    ESP_LOGI(TAG,
             "开始下载应用固件: version=%s, ota_version=%llu, size=%lu, "
             "artifact_id=%s",
             target->version,
             (unsigned long long) target->ota_version,
             (unsigned long) target->size,
             target->artifact_id);
    return firmware_ota_download_and_activate(target);
}

/**
 * @brief OTA Task 主循环：从命令队列取指令并执行
 *
 * CHECK 只保存通过校验的目标并进入 UPDATE_AVAILABLE；INSTALL
 * 消费该目标并执行不可取消写入， 失败时清除目标回到 IDLE，成功时进入
 * AWAITING_RESTART 并立即强制重启。事务状态稳定后， Task
 * 在内部锁之外调用完成回调。STOP 清除待确认目标、释放 task_stopped 后自删除
 * Task。
 *
 * @param[in] context 未使用
 */
static void firmware_ota_task(void *context)
{
    (void) context;
    firmware_ota_command_t command;
    for (;;)
    {
        if (xQueueReceive(s_runtime.commands, &command, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (command == FIRMWARE_OTA_COMMAND_STOP)
        {
            firmware_ota_lock();
            s_runtime.started        = false;
            s_runtime.stopping       = false;
            s_runtime.task           = NULL;
            s_runtime.state          = FIRMWARE_OTA_STATE_STOPPED;
            s_runtime.pending_target = (firmware_ota_target_t) { 0 };
            firmware_ota_unlock();
            (void) xSemaphoreGive(s_runtime.task_stopped);
            vTaskDelete(NULL);
            return;
        }

        if (command == FIRMWARE_OTA_COMMAND_CHECK)
        {
            firmware_ota_check_result_t result = { 0 };
            firmware_ota_target_t       target = { 0 };
            const esp_err_t             error  = firmware_ota_run_check(&result, &target);
            firmware_ota_event_t        event  = {
                .type         = FIRMWARE_OTA_EVENT_CHECK_COMPLETED,
                .result       = error,
                .check_result = result,
            };
            firmware_ota_lock();
            if (error == ESP_OK && result.update_available)
            {
                s_runtime.pending_target = target;
                s_runtime.state          = FIRMWARE_OTA_STATE_UPDATE_AVAILABLE;
            }
            else
            {
                s_runtime.pending_target = (firmware_ota_target_t) { 0 };
                s_runtime.state          = FIRMWARE_OTA_STATE_IDLE;
            }
            event.state                            = s_runtime.state;
            firmware_ota_event_cb_t event_callback = s_runtime.event_callback;
            void                   *event_context  = s_runtime.event_context;
            s_runtime.event_callback_running       = event_callback != NULL;
            firmware_ota_unlock();
            if (event_callback != NULL)
            {
                event_callback(&event, event_context);
                firmware_ota_lock();
                s_runtime.event_callback_running = false;
                firmware_ota_unlock();
            }
            continue;
        }

        firmware_ota_lock();
        const firmware_ota_target_t target = s_runtime.pending_target;
        firmware_ota_unlock();
        const esp_err_t      error = firmware_ota_run_install(&target);
        firmware_ota_event_t event = {
            .type   = FIRMWARE_OTA_EVENT_INSTALL_COMPLETED,
            .result = error,
        };
        firmware_ota_lock();
        s_runtime.pending_target = (firmware_ota_target_t) { 0 };
        if (error == ESP_OK)
        {
            s_runtime.state = FIRMWARE_OTA_STATE_AWAITING_RESTART;
        }
        else
        {
            s_runtime.state = FIRMWARE_OTA_STATE_IDLE;
        }
        event.state                            = s_runtime.state;
        firmware_ota_event_cb_t event_callback = s_runtime.event_callback;
        void                   *event_context  = s_runtime.event_context;
        s_runtime.event_callback_running       = event_callback != NULL;
        firmware_ota_unlock();
        if (event_callback != NULL)
        {
            event_callback(&event, event_context);
            firmware_ota_lock();
            s_runtime.event_callback_running = false;
            firmware_ota_unlock();
        }
        if (error == ESP_OK)
        {
            ESP_LOGI(TAG, "启动分区已切换，立即强制重启");
            esp_restart();
        }
    }
}

/**
 * @brief 初始化 OTA 运行资源，但不启动 Task
 *
 * 实现要点：创建命令队列、互斥量及停止信号量；任一同步原语创建失败时
 * 回滚已分配资源并返回 ESP_ERR_NO_MEM。
 */
esp_err_t firmware_ota_init(const firmware_ota_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->check_timeout_ms > 0 && config->download_timeout_ms > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "OTA 生命周期配置无效");
    ESP_RETURN_ON_FALSE(!s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具已经初始化");
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.commands     = xQueueCreate(2U, sizeof(firmware_ota_command_t));
    s_runtime.mutex        = xSemaphoreCreateMutex();
    s_runtime.task_stopped = xSemaphoreCreateBinary();
    if (s_runtime.commands == NULL || s_runtime.mutex == NULL || s_runtime.task_stopped == NULL)
    {
        if (s_runtime.commands != NULL)
        {
            vQueueDelete(s_runtime.commands);
        }
        if (s_runtime.mutex != NULL)
        {
            vSemaphoreDelete(s_runtime.mutex);
        }
        if (s_runtime.task_stopped != NULL)
        {
            vSemaphoreDelete(s_runtime.task_stopped);
        }
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }
    s_runtime.check_timeout_ms    = config->check_timeout_ms;
    s_runtime.download_timeout_ms = config->download_timeout_ms;
    s_runtime.state               = FIRMWARE_OTA_STATE_STOPPED;
    s_runtime.initialized         = true;
    return ESP_OK;
}

/** @brief 在无进行中事务时替换或清除长期借用的完成回调 */
esp_err_t firmware_ota_set_event_callback_borrow(firmware_ota_event_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(callback != NULL || context == NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "清除 OTA 完成回调时上下文必须为空");
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed = !s_runtime.stopping && !s_runtime.event_callback_running
                         && (s_runtime.state == FIRMWARE_OTA_STATE_STOPPED || s_runtime.state == FIRMWARE_OTA_STATE_IDLE
                             || s_runtime.state == FIRMWARE_OTA_STATE_UPDATE_AVAILABLE);
    if (allowed)
    {
        s_runtime.event_callback = callback;
        s_runtime.event_context  = context;
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA 事务进行中，不能替换完成回调");
    return ESP_OK;
}

/**
 * @brief 在锁内复制服务端连接配置
 *
 * 实现要点：仅在 STOPPED 或 IDLE 状态允许写入，避免与进行中的检查事务竞争。
 */
esp_err_t firmware_ota_configure_copy(const protocol_backend_context_t *backend)
{
    ESP_RETURN_ON_FALSE(protocol_backend_context_is_valid(backend),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "OTA 服务连接配置无效");
    const protocol_backend_context_t backend_copy = *backend;
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed =
        !s_runtime.stopping
        && (s_runtime.state == FIRMWARE_OTA_STATE_STOPPED || s_runtime.state == FIRMWARE_OTA_STATE_IDLE);
    if (allowed)
    {
        s_runtime.backend    = backend_copy;
        s_runtime.configured = true;
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA 当前状态不允许更新服务配置");
    return ESP_OK;
}

/**
 * @brief 启动独立 OTA Task
 *
 * 实现要点：先在锁内抢占 started 标志并置 IDLE，再创建
 * Task；创建失败时回滚状态。
 */
esp_err_t firmware_ota_start(void)
{
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed = !s_runtime.started && !s_runtime.stopping && s_runtime.state == FIRMWARE_OTA_STATE_STOPPED;
    if (allowed)
    {
        s_runtime.started = true;
        s_runtime.state   = FIRMWARE_OTA_STATE_IDLE;
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA Task 当前状态不允许启动");
    if (xTaskCreate(firmware_ota_task,
                    "firmware_ota",
                    FIRMWARE_OTA_TASK_STACK_SIZE,
                    NULL,
                    FIRMWARE_OTA_TASK_PRIORITY,
                    &s_runtime.task)
        != pdPASS)
    {
        firmware_ota_lock();
        s_runtime.started = false;
        s_runtime.state   = FIRMWARE_OTA_STATE_STOPPED;
        firmware_ota_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 原子进入检查状态并异步投递 CHECK 命令
 *
 * 队列满时恢复 IDLE，返回值只描述命令是否成功提交。事务最终结果由 OTA Task
 * 通过完成回调 返回，有更新时 Task 缓存完整目标并保持 UPDATE_AVAILABLE。
 */
esp_err_t firmware_ota_request_check(void)
{
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed = s_runtime.started && !s_runtime.stopping && s_runtime.configured
                         && s_runtime.event_callback != NULL && s_runtime.state == FIRMWARE_OTA_STATE_IDLE;
    if (allowed)
    {
        s_runtime.state          = FIRMWARE_OTA_STATE_CHECKING;
        s_runtime.pending_target = (firmware_ota_target_t) { 0 };
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA 当前状态或配置不接受检查请求");

    const firmware_ota_command_t command = FIRMWARE_OTA_COMMAND_CHECK;
    if (xQueueSend(s_runtime.commands, &command, 0U) != pdTRUE)
    {
        firmware_ota_lock();
        s_runtime.state = FIRMWARE_OTA_STATE_IDLE;
        firmware_ota_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief 原子消费待确认目标并异步投递 INSTALL 命令
 *
 * 队列满时恢复 UPDATE_AVAILABLE。提交成功后写入事务不可取消，最终结果由 OTA
 * Task 通过完成 回调返回；失败时清除目标并回到
 * IDLE，成功时切换启动分区并立即重启。
 */
esp_err_t firmware_ota_request_install(void)
{
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed = s_runtime.started && !s_runtime.stopping && s_runtime.event_callback != NULL
                         && s_runtime.state == FIRMWARE_OTA_STATE_UPDATE_AVAILABLE;
    if (allowed)
    {
        s_runtime.state = FIRMWARE_OTA_STATE_DOWNLOADING;
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA 当前没有待安装目标或完成回调");

    const firmware_ota_command_t command = FIRMWARE_OTA_COMMAND_INSTALL;
    if (xQueueSend(s_runtime.commands, &command, 0U) != pdTRUE)
    {
        firmware_ota_lock();
        s_runtime.state = FIRMWARE_OTA_STATE_UPDATE_AVAILABLE;
        firmware_ota_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/** @brief 在锁内幂等清除尚未开始写入的待确认目标 */
esp_err_t firmware_ota_discard_pending_update(void)
{
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed =
        s_runtime.started && !s_runtime.stopping
        && (s_runtime.state == FIRMWARE_OTA_STATE_IDLE || s_runtime.state == FIRMWARE_OTA_STATE_UPDATE_AVAILABLE);
    if (allowed)
    {
        s_runtime.pending_target = (firmware_ota_target_t) { 0 };
        s_runtime.state          = FIRMWARE_OTA_STATE_IDLE;
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA 当前状态不允许丢弃目标");
    return ESP_OK;
}

/**
 * @brief 同步停止 OTA Task
 *
 * 实现要点：投递 STOP 命令后等待
 * task_stopped；不发送取消信号，进行中的事务必须先自然结束。
 */
esp_err_t firmware_ota_stop(void)
{
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    const bool allowed =
        s_runtime.started && !s_runtime.stopping && s_runtime.state != FIRMWARE_OTA_STATE_AWAITING_RESTART;
    if (allowed)
    {
        s_runtime.stopping = true;
    }
    firmware_ota_unlock();
    ESP_RETURN_ON_FALSE(allowed, ESP_ERR_INVALID_STATE, TAG, "OTA Task 当前状态不允许停止");
    while (xSemaphoreTake(s_runtime.task_stopped, 0U) == pdTRUE)
    {
    }
    const firmware_ota_command_t command = FIRMWARE_OTA_COMMAND_STOP;
    if (xQueueSend(s_runtime.commands, &command, portMAX_DELAY) != pdTRUE)
    {
        firmware_ota_lock();
        s_runtime.stopping = false;
        firmware_ota_unlock();
        ESP_LOGE(TAG, "提交 OTA 停止命令失败");
        return ESP_FAIL;
    }
    const uint64_t wait_ms =
        (uint64_t) s_runtime.check_timeout_ms + (uint64_t) s_runtime.download_timeout_ms + FIRMWARE_OTA_STOP_GRACE_MS;
    return xSemaphoreTake(s_runtime.task_stopped, pdMS_TO_TICKS(wait_ms)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief 释放 OTA 同步资源
 *
 * 实现要点：仅在已初始化、Task 已停止且状态为 STOPPED
 * 时允许释放，避免悬空句柄。
 */
esp_err_t firmware_ota_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_runtime.initialized && !s_runtime.started && !s_runtime.stopping
                            && s_runtime.state == FIRMWARE_OTA_STATE_STOPPED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "OTA 当前生命周期不允许反初始化");
    vQueueDelete(s_runtime.commands);
    vSemaphoreDelete(s_runtime.mutex);
    vSemaphoreDelete(s_runtime.task_stopped);
    memset(&s_runtime, 0, sizeof(s_runtime));
    return ESP_OK;
}

/** @brief 在锁内复制 Task 当前状态 */
esp_err_t firmware_ota_get_state_copy(firmware_ota_state_t *out_state)
{
    ESP_RETURN_ON_FALSE(out_state != NULL, ESP_ERR_INVALID_ARG, TAG, "OTA 状态输出指针为空");
    ESP_RETURN_ON_FALSE(s_runtime.initialized, ESP_ERR_INVALID_STATE, TAG, "OTA 工具尚未初始化");
    firmware_ota_lock();
    *out_state = s_runtime.state;
    firmware_ota_unlock();
    return ESP_OK;
}

/**
 * @brief 若当前镜像处于待验证状态，则标记有效以确认本次本地健康启动
 *
 * 实现要点：未找到 OTA 状态或镜像非待验证时视为无需确认，直接返回 ESP_OK。
 */
esp_err_t firmware_ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(running != NULL, ESP_ERR_NOT_FOUND, TAG, "当前运行分区不存在");
    esp_ota_img_states_t state;
    const esp_err_t      state_error = esp_ota_get_state_partition(running, &state);
    if (state_error == ESP_ERR_NOT_FOUND)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(state_error, TAG, "读取当前 OTA 镜像状态失败");
    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG, "确认本地健康启动失败");
    ESP_LOGI(TAG, "当前 OTA 镜像已通过本地健康确认");
    return ESP_OK;
}

/**
 * @brief 若当前镜像处于待验证状态，则标记无效并立即回滚重启
 *
 * 实现要点：未找到 OTA 状态或镜像非待验证时视为无需回滚；成功回滚时不返回。
 */
esp_err_t firmware_ota_reject_running_image_and_reboot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(running != NULL, ESP_ERR_NOT_FOUND, TAG, "当前运行分区不存在");
    esp_ota_img_states_t state;
    const esp_err_t      state_error = esp_ota_get_state_partition(running, &state);
    if (state_error == ESP_ERR_NOT_FOUND)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(state_error, TAG, "读取待回滚 OTA 镜像状态失败");
    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "本地启动健康门槛失败，标记当前镜像无效并回滚重启");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
