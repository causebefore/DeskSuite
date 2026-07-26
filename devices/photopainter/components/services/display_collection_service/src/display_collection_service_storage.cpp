/**
 * @file display_collection_service_storage.cpp
 * @brief 实现显示集合的 SD 目录、JSON、A/B 状态槽与清理事务
 */
#include "display_collection_service_internal.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "device_sd.h"
#include "display_frame_protocol.h"
#include "esp_log.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "collection_storage";

/** @brief 本地集合持久化格式版本 */
static constexpr int DISPLAY_COLLECTION_STORAGE_SCHEMA_VERSION = 3;
/** @brief 可持久化绝对刷新时间的最早 UTC 秒 */
static constexpr int64_t DISPLAY_COLLECTION_MIN_REFRESH_TIMESTAMP = 1704067200LL;
/** @brief 可持久化绝对刷新时间的最晚 UTC 秒 */
static constexpr int64_t DISPLAY_COLLECTION_MAX_REFRESH_TIMESTAMP = 4102444799LL;
/** @brief A/B 状态文件相对路径 */
static constexpr const char *DISPLAY_COLLECTION_STATE_PATHS[] = {
    "display/state_a.json",
    "display/state_b.json",
};

/** @brief 已解析的 A/B 状态槽 */
struct DisplayCollectionStoredState
{
    uint64_t generation = 0U;
    char active_collection[DISPLAY_PROTOCOL_VERSION_MAX] = {};
    char previous_collection[DISPLAY_PROTOCOL_VERSION_MAX] = {};
};

/**
 * @brief 依次创建显示集合所需目录
 */
static esp_err_t display_collection_storage_prepare_directories()
{
    static constexpr const char *directories[] = {
        "display",
        "display/collections",
        "display/pages",
        "display/tmp",
    };
    for (const char *directory : directories)
    {
        const esp_err_t error = device_sd_make_directory(directory);
        if (error != ESP_OK)
        {
            return error;
        }
    }
    return ESP_OK;
}

/**
 * @brief 计算文本 SHA-256 并输出小写十六进制
 */
static esp_err_t display_collection_storage_hash_text(const char *text, char out_hex[65])
{
    uint8_t digest[UTILS_SHA256_DIGEST_SIZE];
    const esp_err_t error = utils_sha256(reinterpret_cast<const uint8_t *>(text),
                                         std::strlen(text),
                                         digest);
    if (error != ESP_OK)
    {
        return error;
    }
    return utils_bytes_to_hex(out_hex, 65U, digest, sizeof(digest), false) ? ESP_OK
                                                                         : ESP_FAIL;
}

/**
 * @brief 为无 checksum 的 JSON 根对象追加校验和并序列化
 *
 * @param[in,out] root JSON 根对象，成功后包含 checksum_sha256
 * @param[out] out_text 由 cJSON 分配的最终文本，调用方使用 cJSON_free 释放
 * @return ESP_OK 成功；或内存、编码错误码
 */
