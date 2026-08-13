/**
 * @file web_console_service_transfer.hpp
 * @brief 网页文件服务各 C++ 传输实现共享的有界类型和私有接口
 */
#pragma once

#include <cstddef>

#include "web_console_files_internal.hpp"

/** @brief Authorization 请求头缓冲区容量，含 NUL 终止符 */
#define WEB_FILE_AUTHORIZATION_BUFFER_SIZE 48U
/** @brief 临时 JSON 或路径组装用草稿缓冲区容量 */
#define WEB_FILE_RESPONSE_SCRATCH_SIZE     1280U
/** @brief URL 路径单段最大字节数，对应 RFC 3986 限制 */
#define WEB_FILE_PATH_SEGMENT_MAX_BYTES    255U
/** @brief 上传超时后允许的最大重试次数 */
#define WEB_FILE_UPLOAD_TIMEOUT_RETRIES    1U

/**
 * @brief 文件传输准入守卫的结果
 */
enum web_file_guard_result_t
{
    WEB_FILE_GUARD_OK = 0,         /**< 准入成功 */
    WEB_FILE_GUARD_UNAUTHORIZED,   /**< 未通过认证 */
    WEB_FILE_GUARD_BUSY,           /**< 已有另一传输占用槽位 */
    WEB_FILE_GUARD_UNAVAILABLE,    /**< Service 未就绪或正在停止 */
};

/**
 * @brief 文件操作的业务结果码
 */
enum web_file_operation_result_t
{
    WEB_FILE_OPERATION_OK = 0,                /**< 操作成功 */
    WEB_FILE_OPERATION_BAD_REQUEST,           /**< 请求参数非法 */
    WEB_FILE_OPERATION_NOT_FOUND,             /**< 目标路径不存在 */
    WEB_FILE_OPERATION_WRONG_TYPE,            /**< 路径类型与操作不匹配 */
    WEB_FILE_OPERATION_LENGTH_REQUIRED,       /**< 缺少 Content-Length */
    WEB_FILE_OPERATION_TOO_LARGE,             /**< 上传超过容量上限 */
    WEB_FILE_OPERATION_OVERWRITE_REQUIRED,    /**< 目标已存在且未确认覆盖 */
    WEB_FILE_OPERATION_ALREADY_EXISTS,        /**< 不允许覆盖时目标已存在 */
    WEB_FILE_OPERATION_DIRECTORY_NOT_EMPTY,   /**< 删除目标目录非空 */
    WEB_FILE_OPERATION_ROOT_FORBIDDEN,        /**< 操作被保留的根目录拒绝 */
    WEB_FILE_OPERATION_INSUFFICIENT_STORAGE,  /**< 文件系统剩余空间不足 */
    WEB_FILE_OPERATION_NO_MEMORY,             /**< 内存分配失败 */
    WEB_FILE_OPERATION_IN_USE,                /**< 目标文件正被其他组件打开 */
    WEB_FILE_OPERATION_IO_ERROR,              /**< 底层文件系统 I/O 错误 */
    WEB_FILE_OPERATION_CANCELLED,             /**< 操作被停止流程取消 */
};

/**
 * @brief 文件上传请求的前置校验信息
 */
struct web_file_upload_request_t
{
    size_t expected_length;    /**< 请求声明的上传正文长度 */
    bool   overwrite_confirmed;/**< 客户端是否已确认覆盖既有文件 */
    bool   target_exists;      /**< 上传目标路径当前是否存在 */
};

/**
 * @brief 文件传输操作使用的临时路径和草稿缓冲区集合
 */
struct web_file_transfer_workspace_t
{
    char logical[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];       /**< 解码后的逻辑路径 */
    char filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE]; /**< 映射后的文件系统路径 */
    char scratch[WEB_FILE_RESPONSE_SCRATCH_SIZE];          /**< 通用草稿缓冲区 */
    char auxiliary[WEB_FILE_RESPONSE_SCRATCH_SIZE];        /**< 辅助草稿缓冲区 */
};

/**
 * @brief 单路径操作使用的逻辑与文件系统路径缓冲区对
 */
struct web_file_path_workspace_t
{
    char logical[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];       /**< 解码后的逻辑路径 */
    char filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE]; /**< 映射后的文件系统路径 */
};

/**
 * @brief 文件移动操作的源路径与目标路径工作区
 */
struct web_file_move_workspace_t
{
    web_file_path_workspace_t source;      /**< 源路径缓冲区对 */
    web_file_path_workspace_t destination; /**< 目标路径缓冲区对 */
};

/**
 * @brief 文件系统变更操作类型
 */
enum web_file_mutation_t
{
    WEB_FILE_MUTATION_CREATE_DIRECTORY = 0, /**< 创建目录 */
    WEB_FILE_MUTATION_MOVE_FILE,            /**< 移动常规文件 */
    WEB_FILE_MUTATION_DELETE_ITEM,          /**< 删除文件或空目录 */
};

/**
 * @brief 文件扩展名到 MIME 类型的映射条目
 */
struct web_file_mime_entry_t
{
    const char *extension;    /**< 不含点号的文件扩展名 */
    const char *content_type; /**< 对应的 MIME Content-Type 值 */
};

/** @brief 鉴权并独占唯一文件传输槽位 */
web_file_guard_result_t web_file_transfer_acquire(httpd_req_t *request);

/** @brief 释放当前文件传输槽位并刷新活动会话时间 */
void web_file_transfer_release(void);

/** @brief 把传输缓冲区发布到 Service 所有权 */
bool web_file_publish_transfer_buffer(uint8_t *buffer);

/** @brief 查询停止流程是否已经取消当前传输 */
bool web_file_transfer_is_cancelled(void);

/** @brief 读取单路径查询参数并映射到固定文件系统 */
web_file_operation_result_t web_file_read_and_map_path(httpd_req_t *request, char *logical, size_t logical_size,
                                                       char *filesystem, size_t filesystem_size);

/** @brief 读取移动操作的源路径和目标路径并完成映射 */
web_file_operation_result_t web_file_read_and_map_move_paths(httpd_req_t *request,
                                                             web_file_move_workspace_t *workspace);

/** @brief 把传输守卫结果映射为 HTTP 错误响应 */
esp_err_t web_file_send_guard_error(httpd_req_t *request, web_file_guard_result_t result);

/** @brief 把文件操作结果映射为 HTTP 错误响应 */
esp_err_t web_file_send_operation_error(httpd_req_t *request, web_file_operation_result_t result);
