/**
 * @file display_protocol.c
 * @brief 显示 Manifest 与帧下载协议实现
 */
#include "display_protocol.h"
#include "display_frame_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "display_protocol";

/** @brief 服务端绝对刷新时间允许的最早 UTC 秒：2024-01-01 00:00:00 */
#define DISPLAY_PROTOCOL_MIN_REFRESH_TIMESTAMP INT64_C(1704067200)
/** @brief 服务端绝对刷新时间允许的最晚 UTC 秒：2099-12-31 23:59:59 */
#define DISPLAY_PROTOCOL_MAX_REFRESH_TIMESTAMP INT64_C(4102444799)

/**
 * @brief 判断日期时间版本格式是否合法
 *
 * @param[in] version 版本字符串
 * @return true 格式为 YYYYMMDD-HHMMSS，false 不合法
 */
static bool display_protocol_version_is_valid(const char *version)
{
    if (version == NULL || strlen(version) != 15U || version[8] != '-')
    {
        return false;
    }
    for (size_t i = 0; i < 15U; ++i)
    {
        if (i != 8U && (version[i] < '0' || version[i] > '9'))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 判断页面 ID 是否只包含安全字符
 *
 * @param[in] page_id 页面 ID
 * @return true 合法，false 不合法
 */
static bool display_protocol_page_id_is_valid(const char *page_id)
{
    if (page_id == NULL || page_id[0] == '\0')
    {
        return false;
    }
    const size_t length = strlen(page_id);
    if (length >= DISPLAY_PROTOCOL_PAGE_ID_MAX)
    {
        return false;
    }
    for (size_t index = 0U; index < length; ++index)
    {
        const char value = page_id[index];
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
              || (value >= '0' && value <= '9') || value == '_' || value == '-'))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 判断同源相对 URL 是否安全且可完整保存
 *
 * @param[in] url 相对 URL
 * @return true 合法，false 不合法
 */
static bool display_protocol_relative_url_is_valid(const char *url)
{
    return url != NULL && url[0] == '/' && strlen(url) < DISPLAY_PROTOCOL_URL_MAX
           && strstr(url, "://") == NULL && strstr(url, "..") == NULL;
}

/**
 * @brief 解析并校验 Manifest v3 中的单个页面
 *
 * @param[in] item 页面 JSON 对象
 * @param[in] expected_file_size 集合声明的单页文件长度
 * @param[out] out_page 页面输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 字段无效
 */
static esp_err_t display_protocol_parse_page(const cJSON *item, uint32_t expected_file_size,
                                             display_protocol_page_t *out_page)
{
    ESP_RETURN_ON_FALSE(cJSON_IsObject(item) && out_page != NULL,
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "显示页面对象无效");
    const cJSON *page_id        = cJSON_GetObjectItemCaseSensitive(item, "page_id");
    const cJSON *version        = cJSON_GetObjectItemCaseSensitive(item, "content_version");
    const cJSON *file_size      = cJSON_GetObjectItemCaseSensitive(item, "file_size");
    const cJSON *crc32          = cJSON_GetObjectItemCaseSensitive(item, "crc32");
    const cJSON *sha256         = cJSON_GetObjectItemCaseSensitive(item, "sha256");
    const cJSON *payload_sha256 = cJSON_GetObjectItemCaseSensitive(item, "payload_sha256");
    const cJSON *frame_url      = cJSON_GetObjectItemCaseSensitive(item, "frame_url");

    const bool valid =
        cJSON_IsString(page_id) && display_protocol_page_id_is_valid(page_id->valuestring)
        && cJSON_IsString(version) && display_protocol_version_is_valid(version->valuestring)
        && cJSON_IsNumber(file_size) && file_size->valuedouble == (double) expected_file_size
        && cJSON_IsString(crc32) && utils_is_hex_string(crc32->valuestring, 8U)
        && cJSON_IsString(sha256) && utils_is_hex_string(sha256->valuestring, 64U)
        && cJSON_IsString(payload_sha256) && utils_is_hex_string(payload_sha256->valuestring, 64U)
        && cJSON_IsString(frame_url)
        && display_protocol_relative_url_is_valid(frame_url->valuestring);
    ESP_RETURN_ON_FALSE(valid, ESP_ERR_INVALID_RESPONSE, TAG, "显示页面字段无效");

    snprintf(out_page->page_id, sizeof(out_page->page_id), "%s", page_id->valuestring);
    snprintf(out_page->content_version,
             sizeof(out_page->content_version),
             "%s",
             version->valuestring);
    out_page->file_size = (uint32_t) file_size->valuedouble;
    out_page->crc32     = (uint32_t) strtoul(crc32->valuestring, NULL, 16);
    snprintf(out_page->sha256, sizeof(out_page->sha256), "%s", sha256->valuestring);
    snprintf(out_page->payload_sha256,
             sizeof(out_page->payload_sha256),
             "%s",
             payload_sha256->valuestring);
    snprintf(out_page->frame_url, sizeof(out_page->frame_url), "%s", frame_url->valuestring);
    return ESP_OK;
}

/**
 * @brief 从 JSON 解析并校验显示 Manifest
 *
 * @param[in] body JSON 文本
 * @param[out] out Manifest
 * @return ESP_OK 成功，或 ESP_ERR_INVALID_RESPONSE
 */
static esp_err_t display_protocol_parse_manifest(const char *body, display_protocol_manifest_t *out)
{
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "显示 Manifest JSON 无效");

    const cJSON *protocol        = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *format          = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *pixel_format    = cJSON_GetObjectItemCaseSensitive(root, "pixel_format");
    const cJSON *version         = cJSON_GetObjectItemCaseSensitive(root, "collection_version");
    const cJSON *default_page    = cJSON_GetObjectItemCaseSensitive(root, "default_page");
    const cJSON *width           = cJSON_GetObjectItemCaseSensitive(root, "width");
    const cJSON *height          = cJSON_GetObjectItemCaseSensitive(root, "height");
    const cJSON *bpp             = cJSON_GetObjectItemCaseSensitive(root, "bits_per_pixel");
    const cJSON *header_size     = cJSON_GetObjectItemCaseSensitive(root, "header_size");
    const cJSON *payload_size    = cJSON_GetObjectItemCaseSensitive(root, "payload_size");
    const cJSON *file_size       = cJSON_GetObjectItemCaseSensitive(root, "file_size");
    const cJSON *pages           = cJSON_GetObjectItemCaseSensitive(root, "pages");
    const cJSON *next_refresh_at = cJSON_GetObjectItemCaseSensitive(root, "next_refresh_at");
    const int    page_count_json = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;

    bool valid = cJSON_IsNumber(protocol) && protocol->valueint == 3 && cJSON_IsString(format)
                 && strcmp(format->valuestring, DISPLAY_FRAME_PROTOCOL_FORMAT) == 0
                 && cJSON_IsString(pixel_format)
                 && strcmp(pixel_format->valuestring, DISPLAY_FRAME_PROTOCOL_PIXEL_FORMAT) == 0
                 && cJSON_IsString(version)
                 && display_protocol_version_is_valid(version->valuestring)
                 && cJSON_IsString(default_page)
                 && display_protocol_page_id_is_valid(default_page->valuestring)
                 && cJSON_IsNumber(width)
                 && width->valueint == (int) DISPLAY_FRAME_PROTOCOL_WIDTH
                 && cJSON_IsNumber(height)
                 && height->valueint == (int) DISPLAY_FRAME_PROTOCOL_HEIGHT
                 && cJSON_IsNumber(bpp)
                 && bpp->valueint == (int) DISPLAY_FRAME_PROTOCOL_BITS_PER_PIXEL
                 && cJSON_IsNumber(header_size)
                 && header_size->valueint == (int) DISPLAY_FRAME_PROTOCOL_HEADER_SIZE
                 && cJSON_IsNumber(payload_size)
                 && payload_size->valuedouble == (double) DISPLAY_FRAME_PROTOCOL_PAYLOAD_SIZE
                 && cJSON_IsNumber(file_size)
                 && file_size->valuedouble == (double) DISPLAY_FRAME_PROTOCOL_FILE_SIZE
                 && page_count_json > 0 && page_count_json <= (int) DISPLAY_PROTOCOL_PAGE_MAX;
    if (!valid)
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "显示 Manifest 字段无效");
        return ESP_ERR_INVALID_RESPONSE;
    }

    snprintf(out->collection_version, sizeof(out->collection_version), "%s", version->valuestring);
    snprintf(out->default_page, sizeof(out->default_page), "%s", default_page->valuestring);
    out->width          = (uint16_t) width->valueint;
    out->height         = (uint16_t) height->valueint;
    out->bits_per_pixel = (uint8_t) bpp->valueint;
    out->header_size    = (uint16_t) header_size->valueint;
    out->payload_size   = (uint32_t) payload_size->valuedouble;
    out->file_size      = (uint32_t) file_size->valuedouble;
    out->page_count     = (uint8_t) page_count_json;

    bool default_found  = false;
    for (int index = 0; index < page_count_json; ++index)
    {
        const cJSON    *item = cJSON_GetArrayItem(pages, index);
        const esp_err_t page_err =
            display_protocol_parse_page(item, out->file_size, &out->pages[index]);
        if (page_err != ESP_OK)
        {
            cJSON_Delete(root);
            return page_err;
        }
        for (int previous = 0; previous < index; ++previous)
        {
            if (strcmp(out->pages[index].page_id, out->pages[previous].page_id) == 0)
            {
                cJSON_Delete(root);
                ESP_LOGE(TAG, "显示 Manifest 含重复页面 ID");
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (strcmp(out->pages[index].page_id, out->default_page) == 0)
        {
            default_found = true;
        }
    }
    if (!default_found)
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "显示 Manifest 默认页面不存在");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsNumber(next_refresh_at)
        || next_refresh_at->valuedouble < (double) DISPLAY_PROTOCOL_MIN_REFRESH_TIMESTAMP
        || next_refresh_at->valuedouble > (double) DISPLAY_PROTOCOL_MAX_REFRESH_TIMESTAMP)
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "显示 Manifest v3 缺少有效的 next_refresh_at");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int64_t timestamp = (int64_t) next_refresh_at->valuedouble;
    if (next_refresh_at->valuedouble != (double) timestamp)
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "显示 Manifest 的 next_refresh_at 必须为整数秒");
        return ESP_ERR_INVALID_RESPONSE;
    }
    out->next_refresh_at_utc = timestamp;
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 同步查询并复制设备当前显示 Manifest
 */
