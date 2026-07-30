/**
 * @file web_console_provider.h
 * @brief 网页控制台 Settings 与 Status 模块的可移植 Provider 契约
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

/** 单次构建最多装配的 Settings 分区数。 */
#define WEB_CONSOLE_SETTINGS_PROVIDER_MAX_COUNT 4U

/** 单次构建最多装配的 Status 分区数。 */
#define WEB_CONSOLE_STATUS_PROVIDER_MAX_COUNT 8U

/** 单个分区最多公开的字段数。 */
#define WEB_CONSOLE_PROVIDER_MAX_FIELDS_PER_SECTION 12U

/** 单个枚举字段最多公开的候选值数。 */
#define WEB_CONSOLE_PROVIDER_MAX_ENUM_VALUES 8U

/** 分区稳定 ID 最大长度，不含结尾 NUL。 */
#define WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH 31U

/** 字段稳定 ID 最大长度，不含结尾 NUL。 */
#define WEB_CONSOLE_PROVIDER_FIELD_ID_MAX_LENGTH 31U

/** 分区、字段或枚举值显示标签最大 UTF-8 字节数，不含结尾 NUL。 */
#define WEB_CONSOLE_PROVIDER_LABEL_MAX_LENGTH 63U

/** 字符串字段值最大 UTF-8 字节数，不含结尾 NUL。 */
#define WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH 95U

    /**
     * @brief 网页控制台字段类型
     */
    typedef enum
    {
        WEB_CONSOLE_FIELD_TYPE_BOOL = 0, /**< 布尔值 */
        WEB_CONSOLE_FIELD_TYPE_INT32,    /**< 有符号 32 位整数 */
        WEB_CONSOLE_FIELD_TYPE_UINT32,   /**< 无符号 32 位整数 */
        WEB_CONSOLE_FIELD_TYPE_STRING,   /**< 有界 NUL 结尾 UTF-8 字符串 */
        WEB_CONSOLE_FIELD_TYPE_ENUM,     /**< 描述符枚举表中的一个有符号整数值 */
    } web_console_field_type_t;

    /**
     * @brief 设置成功后的生效事实
     */
    typedef enum
    {
        WEB_CONSOLE_FIELD_EFFECT_NONE = 0,        /**< 仅供 Status 字段使用，不描述设置生效方式 */
        WEB_CONSOLE_FIELD_EFFECT_IMMEDIATE,       /**< 所有者完成更新时立即生效 */
        WEB_CONSOLE_FIELD_EFFECT_NEXT_TRANSACTION, /**< 下一次领域事务开始时生效 */
        WEB_CONSOLE_FIELD_EFFECT_RECONNECT,       /**< 下次重连后生效 */
        WEB_CONSOLE_FIELD_EFFECT_RESTART,         /**< 设备重启后生效 */
        WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,       /**< 仅在领域能力空闲时允许更新 */
    } web_console_field_effect_t;

    /** 字段只允许读取，PATCH 必须拒绝。 */
#define WEB_CONSOLE_FIELD_ACCESS_READ_ONLY (1UL << 0U)

    /** 字段属于秘密；快照只能报告 `configured`，首版 HTTP 必须拒绝写入。 */
#define WEB_CONSOLE_FIELD_ACCESS_SECRET (1UL << 1U)

    /** 字段不返回值，只能报告 `configured`；Status 字段不得使用。 */
