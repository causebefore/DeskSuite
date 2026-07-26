/**
 * @file device_storage.h
 * @brief 与 SD 型号、SPI、GPIO 和板型无关的同步块存储能力
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 外部块存储设备的容量与扇区信息 */
    typedef struct
    {
        uint32_t sector_count;      /**< 可访问扇区总数 */
        uint32_t sector_size_bytes; /**< 单个扇区大小，单位字节 */
        uint64_t capacity_bytes;    /**< 总容量，单位字节 */
    } device_storage_info_t;

    /**
     * @brief 初始化外部块存储设备及同步资源
     *
     * 本函数在调用者上下文同步创建事务锁并探测设备，返回时设备已可执行扇区访问；
     * 重复调用成功返回 ESP_OK。初始化和反初始化必须由调用方串行执行。
     *
     * @return ESP_OK 初始化完成；ESP_ERR_NO_MEM 无法创建同步资源；或底层设备错误码
     */
    esp_err_t device_storage_init(void);

    /**
     * @brief 释放外部块存储设备及同步资源
     *
     * 本函数在调用者上下文同步等待当前块事务结束，再释放底层设备和事务锁。调用前必须先
     * 卸载使用该设备的文件系统，并确保不会再发起新的块操作。底层清理失败时组件保持已
     * 初始化状态，调用方可以再次调用本函数重试。
     *
     * @return ESP_OK 已释放；ESP_ERR_INVALID_STATE 尚未初始化；或底层清理错误码
     */
    esp_err_t device_storage_deinit(void);

    /**
     * @brief 检查外部块存储设备当前是否响应
     *
     * 本函数只能在允许阻塞的 Task 上下文调用，会等待事务锁并同步完成底层状态检查；
     * 不得从 ISR 调用。
     *
     * @return ESP_OK 设备可用；ESP_ERR_INVALID_STATE 尚未初始化；或底层设备错误码
     */
    esp_err_t device_storage_check_ready(void);

    /**
     * @brief 复制外部块存储设备信息
     *
     * 本函数只能在允许阻塞的 Task 上下文调用，会等待事务锁；不返回内部对象或底层句柄。
     *
     * @param[out] out_info 设备信息，仅在返回 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或底层设备错误码
     */
    esp_err_t device_storage_get_info_copy(device_storage_info_t *out_info);

    /**
     * @brief 同步读取连续扇区
     *
     * 本函数只能在允许阻塞的 Task 上下文调用，会与其他 Device 存储事务串行执行。
     * 函数返回前完成读取且不保留输出缓冲区指针。
     *
     * @param[in] start_sector 起始扇区编号
     * @param[in] sector_count 连续读取的扇区数，必须大于 0
     * @param[out] out_data 输出缓冲区，容量至少为扇区大小乘扇区数
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或范围无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或底层读取错误码
     */
    esp_err_t device_storage_read_sectors(uint32_t start_sector, size_t sector_count, void *out_data);

    /**
     * @brief 同步写入连续扇区
     *
     * 本函数只能在允许阻塞的 Task 上下文调用，会与其他 Device 存储事务串行执行。
     * 函数返回前完成写入且不保留输入缓冲区指针。
     *
     * @param[in] start_sector 起始扇区编号
     * @param[in] sector_count 连续写入的扇区数，必须大于 0
     * @param[in] data 输入缓冲区，容量至少为扇区大小乘扇区数
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或范围无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或底层写入错误码
     */
    esp_err_t device_storage_write_sectors(uint32_t start_sector, size_t sector_count, const void *data);

#ifdef __cplusplus
}
#endif