esp_err_t display_protocol_get_manifest_copy(const protocol_backend_context_t *in_backend,
                                             const char                       *in_current_version,
                                             int64_t in_current_next_refresh_at_utc,
                                             int timeout_ms,
                                             display_protocol_manifest_t *out_manifest)
{
    ESP_RETURN_ON_FALSE(protocol_backend_context_is_valid(in_backend) && out_manifest != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "显示 Manifest 参数无效");
    memset(out_manifest, 0, sizeof(*out_manifest));

    char url[256];
    ESP_RETURN_ON_ERROR(
        protocol_url_build(url, sizeof(url), in_backend->base_url, "/api/v2/display/manifest"),
        TAG,
        "构造显示 Manifest URL 失败");
    transport_http_header_t headers[4];
    size_t                  header_count = 0;
    char                    bearer[128]  = { 0 };
    char                    etag[64]     = { 0 };
    protocol_identity_add_headers(headers,
                                  &header_count,
                                  in_backend->token,
                                  in_backend->device_id,
                                  bearer,
                                  sizeof(bearer));
    headers[header_count++] = (transport_http_header_t){
        .name  = "Accept",
        .value = "application/json",
    };
    if (in_current_version != NULL && in_current_version[0] != '\0'
        && in_current_next_refresh_at_utc > 0)
    {
        snprintf(etag,
                 sizeof(etag),
                 "\"%s:%lld\"",
                 in_current_version,
                 (long long) in_current_next_refresh_at_utc);
        headers[header_count++] = (transport_http_header_t){
            .name  = "If-None-Match",
            .value = etag,
        };
    }

    const transport_http_request_t request = {
        .url                = url,
        .method             = TRANSPORT_HTTP_GET,
        .headers            = headers,
        .header_count       = header_count,
        .timeout_ms         = timeout_ms,
        .max_response_bytes = 16384U,
    };
    transport_http_response_t response = { 0 };
    const int64_t             started_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "开始检查显示 Manifest v3: current_version=%s, current_next_refresh_at=%lld, "
             "timeout=%d ms",
             in_current_version != NULL && in_current_version[0] != '\0'
                 ? in_current_version
                 : "无",
             (long long) in_current_next_refresh_at_utc,
             timeout_ms);
    esp_err_t err = transport_http_perform_borrow(&request, &response);
    if (err == ESP_OK && response.status_code == 304)
    {
        out_manifest->not_modified = true;
    }
    else if (err == ESP_OK && response.status_code == 200)
    {
        err = display_protocol_parse_manifest(response.body, out_manifest);
    }
    else if (err == ESP_OK)
    {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000LL;
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "显示 Manifest 检查完成: status=%d, result=%s, body=%u bytes, total=%lld ms",
                 response.status_code,
                 out_manifest->not_modified ? "未变更" : "已解析",
                 (unsigned int) response.body_len,
                 (long long) elapsed_ms);
    }
    else
    {
        ESP_LOGW(TAG,
                 "显示 Manifest 检查失败: status=%d, error=%s, body=%u bytes, total=%lld ms",
                 response.status_code,
                 esp_err_to_name(err),
                 (unsigned int) response.body_len,
                 (long long) elapsed_ms);
    }
    transport_http_response_release(&response);
    return err;
}