static esp_err_t display_collection_storage_serialize(cJSON *root, char **out_text)
{
    if (root == nullptr || out_text == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *canonical = cJSON_PrintUnformatted(root);
    if (canonical == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    char checksum[65];
    const esp_err_t error = display_collection_storage_hash_text(canonical, checksum);
    cJSON_free(canonical);
    if (error != ESP_OK)
    {
        return error;
    }
    if (cJSON_AddStringToObject(root, "checksum_sha256", checksum) == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    *out_text = cJSON_PrintUnformatted(root);
    return *out_text != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * @brief 解析并校验由本组件生成的 JSON 文本
 *
 * 成功返回的根对象已经移除 checksum_sha256，调用方负责 cJSON_Delete。
 */
static esp_err_t display_collection_storage_parse_verified(const char *text, cJSON **out_root)
{
    if (text == nullptr || out_root == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(text);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *checksum_item = cJSON_DetachItemFromObjectCaseSensitive(root, "checksum_sha256");
    if (!cJSON_IsString(checksum_item) || !utils_is_hex_string(checksum_item->valuestring, 64U))
    {
        cJSON_Delete(checksum_item);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    char expected[65];
    utils_copy_string(expected, sizeof(expected), checksum_item->valuestring);
    cJSON_Delete(checksum_item);

    char *canonical = cJSON_PrintUnformatted(root);
    if (canonical == nullptr)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char actual[65];
    const esp_err_t error = display_collection_storage_hash_text(canonical, actual);
    cJSON_free(canonical);
    if (error != ESP_OK || std::strcmp(actual, expected) != 0)
    {
        cJSON_Delete(root);
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    *out_root = root;
    return ESP_OK;
}

/**
 * @brief 使用 Runtime 单页缓冲区读取并验证 JSON 文件
 */
static esp_err_t display_collection_storage_read_json(const char *path, cJSON **out_root)
{
    size_t size = 0U;
    const esp_err_t error = device_sd_read_file(path,
                                                g_display_collection_runtime.buffer,
                                                DISPLAY_FRAME_PROTOCOL_FILE_SIZE - 1U,
                                                &size);
    if (error != ESP_OK)
    {
        return error;
    }
    g_display_collection_runtime.buffer[size] = '\0';
    return display_collection_storage_parse_verified(
        reinterpret_cast<const char *>(g_display_collection_runtime.buffer), out_root);
}

/**
 * @brief 将带校验和的 JSON 覆盖写入指定路径
 */
static esp_err_t display_collection_storage_write_json(const char *path, cJSON *root)
{
    char *text = nullptr;
    const esp_err_t serialize_error = display_collection_storage_serialize(root, &text);
    if (serialize_error != ESP_OK)
    {
        return serialize_error;
    }
    const esp_err_t write_error = device_sd_write_file(path, text, std::strlen(text));
    cJSON_free(text);
    return write_error;
}

/**
 * @brief 从 JSON 根对象读取并校验状态槽
 */
static esp_err_t display_collection_storage_parse_state(cJSON *root,
                                                        DisplayCollectionStoredState *out_state)
{
    const cJSON *schema     = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
    const cJSON *active     = cJSON_GetObjectItemCaseSensitive(root, "active_collection");
    const cJSON *previous   = cJSON_GetObjectItemCaseSensitive(root, "previous_collection");
    if (cJSON_IsNumber(schema) && schema->valueint != DISPLAY_COLLECTION_STORAGE_SCHEMA_VERSION)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!cJSON_IsNumber(schema) || !cJSON_IsNumber(generation) || generation->valuedouble < 0.0
        || !cJSON_IsString(active) || active->valuestring[0] == '\0'
        || std::strlen(active->valuestring) >= sizeof(out_state->active_collection)
        || !cJSON_IsString(previous)
        || std::strlen(previous->valuestring) >= sizeof(out_state->previous_collection))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_state->generation = static_cast<uint64_t>(generation->valuedouble);
    utils_copy_string(out_state->active_collection,
                      sizeof(out_state->active_collection),
                      active->valuestring);
    utils_copy_string(out_state->previous_collection,
                      sizeof(out_state->previous_collection),
                      previous->valuestring);
    return ESP_OK;
}

/**
 * @brief 读取一个 A/B 状态槽
 */
static esp_err_t display_collection_storage_load_state(int8_t slot,
                                                       DisplayCollectionStoredState *out_state)
{
    cJSON *root = nullptr;
    esp_err_t error = display_collection_storage_read_json(
        DISPLAY_COLLECTION_STATE_PATHS[slot], &root);
    if (error == ESP_OK)
    {
        error = display_collection_storage_parse_state(root, out_state);
    }
    cJSON_Delete(root);
    return error;
}

/**
 * @brief 从本地集合 Manifest 解析页面元数据
 */
static esp_err_t display_collection_storage_parse_page(const cJSON *item,
                                                       display_collection_page_t *out_page)
{
    const cJSON *page_id = cJSON_GetObjectItemCaseSensitive(item, "page_id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(item, "content_version");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "file_size");
    const cJSON *crc = cJSON_GetObjectItemCaseSensitive(item, "crc32");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(item, "sha256");
    const cJSON *payload_sha = cJSON_GetObjectItemCaseSensitive(item, "payload_sha256");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "frame_url");
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "file_path");
    if (!cJSON_IsString(page_id) || page_id->valuestring[0] == '\0'
        || std::strlen(page_id->valuestring) >= sizeof(out_page->protocol.page_id)
        || !cJSON_IsString(version)
        || std::strlen(version->valuestring) >= sizeof(out_page->protocol.content_version)
        || !cJSON_IsNumber(size) || size->valuedouble != DISPLAY_FRAME_PROTOCOL_FILE_SIZE
        || !cJSON_IsString(crc) || !utils_is_hex_string(crc->valuestring, 8U)
        || !cJSON_IsString(sha) || !utils_is_hex_string(sha->valuestring, 64U)
        || !cJSON_IsString(payload_sha)
        || !utils_is_hex_string(payload_sha->valuestring, 64U) || !cJSON_IsString(url)
        || std::strlen(url->valuestring) >= sizeof(out_page->protocol.frame_url)
        || !cJSON_IsString(path) || std::strncmp(path->valuestring, "display/pages/", 14U) != 0
        || std::strstr(path->valuestring, "..") != nullptr
        || std::strlen(path->valuestring) >= sizeof(out_page->file_path))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    std::memset(out_page, 0, sizeof(*out_page));
    utils_copy_string(out_page->protocol.page_id,
                      sizeof(out_page->protocol.page_id),
                      page_id->valuestring);
    utils_copy_string(out_page->protocol.content_version,
                      sizeof(out_page->protocol.content_version),
                      version->valuestring);
    out_page->protocol.file_size = static_cast<uint32_t>(size->valuedouble);
    out_page->protocol.crc32 = static_cast<uint32_t>(std::strtoul(crc->valuestring, nullptr, 16));
    utils_copy_string(out_page->protocol.sha256,
                      sizeof(out_page->protocol.sha256),
                      sha->valuestring);
    utils_copy_string(out_page->protocol.payload_sha256,
                      sizeof(out_page->protocol.payload_sha256),
                      payload_sha->valuestring);
    utils_copy_string(out_page->protocol.frame_url,
                      sizeof(out_page->protocol.frame_url),
                      url->valuestring);
    utils_copy_string(out_page->file_path, sizeof(out_page->file_path), path->valuestring);
    return ESP_OK;
}

/**
 * @brief 加载一个不可变本地集合 Manifest
 */
static esp_err_t display_collection_storage_load_manifest(
    const char *collection_version, display_collection_snapshot_t *out_snapshot,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> *out_pages)
{
    char path[DISPLAY_COLLECTION_PATH_MAX];
    const int written = std::snprintf(path,
                                      sizeof(path),
                                      "display/collections/%s.json",
                                      collection_version);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = nullptr;
    esp_err_t error = display_collection_storage_read_json(path, &root);
    if (error != ESP_OK)
    {
        return error;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "collection_version");
    const cJSON *default_page = cJSON_GetObjectItemCaseSensitive(root, "default_page");
    const cJSON *next_refresh_at = cJSON_GetObjectItemCaseSensitive(root, "next_refresh_at");
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    const int page_count = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
    if (!cJSON_IsNumber(schema) || schema->valueint != DISPLAY_COLLECTION_STORAGE_SCHEMA_VERSION
        || !cJSON_IsString(version) || std::strcmp(version->valuestring, collection_version) != 0
        || !cJSON_IsString(default_page) || default_page->valuestring[0] == '\0'
        || std::strlen(default_page->valuestring) >= sizeof(out_snapshot->default_page)
        || !cJSON_IsNumber(next_refresh_at)
        || next_refresh_at->valuedouble
               < static_cast<double>(DISPLAY_COLLECTION_MIN_REFRESH_TIMESTAMP)
        || next_refresh_at->valuedouble
               > static_cast<double>(DISPLAY_COLLECTION_MAX_REFRESH_TIMESTAMP)
        || next_refresh_at->valuedouble
               != static_cast<double>(static_cast<int64_t>(next_refresh_at->valuedouble))
        || page_count <= 0
        || page_count > static_cast<int>(DISPLAY_PROTOCOL_PAGE_MAX))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool default_found = false;
    for (int index = 0; index < page_count && error == ESP_OK; ++index)
    {
        error = display_collection_storage_parse_page(
            cJSON_GetArrayItem(pages, index), &(*out_pages)[static_cast<size_t>(index)]);
        if (error == ESP_OK
            && std::strcmp((*out_pages)[static_cast<size_t>(index)].protocol.page_id,
                           default_page->valuestring)
                   == 0)
        {
            default_found = true;
        }
    }
    if (error == ESP_OK && !default_found)
    {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error == ESP_OK)
    {
        out_snapshot->has_active = true;
        utils_copy_string(out_snapshot->active_collection,
                          sizeof(out_snapshot->active_collection),
                          collection_version);
        utils_copy_string(out_snapshot->default_page,
                          sizeof(out_snapshot->default_page),
                          default_page->valuestring);
        out_snapshot->page_count = static_cast<uint8_t>(page_count);
        out_snapshot->next_refresh_at_utc =
            static_cast<int64_t>(next_refresh_at->valuedouble);
    }
    cJSON_Delete(root);
    return error;
}

/**
 * @brief 为页面数组构造本地集合 Manifest JSON
 */
static cJSON *display_collection_storage_build_manifest(
    const display_protocol_manifest_t &manifest,
    const std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &pages)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == nullptr || array == nullptr
        || cJSON_AddNumberToObject(root,
                                  "schema_version",
                                  DISPLAY_COLLECTION_STORAGE_SCHEMA_VERSION)
               == nullptr
        || cJSON_AddStringToObject(root, "collection_version", manifest.collection_version)
               == nullptr
        || cJSON_AddStringToObject(root, "default_page", manifest.default_page) == nullptr
        || cJSON_AddNumberToObject(root,
                                  "next_refresh_at",
                                  static_cast<double>(manifest.next_refresh_at_utc))
               == nullptr)
    {
        cJSON_Delete(array);
        cJSON_Delete(root);
        return nullptr;
    }
    if (!cJSON_AddItemToObject(root, "pages", array))
    {
        cJSON_Delete(array);
        cJSON_Delete(root);
        return nullptr;
    }

    for (uint8_t index = 0U; index < manifest.page_count; ++index)
    {
        const display_collection_page_t &page = pages[index];
        char crc[9];
        std::snprintf(crc, sizeof(crc), "%08x", static_cast<unsigned>(page.protocol.crc32));
        cJSON *item = cJSON_CreateObject();
        if (item == nullptr
            || cJSON_AddStringToObject(item, "page_id", page.protocol.page_id) == nullptr
            || cJSON_AddStringToObject(item, "content_version", page.protocol.content_version)
                   == nullptr
            || cJSON_AddNumberToObject(item, "file_size", page.protocol.file_size) == nullptr
            || cJSON_AddStringToObject(item, "crc32", crc) == nullptr
            || cJSON_AddStringToObject(item, "sha256", page.protocol.sha256) == nullptr
            || cJSON_AddStringToObject(item, "payload_sha256", page.protocol.payload_sha256)
                   == nullptr
            || cJSON_AddStringToObject(item, "frame_url", page.protocol.frame_url) == nullptr
            || cJSON_AddStringToObject(item, "file_path", page.file_path) == nullptr)
        {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return nullptr;
        }
        cJSON_AddItemToArray(array, item);
    }
    return root;
}

esp_err_t display_collection_storage_recover(
    DisplayCollectionRecoveredState *out_state, display_collection_snapshot_t *out_snapshot,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> *out_pages)
{
    if (out_state == nullptr || out_snapshot == nullptr || out_pages == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_state = {};
    *out_snapshot = {};
    *out_pages = {};
    out_snapshot->next_refresh_at_utc = 0;

    esp_err_t error = display_collection_storage_prepare_directories();
    if (error != ESP_OK)
    {
        return error;
    }

    DisplayCollectionStoredState slots[2];
    const esp_err_t errors[2] = {
        display_collection_storage_load_state(0, &slots[0]),
        display_collection_storage_load_state(1, &slots[1]),
    };
    int8_t selected = -1;
    for (int8_t slot = 0; slot < 2; ++slot)
    {
        if (errors[slot] == ESP_OK
            && (selected < 0 || slots[slot].generation > slots[selected].generation))
        {
            selected = slot;
        }
    }
    if (selected < 0)
    {
        const bool first_is_metadata_error = errors[0] == ESP_ERR_NOT_FOUND
                                             || errors[0] == ESP_ERR_INVALID_RESPONSE
                                             || errors[0] == ESP_ERR_NOT_SUPPORTED;
        const bool second_is_metadata_error = errors[1] == ESP_ERR_NOT_FOUND
                                              || errors[1] == ESP_ERR_INVALID_RESPONSE
                                              || errors[1] == ESP_ERR_NOT_SUPPORTED;
        if (first_is_metadata_error && second_is_metadata_error)
        {
            if (errors[0] != ESP_ERR_NOT_FOUND || errors[1] != ESP_ERR_NOT_FOUND)
            {
                ESP_LOGW(TAG, "忽略损坏或旧版的显示状态，等待同步新的四灰阶集合");
            }
            return ESP_OK;
        }
        return !first_is_metadata_error ? errors[0] : errors[1];
    }

    error = display_collection_storage_load_manifest(slots[selected].active_collection,
                                                     out_snapshot,
                                                     out_pages);
    if (error == ESP_ERR_NOT_FOUND || error == ESP_ERR_INVALID_RESPONSE
        || error == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "忽略损坏或旧版的活动集合 Manifest，等待重新同步");
        return ESP_OK;
    }
    if (error != ESP_OK)
    {
        return error;
    }
    out_snapshot->generation = slots[selected].generation;
    utils_copy_string(out_snapshot->previous_collection,
                      sizeof(out_snapshot->previous_collection),
                      slots[selected].previous_collection);
    out_state->generation  = slots[selected].generation;
    out_state->active_slot = selected;
    utils_copy_string(out_state->active_collection,
                      sizeof(out_state->active_collection),
                      slots[selected].active_collection);
    utils_copy_string(out_state->previous_collection,
                      sizeof(out_state->previous_collection),
                      slots[selected].previous_collection);
    return ESP_OK;
}

esp_err_t display_collection_storage_read_page(const display_collection_page_t &page,
                                               uint8_t *buffer, size_t capacity,
                                               size_t *out_size)
{
    return device_sd_read_file(page.file_path, buffer, capacity, out_size);
}

esp_err_t display_collection_storage_store_page(const display_collection_page_t &page,
                                                const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char temporary[DISPLAY_COLLECTION_PATH_MAX];
    const int written = std::snprintf(temporary,
                                      sizeof(temporary),
                                      "display/tmp/%s-%s.ppf.tmp",
                                      page.protocol.page_id,
                                      page.protocol.content_version);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(temporary))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t remove_temp = device_sd_remove_file(temporary);
    if (remove_temp != ESP_OK && remove_temp != ESP_ERR_NOT_FOUND)
    {
        return remove_temp;
    }
    esp_err_t error = device_sd_write_file(temporary, data, size);
    if (error != ESP_OK)
    {
        return error;
    }
    const esp_err_t remove_final = device_sd_remove_file(page.file_path);
    if (remove_final != ESP_OK && remove_final != ESP_ERR_NOT_FOUND)
    {
        (void) device_sd_remove_file(temporary);
        return remove_final;
    }
    error = device_sd_rename_file(temporary, page.file_path);
    if (error != ESP_OK)
    {
        (void) device_sd_remove_file(temporary);
    }
    return error;
}

esp_err_t display_collection_storage_commit(
    const display_protocol_manifest_t &manifest,
    const std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &pages,
    const display_collection_snapshot_t &previous_snapshot, int8_t previous_slot,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &scratch_pages,
    uint64_t *out_generation, int8_t *out_slot)
{
    if (out_generation == nullptr || out_slot == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *manifest_root = display_collection_storage_build_manifest(manifest, pages);
    if (manifest_root == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    char manifest_path[DISPLAY_COLLECTION_PATH_MAX];
    char manifest_temp[DISPLAY_COLLECTION_PATH_MAX];
    const int manifest_written = std::snprintf(manifest_path,
                                               sizeof(manifest_path),
                                               "display/collections/%s.json",
                                               manifest.collection_version);
    const int temp_written = std::snprintf(manifest_temp,
                                           sizeof(manifest_temp),
                                           "display/tmp/%s.collection.tmp",
                                           manifest.collection_version);
    if (manifest_written <= 0 || static_cast<size_t>(manifest_written) >= sizeof(manifest_path)
        || temp_written <= 0 || static_cast<size_t>(temp_written) >= sizeof(manifest_temp))
    {
        cJSON_Delete(manifest_root);
        return ESP_ERR_INVALID_SIZE;
    }
    (void) device_sd_remove_file(manifest_temp);
    esp_err_t error = display_collection_storage_write_json(manifest_temp, manifest_root);
    cJSON_Delete(manifest_root);
    if (error != ESP_OK)
    {
        return error;
    }
    (void) device_sd_remove_file(manifest_path);
    error = device_sd_rename_file(manifest_temp, manifest_path);
    if (error != ESP_OK)
    {
        (void) device_sd_remove_file(manifest_temp);
        return error;
    }

    display_collection_snapshot_t verified_manifest = {};
    scratch_pages = {};
    error = display_collection_storage_load_manifest(manifest.collection_version,
                                                     &verified_manifest,
                                                     &scratch_pages);
    if (error != ESP_OK || verified_manifest.page_count != manifest.page_count)
    {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }

    const uint64_t generation = previous_snapshot.generation + 1U;
    const int8_t slot = previous_slot == 0 ? 1 : 0;
    cJSON *state_root = cJSON_CreateObject();
    if (state_root == nullptr
        || cJSON_AddNumberToObject(state_root,
                                  "schema_version",
                                  DISPLAY_COLLECTION_STORAGE_SCHEMA_VERSION)
               == nullptr
        || cJSON_AddNumberToObject(state_root, "generation", static_cast<double>(generation))
               == nullptr
        || cJSON_AddStringToObject(state_root,
                                  "active_collection",
                                  manifest.collection_version)
               == nullptr
        || cJSON_AddStringToObject(state_root,
                                  "previous_collection",
                                  previous_snapshot.has_active
                                      ? previous_snapshot.active_collection
                                      : "")
               == nullptr)
    {
        cJSON_Delete(state_root);
        return ESP_ERR_NO_MEM;
    }

    char state_temp[64];
    std::snprintf(state_temp, sizeof(state_temp), "display/tmp/state_%c.json.tmp", slot == 0 ? 'a' : 'b');
    (void) device_sd_remove_file(state_temp);
    error = display_collection_storage_write_json(state_temp, state_root);
    cJSON_Delete(state_root);
    if (error != ESP_OK)
    {
        return error;
    }

    cJSON *verified_root = nullptr;
    error = display_collection_storage_read_json(state_temp, &verified_root);
    DisplayCollectionStoredState verified;
    if (error == ESP_OK)
    {
        error = display_collection_storage_parse_state(verified_root, &verified);
    }
    cJSON_Delete(verified_root);
    if (error != ESP_OK || verified.generation != generation
        || std::strcmp(verified.active_collection, manifest.collection_version) != 0)
    {
        (void) device_sd_remove_file(state_temp);
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }

    (void) device_sd_remove_file(DISPLAY_COLLECTION_STATE_PATHS[slot]);
    error = device_sd_rename_file(state_temp, DISPLAY_COLLECTION_STATE_PATHS[slot]);
    if (error != ESP_OK)
    {
        (void) device_sd_remove_file(state_temp);
        return error;
    }
    *out_generation = generation;
    *out_slot       = slot;
    return ESP_OK;
}

/**
 * @brief 判断文件路径是否仍被保留集合引用
 */
static bool display_collection_storage_path_is_retained(
    const char *path, const display_collection_page_t *pages, uint8_t page_count)
{
    for (uint8_t index = 0U; index < page_count; ++index)
    {
        if (std::strcmp(path, pages[index].file_path) == 0)
        {
            return true;
        }
    }
    return false;
}

void display_collection_storage_cleanup_obsolete(
    const char *obsolete_collection, const display_collection_page_t *retained_previous,
    uint8_t retained_previous_count, const display_collection_page_t *retained_active,
    uint8_t retained_active_count,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &scratch_pages)
{
    if (obsolete_collection == nullptr || obsolete_collection[0] == '\0')
    {
        return;
    }
    display_collection_snapshot_t obsolete_snapshot = {};
    scratch_pages = {};
    const esp_err_t load_error = display_collection_storage_load_manifest(
        obsolete_collection, &obsolete_snapshot, &scratch_pages);
    if (load_error == ESP_OK)
    {
        for (uint8_t index = 0U; index < obsolete_snapshot.page_count; ++index)
        {
            const char *path = scratch_pages[index].file_path;
            if (!display_collection_storage_path_is_retained(
                    path, retained_previous, retained_previous_count)
                && !display_collection_storage_path_is_retained(
                    path, retained_active, retained_active_count))
            {
                const esp_err_t remove_error = device_sd_remove_file(path);
                if (remove_error != ESP_OK && remove_error != ESP_ERR_NOT_FOUND)
                {
                    ESP_LOGW(TAG, "清理旧页面失败: %s", esp_err_to_name(remove_error));
                }
            }
        }
    }

    char manifest_path[DISPLAY_COLLECTION_PATH_MAX];
    const int written = std::snprintf(manifest_path,
                                      sizeof(manifest_path),
                                      "display/collections/%s.json",
                                      obsolete_collection);
    if (written > 0 && static_cast<size_t>(written) < sizeof(manifest_path))
    {
        const esp_err_t remove_error = device_sd_remove_file(manifest_path);
        if (remove_error != ESP_OK && remove_error != ESP_ERR_NOT_FOUND)
        {
            ESP_LOGW(TAG, "清理旧集合 Manifest 失败: %s", esp_err_to_name(remove_error));
        }
    }
}
