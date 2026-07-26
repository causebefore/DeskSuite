/**
 * @file display_collection_service_internal.hpp
 * @brief 集合 Service 的私有 Runtime 与存储接口
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "display_collection_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** @brief A/B 状态槽恢复结果 */
struct DisplayCollectionRecoveredState
{
    uint64_t generation = 0U;                               /**< 状态代数 */
    int8_t   active_slot = -1;                              /**< 0=A，1=B */
    char     active_collection[DISPLAY_PROTOCOL_VERSION_MAX] = {};   /**< 活动集合 */
    char     previous_collection[DISPLAY_PROTOCOL_VERSION_MAX] = {}; /**< 上一集合 */
};

/** @brief 集合恢复与同步事务独占的固定容量 PSRAM 工作区 */
struct DisplayCollectionWorkspace
{
    display_protocol_manifest_t manifest = {}; /**< 当前服务端 Manifest */
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> active_pages = {}; /**< 活动页 */
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> previous_pages = {}; /**< 事务前页面 */
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> next_pages = {}; /**< 待提交页面 */
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> scratch_pages = {}; /**< 存储临时页 */
};

/** @brief 集合 Service 进程期唯一 Runtime */
class DisplayCollectionRuntime final
{
public:
    DisplayCollectionRuntime() = default;
    DisplayCollectionRuntime(const DisplayCollectionRuntime &) = delete;
    DisplayCollectionRuntime &operator=(const DisplayCollectionRuntime &) = delete;
    DisplayCollectionRuntime(DisplayCollectionRuntime &&) = delete;
    DisplayCollectionRuntime &operator=(DisplayCollectionRuntime &&) = delete;

    bool      initialized = false; /**< 生命周期初始化标记 */
    bool      syncing     = false; /**< 同步事务占用标记 */
    SemaphoreHandle_t lock = nullptr; /**< 快照与回调锁 */
    uint8_t  *buffer       = nullptr; /**< 单页 PSRAM 工作缓冲区 */
    size_t    buffer_size  = 0U;     /**< 当前工作数据长度 */
    DisplayCollectionWorkspace *workspace = nullptr; /**< PSRAM 元数据工作区 */
    display_collection_snapshot_t snapshot = {}; /**< 对外快照 */
    int8_t active_slot = -1; /**< 最近有效 A/B 槽 */
    display_collection_commit_cb_t commit_callback = nullptr; /**< 借用提交回调 */
    void *commit_context = nullptr; /**< 借用回调上下文 */
};

/** @brief 集合 Service 唯一 Runtime */
extern DisplayCollectionRuntime g_display_collection_runtime;

/**
 * @brief 创建集合存储目录并从 A/B 状态恢复活动集合
 *
 * @param[out] out_state 恢复出的状态槽信息
 * @param[out] out_snapshot 恢复出的活动快照
 * @param[out] out_pages 恢复出的活动页面数组
 * @return ESP_OK 成功或尚无状态；其他值表示 SD、JSON 或校验错误
 */
esp_err_t display_collection_storage_recover(
    DisplayCollectionRecoveredState *out_state, display_collection_snapshot_t *out_snapshot,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> *out_pages);

/**
 * @brief 读取一个本地页面文件
 *
 * @param[in] page 页面描述
 * @param[out] buffer 输出缓冲区
 * @param[in] capacity 缓冲区容量
 * @param[out] out_size 实际长度
 * @return ESP_OK 成功；或 device_sd 错误码
 */
esp_err_t display_collection_storage_read_page(const display_collection_page_t &page,
                                               uint8_t *buffer, size_t capacity,
                                               size_t *out_size);

/**
 * @brief 使用临时文件写入并发布一个页面文件
 *
 * @param[in] page 页面描述
 * @param[in] data 完整 PPF2 数据
 * @param[in] size 数据长度
 * @return ESP_OK 成功；或 device_sd 错误码
 */
esp_err_t display_collection_storage_store_page(const display_collection_page_t &page,
                                                const uint8_t *data, size_t size);

/**
 * @brief 写入集合 Manifest 和非活动状态槽并回读确认
 *
 * @param[in] manifest 新服务端 Manifest
 * @param[in] pages 新本地页面数组
 * @param[in] previous_snapshot 当前活动快照，将成为上一集合
 * @param[in] previous_slot 当前有效状态槽
 * @param[in,out] scratch_pages 提交回读校验使用的固定容量临时数组
 * @param[out] out_generation 新状态代数
 * @param[out] out_slot 新有效状态槽
 * @return ESP_OK 提交完成；或编码、SD、回读错误码
 */
esp_err_t display_collection_storage_commit(
    const display_protocol_manifest_t &manifest,
    const std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &pages,
    const display_collection_snapshot_t &previous_snapshot, int8_t previous_slot,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &scratch_pages,
    uint64_t *out_generation, int8_t *out_slot);

/**
 * @brief 尽力清理不再保留的旧集合文件
 *
 * @param[in] obsolete_collection 待清理集合版本；空字符串表示无需清理
 * @param[in] retained_previous 当前保留的上一集合页面
 * @param[in] retained_previous_count 上一集合页面数
 * @param[in] retained_active 当前保留的活动集合页面
 * @param[in] retained_active_count 活动集合页面数
 * @param[in,out] scratch_pages 读取待清理集合使用的固定容量临时数组
 */
void display_collection_storage_cleanup_obsolete(
    const char *obsolete_collection, const display_collection_page_t *retained_previous,
    uint8_t retained_previous_count, const display_collection_page_t *retained_active,
    uint8_t retained_active_count,
    std::array<display_collection_page_t, DISPLAY_PROTOCOL_PAGE_MAX> &scratch_pages);
