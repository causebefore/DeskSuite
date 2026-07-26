/**
 * @file device_sd.h
 * @brief SD 卡同步设备能力与文件访问接口
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

    /** @brief SD 卡物理与文件系统状态快照 */
    typedef struct
    {
        bool card_present; /**< true 表示卡槽检测到实体卡 */
        bool mounted;      /**< true 表示 FATFS 已成功挂载 */
    } device_sd_status_t;

    /**
     * @brief SD 卡插拔 ISR 通知回调
     *
     * 回调在 GPIO ISR 上下文执行，只能调用 FreeRTOS FromISR API，不得阻塞、记录日志或访问文件。
     *
     * @param[in] context 注册时传入的借用上下文
     */
    typedef void (*device_sd_detect_isr_cb_t)(void *context);

    /**
     * @brief 初始化 SD 卡槽和文件系统访问能力
     *
     * 同步初始化供电、检测 GPIO、共享 SPI2 总线与内部互斥锁，但不自动挂载。必须在墨水屏
     * 初始化前调用。本组件不创建或管理 Task。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_NO_MEM 内存不足；
     *         或 BSP 初始化错误码
     */
    esp_err_t device_sd_init(void);

    /**
     * @brief 设置或清除 SD 卡插拔 ISR 回调
     *
     * callback 和 context 的借用持续到下一次设置、传入 NULL 清除或 device_sd_deinit()。
     *
     * @param[in] callback ISR 回调；NULL 表示清除
     * @param[in] context 回调上下文
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP GPIO ISR 错误码
     */
    esp_err_t device_sd_set_detect_isr_callback_borrow(device_sd_detect_isr_cb_t callback,
                                                       void                     *context);

    /**
     * @brief 同步读取 SD 卡状态快照
     *
     * @param[out] out_status 状态快照，仅 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层 GPIO 错误码
     */
    esp_err_t device_sd_get_status_copy(device_sd_status_t *out_status);

    /**
     * @brief 同步挂载当前已插入 SD 卡的 FATFS
     *
     * 调用可能阻塞数秒，禁止在 ISR 或定时器回调中调用。
     *
     * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 未插卡；ESP_ERR_INVALID_STATE 状态非法；
     *         或 SDSPI、FATFS 错误码
     */
    esp_err_t device_sd_mount(void);

    /**
     * @brief 同步卸载 SD 卡 FATFS
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或未挂载；或 FATFS 错误码
     */
    esp_err_t device_sd_unmount(void);

    /**
     * @brief 同步覆盖写入 SD 卡文件
     *
     * relative_path 必须是相对 SD 根目录的安全路径，不允许绝对路径、空段、. 或 ..。
     * 输入缓冲只在调用期间借用，函数返回前已完成写入和关闭。
     *
     * @param[in] relative_path 相对路径
     * @param[in] data 待写数据
     * @param[in] size_bytes 数据长度
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或路径无效；ESP_ERR_INVALID_SIZE 路径过长；
     *         ESP_ERR_INVALID_STATE 尚未挂载；ESP_FAIL 文件 I/O 失败
     */
    esp_err_t device_sd_write_file(const char *relative_path, const void *data, size_t size_bytes);

    /**
     * @brief 同步读取完整 SD 卡文件到调用方缓冲区
     *
     * relative_path 规则与 device_sd_write_file() 相同。文件大于 capacity_bytes 时返回
     * ESP_ERR_INVALID_SIZE，out_size_bytes 仅在 ESP_OK 时有效。
     *
     * @param[in] relative_path 相对路径
     * @param[out] out_data 输出缓冲区
     * @param[in] capacity_bytes 输出缓冲区容量
     * @param[out] out_size_bytes 实际读取长度，仅 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或路径无效；ESP_ERR_INVALID_SIZE 容量不足；
     *         ESP_ERR_INVALID_STATE 尚未挂载；ESP_ERR_NOT_FOUND 文件不存在；ESP_FAIL 文件 I/O 失败
     */
    esp_err_t device_sd_read_file(const char *relative_path, void *out_data, size_t capacity_bytes,
                                  size_t *out_size_bytes);

    /**
     * @brief 在 SD 卡中创建一个目录
     *
     * relative_path 使用与文件读写相同的安全相对路径规则。目录已经存在时返回 ESP_OK，
     * 但同名普通文件存在时返回 ESP_ERR_INVALID_STATE。本函数不递归创建父目录。
     *
     * @param[in] relative_path 待创建目录的相对路径
     * @return ESP_OK 成功或目录已存在；ESP_ERR_INVALID_ARG 路径无效；
     *         ESP_ERR_INVALID_STATE 尚未挂载或同名普通文件存在；ESP_FAIL 文件系统错误
     */
    esp_err_t device_sd_make_directory(const char *relative_path);

    /**
     * @brief 原子改名一个 SD 卡文件
     *
     * 两个路径均使用安全相对路径规则，目标路径必须不存在。调用方必须确保源和目标位于
     * 同一已挂载文件系统。
     *
     * @param[in] source_relative_path 源文件相对路径
     * @param[in] target_relative_path 目标文件相对路径
     * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 源文件不存在；ESP_ERR_INVALID_STATE 尚未挂载或
     *         目标已存在；ESP_ERR_INVALID_ARG 路径无效；ESP_FAIL 文件系统错误
     */
    esp_err_t device_sd_rename_file(const char *source_relative_path,
                                    const char *target_relative_path);

    /**
     * @brief 删除一个 SD 卡文件
     *
     * @param[in] relative_path 待删除文件的相对路径
     * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 文件不存在；ESP_ERR_INVALID_STATE 尚未挂载；
     *         ESP_ERR_INVALID_ARG 路径无效；ESP_FAIL 文件系统错误
     */
    esp_err_t device_sd_remove_file(const char *relative_path);

    /**
     * @brief 释放 SD 卡槽、共享总线与内部同步资源
     *
     * 调用前必须清除 ISR 回调并成功卸载文件系统。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化、仍已挂载或仍有回调；
     *         或 BSP 清理错误码
     */
    esp_err_t device_sd_deinit(void);

#ifdef __cplusplus
}
#endif
