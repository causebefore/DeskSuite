/**
 * @file system_info.h
 * @brief DeskMate 固件、设备标识和运行资源只读信息
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        bool     valid;
        char     version[32];
        char     build_time[24];
        uint32_t uptime_sec;
        uint32_t sram_total_kb;
        uint32_t sram_free_kb;
        uint8_t  sram_used_percent;
        uint32_t psram_total_kb;
        uint32_t psram_free_kb;
        uint8_t  psram_used_percent;
        uint16_t cpu_mhz;
    } system_info_snapshot_t;

    /**
     * @brief 同步复制采集当前系统信息快照
     * @param[out] out_snapshot 快照输出，仅在 ESP_OK 时有效
     */
    esp_err_t system_info_get_snapshot_copy(system_info_snapshot_t *out_snapshot);

    /**
     * @brief 基于 Wi-Fi Station MAC 生成稳定设备 ID
     * @param[out] out 设备 ID 输出缓冲区
     * @param[in] out_len 输出缓冲区容量
     */
    esp_err_t system_info_get_device_id(char *out, size_t out_len);

    /** @brief 借用当前固件版本只读字符串，生命周期覆盖整个进程 */
    const char *system_info_get_firmware_version_borrow(void);

    /** @brief 借用当前启动对应的静态重启原因字符串 */
    const char *system_info_get_reset_reason_borrow(void);

#ifdef __cplusplus
}
#endif