#define WEB_CONSOLE_FIELD_ACCESS_WRITE_ONLY (1UL << 2U)

    /** 字段访问属性位集合；零表示普通可读写设置。 */
    typedef uint32_t web_console_field_access_t;

    /**
     * @brief 枚举字段的一个允许值
     */
    typedef struct
    {
        int32_t     value; /**< 稳定协议值 */
        const char *label; /**< 面向用户的显示标签，初始化期间复制 */
    } web_console_field_enum_value_t;

    /**
     * @brief 一个管理字段的稳定元数据
     *
     * `minimum`、`maximum` 与 `step` 只用于整数字段；`max_length_bytes` 只用于字符串；
     * `enum_values` 与 `enum_value_count` 只用于枚举。所有字符串、枚举表和描述符数组只在
     * `web_console_service_init_borrow()` 调用期间读取并复制。
     */
    typedef struct
    {
        const char                    *id;               /**< 分区内唯一的稳定字段 ID */
        const char                    *label;            /**< 面向用户的显示标签 */
        web_console_field_type_t       type;             /**< 字段值类型 */
        web_console_field_access_t     access;           /**< 访问属性位集合 */
        web_console_field_effect_t     effect;           /**< Settings 生效事实；Status 必须为 NONE */
        int64_t                        minimum;          /**< 整数最小值 */
        int64_t                        maximum;          /**< 整数最大值 */
        uint32_t                       step;             /**< 整数步长，必须非零 */
        uint32_t                       max_length_bytes; /**< 字符串 UTF-8 字节上限 */
        const web_console_field_enum_value_t *enum_values; /**< 枚举值表 */
        size_t                         enum_value_count; /**< 枚举值数量 */
    } web_console_field_info_t;

    /**
     * @brief 一个字段的类型化值副本
     *
     * `type` 必须与同索引字段描述符一致。普通可读字段必须设置 `configured = true` 并写入
     * 对应 union 成员；Secret 或 Write-only 字段只设置 `configured`，其 union 必须保持全零。
     */
    typedef struct
    {
        web_console_field_type_t type;       /**< 值类型 */
        bool                     configured; /**< 字段是否已有有效配置 */
        union
        {
            bool     boolean_value; /**< BOOL 值 */
            int32_t  int32_value;   /**< INT32 或 ENUM 值 */
            uint32_t uint32_value;  /**< UINT32 值 */
            char     string_value[WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH + 1U]; /**< STRING 值 */
        } data;
    } web_console_field_value_t;

    /**
     * @brief Settings 分区的完整公开快照
     *
     * Console 在调用 Provider 前设置 `values` 与 `value_capacity`。Provider 不得改写这两个
     * 成员；成功返回时必须按描述符顺序完整写入所有值，并把 `value_count` 设置为字段数量，
     * 且不得保存输出指针。
     */
    typedef struct
    {
        uint64_t                   version;        /**< 设置版本，成功更新后必须严格递增且不得回绕 */
        web_console_field_value_t *values;         /**< 调用方提供的字段值数组 */
        size_t                     value_capacity; /**< `values` 可容纳的元素数 */
        size_t                     value_count;    /**< Provider 实际写入的元素数 */
    } web_console_settings_snapshot_t;

    /**
     * @brief Settings update 中的一个字段更新
     */
    typedef struct
    {
        size_t                    field_index; /**< Provider 描述符数组中的字段索引 */
        web_console_field_value_t value;       /**< 已通过通用类型和元数据校验的新值 */
    } web_console_settings_update_field_t;

    /**
     * @brief 基于版本的 Settings update
     *
     * `fields` 仅在回调期间有效；异步请求回调必须在返回前复制所需字段，不能保存此指针。
     */
    typedef struct
    {
        uint64_t                                  expected_version; /**< 浏览器读取快照时的版本 */
        const web_console_settings_update_field_t *fields;          /**< 不重复的字段更新数组 */
        size_t                                    field_count;      /**< 更新字段数量 */
    } web_console_settings_update_t;

    /**
     * @brief 一次异步 Settings 更新的状态
     */
    typedef enum
    {
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING = 0, /**< 已接受，所有者尚未形成最终结果 */
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED,   /**< 已成功形成新设置版本 */
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED,      /**< 已形成失败结果，具体事实见 `error` */
    } web_console_settings_update_state_t;

    /**
     * @brief 一次异步 Settings 更新的最终或当前结果
     */
    typedef struct
    {
        web_console_settings_update_state_t state;   /**< 当前请求状态 */
        uint64_t                            version; /**< 当前或最终设置版本 */
        esp_err_t                           error;   /**< PENDING/SUCCEEDED 时为 ESP_OK；FAILED 时为最终错误 */
    } web_console_settings_update_result_t;

    /**
     * @brief 复制 Settings 分区完整公开快照
     *
     * 回调在 Console HTTPD 普通 Task 上下文、Core 锁外同步执行，只能进行有界内存读取和短时
     * 所有者加锁，不得执行持久化或网络 I/O，不得回调 Console。输出缓冲区只在回调期间有效。
     *
     * @param[in] context Provider 的长期借用上下文，可为空
     * @param[in,out] out_snapshot Console 提供缓冲区的快照输出
     * @return ESP_OK 快照完整有效；ESP_ERR_INVALID_ARG 输出无效；其他值由领域所有者定义
     */
    typedef esp_err_t (*web_console_settings_get_snapshot_copy_cb_t)(
        void *context,
        web_console_settings_snapshot_t *out_snapshot);

    /**
     * @brief 由领域所有者同步校验一个完整语义 update
     *
     * Console 已完成字段存在性、访问属性、类型、范围、步长、枚举和字符串长度校验；本回调
     * 继续做早期版本、字段组合和领域状态校验，不得修改产品状态或执行 I/O。此结果不是提交
     * 保证：`request_update_copy` 或领域执行点仍必须原子地重新校验 `expected_version`，避免
     * 两次回调之间发生的设置变化被旧更新覆盖。版本冲突返回 `ESP_ERR_INVALID_VERSION`，
     * 其他领域约束使用对应 `esp_err_t`。
     *
     * @param[in] context Provider 的长期借用上下文，可为空
     * @param[in] update 调用期间借用的更新
     * @return ESP_OK 可以提交；其他值表示拒绝原因
     */
    typedef esp_err_t (*web_console_settings_validate_update_cb_t)(
        void *context,
        const web_console_settings_update_t *update);

    /**
     * @brief 向领域所有者异步提交 Settings 更新副本
     *
     * 返回 ESP_OK 只表示 Provider 已在返回前复制并接受请求；回调必须在接受点重新校验
     * `expected_version`，或保证领域执行点在修改前原子重检并把冲突发布为该请求的
     * `FAILED / ESP_ERR_INVALID_VERSION` 最终结果。持久化和运行时应用的最终结果必须由
     * `get_update_result_copy` 查询。回调在 Console HTTPD 普通 Task 上下文、Core 锁外执行，
     * 不得长期阻塞、执行更新本身或回调 Console。
     *
     * @param[in] context Provider 的长期借用上下文，可为空
     * @param[in] update 调用期间借用的更新
     * 请求 ID 在同一 Provider context 生命周期内必须非零、单调递增且永不复用或回绕；
     * ID 耗尽时必须拒绝新请求。每个终态结果至少保留到下一请求被接受，不能在浏览器首次查询
     * 前立即淘汰。
     *
     * @param[out] out_request_id 成功时写入符合上述约束的请求 ID
     * @return ESP_OK 请求已接受；其他值表示同步拒绝
     */
    typedef esp_err_t (*web_console_settings_request_update_copy_cb_t)(
        void *context,
        const web_console_settings_update_t *update,
        uint64_t *out_request_id);

    /**
     * @brief 复制一次已接受 Settings 更新的当前结果
     *
     * 回调在 Console HTTPD 普通 Task 上下文、Core 锁外同步执行，只能进行有界内存读取和短时
     * 所有者加锁。`PENDING` 和 `SUCCEEDED` 的 `error` 必须为 `ESP_OK`；`FAILED` 的 `error`
     * 必须为非 `ESP_OK` 的最终事实。未知或按上述保留规则已淘汰的请求返回
     * `ESP_ERR_NOT_FOUND`。
     *
     * @param[in] context Provider 的长期借用上下文，可为空
     * @param[in] request_id `request_update_copy` 返回的非零请求 ID
     * @param[out] out_result 当前结果副本
     * @return ESP_OK 结果有效；ESP_ERR_NOT_FOUND 请求未知；其他值由领域所有者定义
     */
    typedef esp_err_t (*web_console_settings_get_update_result_copy_cb_t)(
        void *context,
        uint64_t request_id,
        web_console_settings_update_result_t *out_result);

    /**
     * @brief 一个可移植 Settings 分区 Provider
     *
     * 初始化复制 Provider、字段元数据、字符串、枚举值和回调函数；仅 `context` 长期借用到
     * Service 成功反初始化。Provider 所属组件继续拥有数据、校验、持久化和运行时应用。
     */
    typedef struct
    {
        const char                                          *section_id; /**< 全部 Settings Provider 中唯一的稳定 ID */
        const char                                          *label;      /**< 面向用户的分区标签 */
        const web_console_field_info_t                       *fields;     /**< 固定字段描述符数组 */
        size_t                                               field_count; /**< 字段数量 */
        web_console_settings_get_snapshot_copy_cb_t          get_snapshot_copy; /**< 快照回调 */
        web_console_settings_validate_update_cb_t            validate_update; /**< 领域校验回调 */
        web_console_settings_request_update_copy_cb_t        request_update_copy; /**< 异步提交回调 */
        web_console_settings_get_update_result_copy_cb_t     get_update_result_copy; /**< 结果查询回调 */
        void                                                *context;    /**< 长期借用上下文 */
    } web_console_settings_provider_t;

    /**
     * @brief Status 分区的一次有界只读运行摘要
     *
     * Console 在调用 Provider 前设置 `values` 与 `value_capacity`。Provider 不得改写这两个
     * 成员；成功返回时必须按描述符顺序完整写入所有值，并设置字段数量，且不得保存输出指针。
     */
    typedef struct
    {
        uint64_t                   version;        /**< 领域摘要版本；无版本来源时可为零 */
        web_console_field_value_t *values;         /**< 调用方提供的字段值数组 */
        size_t                     value_capacity; /**< `values` 可容纳的元素数 */
        size_t                     value_count;    /**< Provider 实际写入的元素数 */
    } web_console_section_status_t;

    /**
     * @brief 复制 Status 分区当前运行摘要
     *
     * 回调在 Console HTTPD 普通 Task 上下文、Core 锁外同步执行，只能执行有界事实读取和短时
     * 所有者加锁；不得执行网络、文件或持久化 I/O，不得修改产品状态或回调 Console。
     *
     * @param[in] context Provider 的长期借用上下文，可为空
     * @param[in,out] out_status Console 提供缓冲区的运行摘要输出
     * @return ESP_OK 运行摘要完整有效；其他值由领域所有者定义
     */
    typedef esp_err_t (*web_console_status_get_status_copy_cb_t)(
        void *context,
        web_console_section_status_t *out_status);

    /**
     * @brief 一个可移植只读 Status 分区 Provider
     *
     * 初始化复制 Provider、字段元数据、字符串、枚举值和回调函数；仅 `context` 长期借用到
     * Service 成功反初始化。所有字段必须为 `READ_ONLY`，`effect` 必须为 `NONE`。
     */
    typedef struct
    {
        const char                              *section_id; /**< 全部 Status Provider 中唯一的稳定 ID */
        const char                              *label;      /**< 面向用户的分区标签 */
        const web_console_field_info_t           *fields;     /**< 固定字段描述符数组 */
        size_t                                   field_count; /**< 字段数量 */
        web_console_status_get_status_copy_cb_t  get_status_copy; /**< 运行摘要回调 */
        void                                    *context;    /**< 长期借用上下文 */
    } web_console_status_provider_t;

#ifdef __cplusplus
}
#endif