/**
 * @brief 同步借用请求参数并流式下载 Manifest 指定的 PPF 文件
 */
esp_err_t display_protocol_download_frame_borrow(const protocol_backend_context_t *in_backend,
                                                 const char *in_frame_url, int timeout_ms,
                                                 transport_http_data_cb_t          in_callback,
                                                 void                             *in_context,
                                                 transport_http_download_result_t *out_result)
{
    ESP_RETURN_ON_FALSE(protocol_backend_context_is_valid(in_backend) && in_frame_url != NULL
                            && in_frame_url[0] == '/' && strstr(in_frame_url, "://") == NULL
                            && strstr(in_frame_url, "..") == NULL && in_callback != NULL
                            && out_result != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "显示帧下载参数无效");
    char url[320];
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), in_backend->base_url, in_frame_url),
                        TAG,
                        "构造显示帧 URL 失败");
    transport_http_header_t headers[3];
    size_t                  header_count = 0;
    char                    bearer[128]  = { 0 };
    protocol_identity_add_headers(headers,
                                  &header_count,
                                  in_backend->token,
                                  in_backend->device_id,
                                  bearer,
                                  sizeof(bearer));
    headers[header_count++] = (transport_http_header_t){
        .name  = "Accept",
        .value = "application/octet-stream",
    };
    const transport_http_download_request_t request = {
        .url                   = url,
        .headers               = headers,
        .header_count          = header_count,
        .read_buffer_bytes     = 4096U,
        .timeout_ms            = timeout_ms,
        .automatic_redirects   = false,
        .on_response_data      = in_callback,
        .ctx                   = in_context,
    };
    return transport_http_download_borrow(&request, out_result);
}
