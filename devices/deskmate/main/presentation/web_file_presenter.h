/**
 * @file web_file_presenter.h
 * @brief 网页文件管理设备端纯展示事实与 View Model 接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 网页文件管理 Presenter 的稳定展示状态 */
    typedef enum
    {
        WEB_FILE_PRESENTER_STATE_STOPPED = 0,       /**< 未启用网页文件管理 */
        WEB_FILE_PRESENTER_STATE_CHECKING_STORAGE,  /**< 正在检查 SD 文件系统 */
        WEB_FILE_PRESENTER_STATE_ACQUIRING_NETWORK, /**< 正在申请网络租约 */
        WEB_FILE_PRESENTER_STATE_STARTING_SERVICE,  /**< 正在启动网页文件 Service */
        WEB_FILE_PRESENTER_STATE_RUNNING,           /**< 网页文件管理正在运行 */
        WEB_FILE_PRESENTER_STATE_STOPPING,          /**< 正在安全关闭网页文件管理 */
        WEB_FILE_PRESENTER_STATE_ERROR,             /**< 启动或关闭失败 */
    } web_file_presenter_state_t;

    /** @brief Application 按值推送给 Presenter 的有界纯展示事实 */
    typedef struct
    {
        uint64_t                   presentation_revision; /**< Application 分配的严格单调展示版本 */
        web_file_presenter_state_t state;                 /**< 当前展示状态 */
        bool                       exit_allowed;          /**< 当前是否可以安全离开页面 */
        char                       url[32];               /**< 当前 STA IPv4 对应 URL */
        char                       access_code[7];        /**< 仅供本地显示的六位访问码 */
        uint64_t                   total_bytes;           /**< 文件系统总容量，单位字节 */
        uint64_t                   free_bytes;            /**< 文件系统可用容量，单位字节 */
        esp_err_t                  error;                 /**< 最近一次启动或停止错误 */
    } web_file_presenter_input_t;

    /** @brief 网页文件管理页面按值读取的有界 View Model */
    typedef struct
    {
        web_file_presenter_state_t state;          /**< 当前展示状态 */
        bool                       running;        /**< Service 是否正在运行 */
        bool                       exit_allowed;   /**< 当前是否可以安全离开页面 */
        char                       title[32];      /**< 当前状态中文标题 */
        char                       url[32];        /**< 当前 STA IPv4 对应 URL */
        char                       access_code[7]; /**< 仅供本地显示的六位访问码 */
        char                       total_size[24]; /**< 格式化后的文件系统总容量 */
        char                       free_size[24];  /**< 格式化后的文件系统可用容量 */
        esp_err_t                  error;          /**< 最近一次启动或停止错误 */
    } web_file_view_model_t;

    /**
     * @brief 初始化网页文件管理 Presenter
     *
     * 首次调用把值快照重置为停止态，并把已应用版本初始化为 0；输入版本必须从 1 开始。
     * 重复调用保持幂等，保留当前 View Model 和已应用版本，避免旧的在途更新因版本归零重新
     * 生效。本函数不创建 Task、Queue 或 Timer，也不发起产品命令。
     *
     * @return ESP_OK 初始化完成
     */
    esp_err_t web_file_presenter_init(void);

    /**
     * @brief 按值接收 Application 推送的纯展示事实并更新 View Model
     *
     * 本同步函数只复制有界文本并执行枚举、容量和标题转换，不保存输入指针，不读取
     * Application、Service、Network 或文件系统，也不发布呈现事件。访问码仅保存在当前
     * 本地 View Model；接口不接收浏览器 Bearer token。
     *
     * 只有 `presentation_revision` 严格晚于已应用版本时才替换 View Model；相同或更旧的输入
     * 返回 `ESP_OK` 但通过 `out_accepted=false` 明确拒绝。版本 0 非法，Application 在
     * `UINT64_MAX` 发出一次后必须停止推送，禁止回绕复用。唯一写入方还必须把本函数到对应
     * 刷新事件入队之间串行化，避免已接受版本在派发前被更高版本覆盖。
     *
     * @param[in] input 调用期间借用的完整展示事实
     * @param[out] out_accepted true 表示本次输入已替换 View Model；false 表示旧版本被拒绝
     * @return ESP_OK 已完成版本仲裁；ESP_ERR_INVALID_ARG 参数或状态无效；
     *         ESP_ERR_INVALID_STATE Presenter 尚未初始化
     */
    esp_err_t web_file_presenter_update_copy(const web_file_presenter_input_t *input, bool *out_accepted);

    /**
     * @brief 按值读取网页文件管理 View Model
     *
     * 本同步函数只在短临界区内复制完整结构，不返回内部指针；`out_view` 为空时不执行操作。
     *
     * @param[out] out_view 调用方提供的 View Model 输出
     */
    void web_file_presenter_get_view_copy(web_file_view_model_t *out_view);

#ifdef __cplusplus
}
#endif
