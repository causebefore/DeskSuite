/*
 * 文件职责：提供与产品生命周期无关的 Wi-Fi 链路、连接事件和配网 Portal 能力。
 * 主要依赖：ESP-IDF Wi-Fi、Netif、Event、HTTP Server。
 * 调用方：network_manager。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CONNECT_WIFI_SSID_MAX     33
#define CONNECT_WIFI_PASSWORD_MAX 65
#define CONNECT_SERVICE_URL_MAX   128
#define CONNECT_DEVICE_TOKEN_MAX  96
#define CONNECT_PORTAL_URL_MAX    32
#define CONNECT_QR_PAYLOAD_MAX    160

    /**
 * @brief STA 连接参数
 */
    typedef struct
    {
        char ssid[CONNECT_WIFI_SSID_MAX];
        char password[CONNECT_WIFI_PASSWORD_MAX];
    } connect_sta_config_t;

    /**
 * @brief Portal 表单提交数据
 */
    typedef struct
    {
        char ssid[CONNECT_WIFI_SSID_MAX];
        char password[CONNECT_WIFI_PASSWORD_MAX];
        char service_url[CONNECT_SERVICE_URL_MAX];
        char device_token[CONNECT_DEVICE_TOKEN_MAX];
    } connect_portal_submission_t;

    /**
 * @brief Portal 启动后可展示的固定连接信息
 */
    typedef struct
    {
        bool active;
        char ssid[CONNECT_WIFI_SSID_MAX];
        char password[CONNECT_WIFI_PASSWORD_MAX];
        char portal_url[CONNECT_PORTAL_URL_MAX];
        char wifi_qr_payload[CONNECT_QR_PAYLOAD_MAX];
    } connect_portal_info_t;

    /**
 * @brief 当前 STA 物理链路查询结果
 *
 * `associated` 表示已关联 AP，`has_ipv4` 表示 STA netif 已获得非零 IPv4 地址。
 * 该结构只描述查询瞬间的底层事实，不表示产品业务状态。
 */
    typedef struct
    {
        bool    associated;                  /**< 已关联 Wi-Fi AP */
        bool    has_ipv4;                    /**< 已取得非零 IPv4 */
        int8_t  rssi_dbm;                    /**< 当前 RSSI，单位 dBm */
        char    ssid[CONNECT_WIFI_SSID_MAX]; /**< 当前 AP SSID */
        uint8_t bssid[6];                    /**< 当前 AP BSSID */
        uint8_t primary_channel;             /**< 主信道 */
        uint8_t secondary_channel;           /**< ESP-IDF wifi_second_chan_t 原始值 */
        uint8_t auth_mode;                   /**< ESP-IDF wifi_auth_mode_t 原始值 */
        uint8_t bandwidth;                   /**< ESP-IDF wifi_bandwidth_t 原始值 */
        bool    phy_11b;                     /**< AP 支持 802.11b */
        bool    phy_11g;                     /**< AP 支持 802.11g */
        bool    phy_11n;                     /**< AP 支持 802.11n */
        bool    phy_11ax;                    /**< AP 支持 802.11ax */
        char    ip[16];                      /**< STA IPv4 */
        char    netmask[16];                 /**< STA IPv4 子网掩码 */
        char    gateway[16];                 /**< STA IPv4 网关 */
        char    dns_primary[16];             /**< 主 DNS IPv4；不可用时为空 */
        char    dns_backup[16];              /**< 备用 DNS IPv4；不可用时为空 */
    } connect_link_info_t;

    /**
 * @brief connect 上报的底层链路事件类型
 */
    typedef enum
    {
        CONNECT_LINK_EVENT_GOT_IP = 0,
        CONNECT_LINK_EVENT_DISCONNECTED,
    } connect_link_event_type_t;

    /**
 * @brief 底层链路事件不可变数据
 *
 * GOT_IP 事件的 `link` 包含当前 SSID、IPv4 和 RSSI；DISCONNECTED 事件的
 * `link` 保留驱动提供的断开前 SSID/RSSI，`disconnect_reason` 为 ESP-IDF
 * 原始原因码。
 */
    typedef struct
    {
        connect_link_event_type_t type;
        connect_link_info_t       link;
        uint8_t                   disconnect_reason;
    } connect_link_event_t;

    /**
 * @brief 底层链路事件通知回调
 *
 * 回调可能运行于 ESP 事件循环任务，事件数据仅在回调期间有效。回调必须快速返回，
 * 需要异步处理时应复制事件并投递到调用方队列。
 *
 * @param[in] event 不可修改的链路事件数据
 * @param[in] ctx 调用方上下文
 */
    typedef void (*connect_link_event_cb_t)(const connect_link_event_t *event, void *ctx);

    /**
 * @brief 配网表单提交回调
 *
 * 回调运行于 HTTP Server 任务，必须仅复制或投递数据，不得执行耗时操作。
 *
 * @param[in] submission Portal 提交数据
 * @param[in] ctx 调用方上下文
 * @return ESP_OK 表示已接收；其他值表示拒绝或处理失败
 */
    typedef esp_err_t (*connect_credentials_cb_t)(const connect_portal_submission_t *submission, void *ctx);

    /**
 * @brief Portal 显式用户活动回调
 *
 * 回调运行于 HTTP Server
 * 任务，只表示页面发生了真实输入、点击等活动；自动扫描和状态轮询
 * 不会触发。回调必须快速返回，只能复制事实或向调用方队列投递命令。
 *
 * @param[in] ctx 调用方上下文
 * @return ESP_OK 表示活动事实已接收；其他值表示拒绝或投递失败
 */
    typedef esp_err_t (*connect_portal_activity_cb_t)(void *ctx);

    /**
 * @brief connect 回调集合
 */
    typedef struct
    {
        connect_link_event_cb_t      on_link_event;
        connect_credentials_cb_t     on_credentials_submitted;
        connect_portal_activity_cb_t on_portal_activity;
        void                        *ctx;
    } connect_callbacks_t;

    /**
 * @brief 初始化 Wi-Fi 链路能力
 *
 * @return ESP_OK 成功；其他值表示 ESP-IDF 初始化失败
 */
    esp_err_t connect_init(void);

    /**
 * @brief 设置 connect 异步事件回调
 *
 * 函数在返回前同步复制回调函数值和 context 指针，不保留 `callbacks`
 * 结构指针本身。回调和 context 可在函数返回后由 ESP 事件循环或 HTTP
 * Server
 * 任务调用。清除槽位不会等待已经复制到事件任务栈上的回调返回，因此当前内部
 * 调用方必须使用静态函数，并让 context
 * 在设备运行期保持有效；本接口不得传入临时对象。 后续设置替换、传入 NULL 或
 * connect_stop() 会阻止新的回调取得该槽位。
 *
 * @param[in] callbacks 回调集合；NULL 表示清除槽位，但不等待已经在途的回调
 */
    void connect_set_callbacks_borrow(const connect_callbacks_t *callbacks);

    /**
 * @brief 复制指定配置并请求启动一次 STA 连接
 *
 * 函数返回前会完成配置复制，不保留调用方传入的指针。
 *
 * @param[in] config STA 连接参数，只在调用期间借用
 * @return ESP_OK 已发起连接；其他值表示参数或驱动操作失败
 */
    esp_err_t connect_request_start_station_copy(const connect_sta_config_t *config);

    /**
 * @brief 保留配网 Portal 并复制配置发起一次 STA 候选连接
 *
 * 调用前 Portal 必须已启动。函数会停止后台 Wi-Fi 扫描，保持 HTTP、DNS 和
 * SoftAP，继续使用 APSTA
 * 模式验证候选配置。函数返回前完成配置复制，不保留调用方指针。
 *
 * @param[in] config STA 候选连接参数，只在调用期间借用
 * @return ESP_OK 已发起连接；其他值表示参数、Portal 状态或驱动操作失败
 */
    esp_err_t connect_request_start_station_with_portal_copy(const connect_sta_config_t *config);

    /**
 * @brief 断开 STA 候选连接并保留配网 Portal
 *
 * @return ESP_OK 已断开或原本未连接；其他值表示驱动操作失败
 */
    esp_err_t connect_disconnect_station_keep_portal(void);

    /**
 * @brief 候选连接验证成功后关闭 Portal 并保留 STA 链路
 *
 * 同步停止 Portal DNS、HTTP 和扫描资源，再把 Wi-Fi 从 APSTA 切换为
 * STA。只有返回 ESP_OK 时，调用方才能确认配网入口已经关闭且当前 STA 链路仍由
 * connect 持有。
 *
 * @return ESP_OK Portal 已关闭并切换为 STA；其他值表示清理或模式切换失败
 */
    esp_err_t connect_complete_portal_station(void);

    /**
 * @brief 启动配网 Portal 并返回本次 Portal 信息
 *
 * 函数返回 ESP_OK 时，会在返回前将 Portal 信息复制到 `out_info`。
 * 返回其他错误时，`out_info` 内容无效，调用方不得读取。
 *
 * @param[out] out_info Portal 连接信息
 * @return ESP_OK 已启动或已在运行；其他值表示启动失败
 */
    esp_err_t connect_start_portal_copy(connect_portal_info_t *out_info);

    /**
 * @brief 设置 Portal 状态页展示信息
 *
 * 该接口可由网络管理任务调用，供 Portal 状态页轮询读取。函数在返回前
 * 同步复制字符串，不保留 `message` 指针。
 *
 * @param[in] message 待展示的 UTF-8 状态信息；NULL 会写入空字符串
 */
    void connect_set_portal_status_borrow(const char *message);

    /** @brief QR 码渲染布局 */
    typedef struct
    {
        uint16_t side_pixels;        /**< 含 quiet zone 的正方形总边长（像素） */
        uint16_t module_pixels;      /**< 单个 QR 模块的正方形边长（像素） */
        uint16_t module_count;       /**< 不含 quiet zone 的单边模块数量 */
        uint8_t  quiet_zone_modules; /**< 四周 quiet zone 的模块数量 */
    } connect_qr_layout_t;

    /**
 * @brief QR 码渲染开始回调
 *
 * 回调应根据布局初始化目标，例如填充白色背景、建立 SVG 画布或计算显示偏移。
 * `layout` 仅在回调执行期间有效，不得保存其指针。
 *
 * @param[in] layout QR 码真实渲染布局
 * @param[in] ctx 用户上下文
 * @return ESP_OK 初始化成功；其他错误会中止渲染并原样返回调用方
 */
    typedef esp_err_t (*connect_qr_begin_cb_t)(const connect_qr_layout_t *layout, void *ctx);

    /**
 * @brief QR 码黑色矩形填充回调
 *
 * 每个黑色 QR 模块调用一次，坐标与尺寸均为像素单位并已包含 quiet zone 偏移。
 *
 * @param[in] x_pixels 矩形左上角横坐标
 * @param[in] y_pixels 矩形左上角纵坐标
 * @param[in] width_pixels 矩形宽度
 * @param[in] height_pixels 矩形高度
 * @param[in] ctx 用户上下文
 * @return ESP_OK 填充成功；其他错误会中止渲染并原样返回调用方
 */
    typedef esp_err_t (*connect_qr_fill_dark_rect_cb_t)(uint16_t x_pixels, uint16_t y_pixels, uint16_t width_pixels,
                                                        uint16_t height_pixels, void *ctx);

    /** @brief 显示格式无关的 QR 码渲染接收端 */
    typedef struct
    {
        connect_qr_begin_cb_t          begin;          /**< 初始化白底或输出容器 */
        connect_qr_fill_dark_rect_cb_t fill_dark_rect; /**< 填充单个黑色模块矩形 */
    } connect_qr_sink_t;

    /**
 * @brief 将配网 QR 码同步渲染到调用方 sink
 *
 * 函数只借用 `in_portal`、`in_sink` 和 `ctx`，返回后不保存任何指针。
 * 二维码按真实模块数量计算最大整数缩放，输出边长保证不超过
 * `max_side_pixels`。`begin` 先执行一次，随后每个黑色模块调用一次
 * `fill_dark_rect`；回调均在当前调用上下文同步执行。
 *
 * `begin` 负责将整个 `side_pixels × side_pixels` 目标初始化为白色，
 * 以形成 QR 码规范要求的 quiet zone。函数内部不分配位图缓冲区。
 *
 * @param[in] in_portal 配网信息，仅在调用期间借用
 * @param[in] max_side_pixels 允许输出的最大正方形边长（像素）
 * @param[in] in_sink QR 码渲染回调集合，仅在调用期间借用
 * @param[in] ctx 用户上下文，仅在调用期间借用
 * @return ESP_OK 渲染完成；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 配网信息未就绪；ESP_ERR_INVALID_SIZE
 *         可用边长不足以容纳每模块至少一个像素；其他值为编码或 sink 错误
 */
    esp_err_t connect_render_portal_qr_borrow(const connect_portal_info_t *in_portal, uint16_t max_side_pixels,
                                              const connect_qr_sink_t *in_sink, void *ctx);

    /**
 * @brief 停止 connect 持有的 Portal 和 Wi-Fi 资源
 *
 * @return ESP_OK 成功；其他值表示停止 Wi-Fi 失败
 */
    esp_err_t connect_stop(void);

    /**
 * @brief 同步反初始化 connect 及其独占的 Wi-Fi 资源
 *
 * 本函数会先停止 HTTPD、DNS 和扫描任务，再停止 Wi-Fi、注销事件处理器、
 * 销毁默认 STA/AP netif，最后调用 `esp_wifi_deinit()`。默认事件循环和
 * lwIP 基础任务属于平台共享资源，不由本函数销毁。只有返回 ESP_OK 时才能确认
 * connect 私有任务已退出并再次调用
 * connect_init()。清理失败时组件锁存不可重启状态，防止残留 Task、handler 或
 * Driver 被下一会话复用。
 *
 * 尚未初始化时调用属于幂等清理并返回 ESP_OK。不得从 connect 回调中调用。
 *
 * @return ESP_OK 已完成清理或原本未初始化；其他值表示至少一个清理步骤失败
 */
    esp_err_t connect_deinit(void);

    /**
 * @brief 同步复制当前 STA 物理链路和 IPv4 快照
 *
 * 未关联 AP 或 Wi-Fi 尚未启动属于正常查询结果，函数返回 ESP_OK，并通过
 * `associated`/`has_ipv4` 返回 false。该接口不读取缓存的业务状态。
 *
 * @param[out] out 当前链路信息
 * @return ESP_OK 查询完成；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE
 * 尚未初始化； 其他值表示底层查询失败
 */
    esp_err_t connect_get_link_snapshot_copy(connect_link_info_t *out);

#ifdef __cplusplus
}
#endif
