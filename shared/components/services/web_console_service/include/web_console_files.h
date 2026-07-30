/**
 * @file web_console_files.h
 * @brief 网页控制台 Files 模块的可移植存储配置接口
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Files 模块可复制的挂载根路径最大长度，不含结尾 NUL。 */
#define WEB_CONSOLE_FILES_MOUNT_ROOT_MAX_LENGTH 127U

/** Files 模块可复制的工作区名称最大长度，不含结尾 NUL。 */
#define WEB_CONSOLE_FILES_WORKSPACE_NAME_MAX_LENGTH 63U

    /**
     * @brief Files 模块使用的存储容量快照
     */
    typedef struct
    {
        uint64_t total_bytes; /**< 文件系统总容量，单位字节 */
        uint64_t free_bytes;  /**< 文件系统当前可用容量，单位字节 */
    } web_console_files_capacity_t;

    /**
     * @brief 复制 Files 模块所用存储的容量快照
     *
     * 回调在 HTTPD handler 所在的普通 Task 上下文同步执行，允许短时阻塞，但不得回调
     * Console API 形成重入。`out_capacity` 只在本次回调期间有效，Provider 不得保存。
     * 成功返回时 `free_bytes` 不得大于 `total_bytes`。
     *
     * @param[in] context Provider 的长期借用上下文，可为空
     * @param[out] out_capacity 调用方提供的容量输出
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；其他值由存储实现定义
     */
    typedef esp_err_t (*web_console_files_get_capacity_copy_cb_t)(void *context,
                                                                  web_console_files_capacity_t *out_capacity);

    /**
     * @brief Files 模块使用的存储能力提供者
     *
     * 初始化会复制挂载根路径和函数指针；`context` 仅长期借用，必须保持有效直到 Service
     * 成功反初始化。挂载根在该期间必须保持挂载且不得改变指向。当前 Files 内核要求 VFS
     * 不支持符号链接，并要求调用方串行化 Console 外部写入；尤其不能在 Files 完成
     * `stat()` 目标不存在后、执行 `rename()` 前由其他写入者创建同名目标。底层
     * `rename()` 可以采用覆盖语义，但在上述独占写入约束下不得出现未确认覆盖。
     */
    typedef struct
    {
        const char                              *mount_root;        /**< 绝对规范 VFS 根；除 `/` 外不得以 `/` 结尾 */
        web_console_files_get_capacity_copy_cb_t get_capacity_copy; /**< 容量快照回调 */
        void                                    *context;           /**< 回调的长期借用上下文 */
    } web_console_files_storage_provider_t;

    /**
     * @brief Files 模块配置
     *
     * `mount_root` 与 `workspace_name` 在初始化期间复制；Storage Provider 的 `context`
     * 长期借用到 Service 成功反初始化。
     */
    typedef struct
    {
        web_console_files_storage_provider_t storage; /**< 存储能力提供者 */
        const char *workspace_name; /**< 非空、非 `.`/`..` 且不含分隔符的单个事务工作区路径段 */
        uint64_t upload_max_bytes; /**< 非零且不超过目标平台 `SIZE_MAX` 的单次上传正文上限 */
        uint64_t reserved_free_bytes; /**< 上传后保留量；与 `upload_max_bytes` 求和不得溢出 */
    } web_console_files_config_t;

#ifdef __cplusplus
}
#endif
