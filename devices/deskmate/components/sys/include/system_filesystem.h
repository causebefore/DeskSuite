/**
 * @file system_filesystem.h
 * @brief 基于外部块存储设备的 FAT 文件系统生命周期与状态接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** 外部 SD 卡文件系统在 VFS 中的固定挂载点 */
#define SYSTEM_FILESYSTEM_MOUNT_POINT "/sdcard"

    /** @brief 已挂载文件系统的容量状态 */
    typedef struct
    {
        uint64_t total_bytes;           /**< 文件系统总容量，单位字节 */
        uint64_t free_bytes;            /**< 当前可用容量，单位字节 */
        bool     formatted_during_init; /**< 本次初始化是否执行过自动格式化 */
    } system_filesystem_info_t;

    /**
     * @brief 注册 Device 块设备并挂载 FAT 文件系统
     *
     * 本函数在调用者上下文同步完成 DiskIO、VFS 注册与 FAT 挂载，调用前必须已经成功初始化
     * Device 存储。挂载点固定为 SYSTEM_FILESYSTEM_MOUNT_POINT；重复调用成功返回 ESP_OK。
     * 初始化、反初始化和状态查询必须由调用方串行执行。
     *
     * @warning 仅当首次挂载返回 FR_NO_FILESYSTEM 或 FR_INT_ERR 时，本函数会清除整卡现有
     * 分区和数据，重新建立占满整卡的单分区，并以 16 KiB 簇、双 FAT 自动格式化。硬件未
     * 就绪或普通读写错误不会触发该流程；函数成功返回只表示新文件系统已重新挂载。
     *
     * @return ESP_OK 已挂载；ESP_ERR_INVALID_STATE Device 未初始化或存在未清理状态；
     *         ESP_ERR_NO_MEM 资源不足；ESP_FAIL FAT 操作失败；或 VFS 错误码
     */
    esp_err_t system_filesystem_init(void);

    /**
     * @brief 卸载 FAT 文件系统并注销 VFS 与 DiskIO
     *
     * 本函数在调用者上下文同步执行。调用前必须关闭 SYSTEM_FILESYSTEM_MOUNT_POINT 下的
     * 全部文件和目录，并阻止新的文件访问。若某一步清理失败，尚未释放的注册状态会被保留，
     * 调用方可以再次调用本函数重试。本函数不反初始化 Device 存储。
     *
     * @return ESP_OK 已完成；ESP_ERR_INVALID_STATE 尚未挂载；ESP_FAIL FatFs 卸载失败；
     *         或 VFS 注销错误码
     */
    esp_err_t system_filesystem_deinit(void);

    /**
     * @brief 判断外部 FAT 文件系统是否已经挂载
     *
     * 本函数只读取组件生命周期状态；调用方必须与初始化和反初始化串行执行。
     *
     * @return true 已挂载；false 未挂载
     */
    bool system_filesystem_is_mounted(void);

    /**
     * @brief 复制已挂载文件系统的容量状态
     *
     * 本函数在调用者上下文同步查询 VFS/FatFs。formatted_during_init 为 true 表示本次
     * 成功初始化过程中执行过会清除整卡数据的自动格式化。
     *
     * @param[out] out_info 文件系统信息，仅在返回 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；
     *         ESP_ERR_INVALID_STATE 尚未挂载；或 FatFs 查询错误码
     */
    esp_err_t system_filesystem_get_info_copy(system_filesystem_info_t *out_info);

#ifdef __cplusplus
}
#endif
