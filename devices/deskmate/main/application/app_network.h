/*
 * 文件职责：声明网络 Application 的产品命令、租约和生命周期接口。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_backend_context.h"

/** @brief OTA 检查的产品来源，用于决定完成后的自动安装策略 */
typedef enum
{
    APP_NETWORK_OTA_CHECK_MANUAL = 0, /*!< 用户从设置菜单手动发起，禁止自动安装 */
    APP_NETWORK_OTA_CHECK_AUTOMATIC,  /*!< 周期策略自动发起，可应用持久化自动安装设置 */
} app_network_ota_check_mode_t;

/** @brief 网络 Application Task 当前支持的互斥网络产品租约类型 */
typedef enum
{
    APP_NETWORK_LEASE_NONE = 0,       /*!< 未持有租约 */
    APP_NETWORK_LEASE_REALTIME_VOICE, /*!< 实时语音网络租约 */
    APP_NETWORK_LEASE_WEB_FILE,       /*!< Web 文件管理网络租约 */
} app_network_lease_type_t;

/** @brief 互斥网络产品租约只读值快照 */
typedef struct
{
    app_network_lease_type_t type;       /*!< 当前租约类型 */
    bool                     active;     /*!< 是否正在持有 */
    uint32_t                 generation; /*!< 当前租约代次 */
} app_network_lease_snapshot_t;

/**
 * @brief Network Manager 最新变化已由网络 Application Task 收敛后的借用通知回调
 *
 * 回调运行于 `app_network_task`，不携带 Manager 内部指针。实现必须有界且快速，只能合并
 * 自身静态通知并唤醒对应状态所有者；不得阻塞、访问磁盘、调用网络控制 API 或等待其他 Task。
 * 回调函数和 context 必须在整个固件运行期内保持有效。
 *
 * @param[in] context 注册时借用的静态上下文
 */
typedef void (*app_network_link_change_callback_t)(void *context);

/**
 * @brief 初始化网络任务、命令队列和周期定时器
 *
 * 重复调用时保持幂等；成功返回后即可投递网络命令。
 *
 * @return ESP_OK 初始化成功；其他值表示资源创建失败
 */
esp_err_t app_network_init(void);

/**
 * @brief 注册唯一的 Network Manager 收敛后链路变化借用回调
 *
 * 网络 Application 在现有耐久 Manager pending 标志完成状态收敛后，于锁外调用该回调。
 * 通知可以合并；订阅者收到通知后必须重新复制自身需要的最新链路事实，不能依赖事件历史。
 * 本接口仅接受固件进程期静态回调；重复注册同一 callback/context 保持幂等，其他替换请求
 * 返回 `ESP_ERR_INVALID_STATE`，避免借用生命周期与在途回调发生竞态。
 *
 * @param[in] callback 固件进程期保持有效的快速通知回调，不得为 NULL
 * @param[in] context 固件进程期保持有效的借用上下文，可为 NULL
 * @return ESP_OK 已注册或原本就是同一订阅；ESP_ERR_INVALID_ARG callback 为空；
 *         ESP_ERR_INVALID_STATE 网络 Application 未初始化或已经注册其他订阅
 */
esp_err_t app_network_set_link_change_callback_borrow(app_network_link_change_callback_t callback, void *context);

/**
 * @brief 请求执行一次 Dashboard 同步
 *
 * 命令按值投递，不在调用者上下文执行网络访问。
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 任务未初始化、同步已排队/运行或存在产品冲突；
 *         ESP_ERR_TIMEOUT 命令队列已满
 */
esp_err_t app_network_request_sync(void);

/**
 * @brief 请求由网络任务切换到配网 Portal
 *
 * 网络 Application Task 在向 Network Manager 提交异步请求前原子占用 Portal 过渡状态；
 * 该状态到达明确 Manager 结果前会阻止新的互斥网络产品租约。
 *
 * @return ESP_OK 命令已入队；ESP_ERR_INVALID_STATE 任务未初始化或存在产品冲突；
 *         ESP_ERR_TIMEOUT 命令队列已满
 */
esp_err_t app_network_start_portal(void);

/**
 * @brief 取消已排队或正在运行的 Dashboard 同步
 *
 * @return ESP_OK 已记录取消请求
 */
esp_err_t app_network_cancel_sync(void);

/**
 * @brief 在轻睡眠维护窗口同步执行一次 Dashboard 拉取
 *
 * 调用前必须先通过 app_network_resume_from_light_sleep() 恢复网络策略。本函数等待唯一网络
 * Application Task 完成联网、Dashboard 更新及下一刷新截止时间解析后才返回；命令一旦被
 * Task 认领，timeout_ms 不会截断正在执行的 HTTP 事务。
 *
 * @param[in] timeout_ms 命令被网络 Task 认领前的最长等待时间，单位毫秒
 * @return ESP_OK Dashboard 与下一刷新时间均已更新；ESP_ERR_INVALID_ARG timeout_ms 为 0；
 *         ESP_ERR_INVALID_STATE 网络仍暂停或存在同步、OTA、租约冲突；ESP_ERR_TIMEOUT
 *         命令未及时认领；或联网、传输、协议和 Dashboard Store 错误码
 */
esp_err_t app_network_sync_for_light_sleep(uint32_t timeout_ms);

/**
 * @brief 复制当前有效的下一次 Dashboard 自动同步 UTC 截止
 *
 * 成功场景返回最近响应的 `next_refresh_at_utc`；完整同步失败后返回本地失败退避截止。
 *
 * @param[out] out_utc_timestamp UTC Unix 时间戳，单位秒
 * @return ESP_OK 已复制；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE
 *         尚无服务端截止且没有可换算为 UTC 的失败退避截止
 */
esp_err_t app_network_get_next_dashboard_sync_at_utc(int64_t *out_utc_timestamp);

/**
 * @brief 从当前持久化设置构造完整后端连接上下文
 *
 * 产品 ID 与固件目标来自构建生成头，设备 ID 由共享 protocols 组件基于 Wi-Fi Station
 * 基础 MAC 生成。返回值是纯值快照，调用方可在当前事务中复制或借用。
 *
 * @param[out] out_context 后端连接、鉴权与设备身份上下文
 * @return ESP_OK 已构造；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 服务地址为空；
 *         或设置读取、配置校验、硬件身份错误码
 */
esp_err_t app_network_get_backend_context_copy(protocol_backend_context_t *out_context);

/**
 * @brief 同步暂停网络策略与底层连接，为整机轻睡眠做准备
 *
 * 命令在唯一网络 Application Task 中串行处理：停止同步、OTA、会话退避和校时 Timer，
 * 再同步停止 Network Manager 会话及其 Wi-Fi/Portal 资源。任一互斥网络产品租约或 OTA
 * 活跃时拒绝暂停。
 *
 * @param[in] timeout_ms 最长等待回执时间，单位毫秒
 * @return ESP_OK 已暂停；ESP_ERR_INVALID_ARG timeout_ms 为 0；
 *         ESP_ERR_INVALID_STATE 任务未初始化或当前有冲突；ESP_ERR_TIMEOUT 未及时处理；
 *         或回执资源、底层停机错误码
 */
esp_err_t app_network_suspend_for_light_sleep(uint32_t timeout_ms);

/**
 * @brief 同步恢复轻睡眠前暂停的网络连接与周期策略
 *
 * 本函数只保证连接策略已经重新启动，不等待 STA 真正联网；重复恢复保持幂等。
 *
 * @param[in] timeout_ms 最长等待回执时间，单位毫秒
 * @return ESP_OK 已恢复；ESP_ERR_INVALID_ARG timeout_ms 为 0；
 *         ESP_ERR_INVALID_STATE 任务未初始化；ESP_ERR_TIMEOUT 未及时处理；
 *         或回执资源、命令投递错误码
 */
esp_err_t app_network_resume_from_light_sleep(uint32_t timeout_ms);

/**
 * @brief 启用或停用 Dashboard 自动同步
 *
 * 启用后，成功场景按服务端 `next_refresh_at_utc` 安排一次性同步；失败场景按本地退避重试。
 *
 * @param[in] enabled true 启用；false 停用并停止自动同步 Timer
 */
void app_network_set_dashboard_auto_sync_enabled(bool enabled);

/**
 * @brief 启用或停用 OTA 自动检查
 *
 * 周期由持久化设置中的 ota_check_interval_sec 决定；定时器只投递命令。
 *
 * @param[in] enabled true 启用；false 停用
 */
void app_network_set_ota_auto_check_enabled(bool enabled);

/**
 * @brief 按明确来源请求执行一次 OTA 检查
 *
 * 检查来源随异步事务传递到完成策略；手动检查始终禁止自动安装。
 *
 * @param[in] mode 手动或自动检查来源
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE OTA 状态不允许、已排队/运行或存在产品冲突；
 *         ESP_ERR_INVALID_ARG mode 无效；ESP_ERR_TIMEOUT 命令队列已满；或 OTA 状态读取错误码
 */
esp_err_t app_network_request_ota_check(app_network_ota_check_mode_t mode);

/**
 * @brief 请求安装最近一次检查发现的固件
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 没有待安装目标、OTA 已排队/运行或存在产品冲突；
 *         ESP_ERR_TIMEOUT 命令队列已满；或 OTA 状态读取错误码
 */
esp_err_t app_network_request_ota_install(void);

/**
 * @brief 幂等丢弃尚未开始安装的 OTA 目标
 *
 * 本操作同时清除 Firmware OTA 工具缓存与网络 Application 的待安装标记。检查、下载或
 * 安装命令已排队时拒绝执行，避免把正在使用的目标清掉。
 *
 * @return ESP_OK 两处待安装状态均已清除；ESP_ERR_INVALID_STATE 当前事务不可取消；
 *         或 Firmware OTA 返回的错误码
 */
esp_err_t app_network_discard_ota_update(void);

/**
 * @brief 查询 OTA 检查、下载或重启切换事务是否正在占用整机资源
 *
 * 已发现但尚未确认安装的目标不视为忙碌，不阻止语音或轻睡眠。
 *
 * @return true OTA 事务已排队或执行中；false 当前没有活动 OTA 事务
 */
bool app_network_is_ota_busy(void);

/**
 * @brief 请求实时语音网络租约并等待有限时间回执
 *
 * 命令只携带内部响应槽索引、租约类型和代次，不跨线程传递调用者指针。
 * Portal、验证、OTA 或其他互斥网络产品租约活跃时明确拒绝；成功后后台同步与自动 OTA 暂停。
 * 代次在一次固件运行期内不回绕复用；发出 UINT32_MAX 后继续申请会失败，直至设备重启。
 *
 * @param[in] timeout_ms 最长等待时间，单位毫秒
 * @param[out] out_generation 输出本次租约代次，释放时必须原样传回
 * @return ESP_OK 已授予；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 网络未在线、存在产品冲突或代次已耗尽；
 *         ESP_ERR_TIMEOUT 未及时处理；
 *         或网络状态、回执资源错误码
 */
esp_err_t app_network_acquire_realtime_voice_lease(uint32_t timeout_ms, uint32_t *out_generation);

/**
 * @brief 按代次释放实时语音网络租约
 *
 * 类型或代次不匹配时不能释放当前租约；重复释放已经结束的租约返回 ESP_OK。
 * timeout_ms 是命令被网络 Task 认领前的截止时间；一旦认领，函数等待对应完成回执，
 * 保证不会出现租约已释放但调用方收到超时的结果。
 *
 * @param[in] generation 申请时获得的租约代次
 * @param[in] timeout_ms 命令被网络 Task 认领前的截止时间，单位毫秒
 * @return ESP_OK 已释放或此前已释放；其他值表示参数、代次或超时错误
 */
esp_err_t app_network_release_realtime_voice_lease(uint32_t generation, uint32_t timeout_ms);

/**
 * @brief 请求 Web 文件管理网络租约并等待有限时间回执
 *
 * 命令只携带内部响应槽索引、租约类型和代次，不跨线程传递调用者指针。
 * Portal、验证、OTA 或其他互斥网络产品租约活跃时明确拒绝；成功后后台同步与自动 OTA 暂停。
 * 租约期间允许 Network Manager 继续执行已保存 STA 的断线重连和 IPv4 更新。
 * 代次在一次固件运行期内不回绕复用；发出 UINT32_MAX 后继续申请会失败，直至设备重启。
 *
 * @param[in] timeout_ms 最长等待时间，单位毫秒
 * @param[out] out_generation 输出本次租约代次，释放时必须原样传回
 * @return ESP_OK 已授予；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 网络未在线、存在产品冲突或代次已耗尽；
 *         ESP_ERR_TIMEOUT 未及时处理；
 *         或网络状态、回执资源错误码
 */
esp_err_t app_network_acquire_web_file_lease(uint32_t timeout_ms, uint32_t *out_generation);

/**
 * @brief 按代次释放 Web 文件管理网络租约
 *
 * 只有类型和代次均匹配时才释放；错误类型或旧代次不能释放当前租约。
 * 重复释放已经结束的租约返回 ESP_OK。
 * timeout_ms 是命令被网络 Task 认领前的截止时间；一旦认领，函数等待对应完成回执，
 * 保证不会出现租约已释放但调用方收到超时的结果。
 *
 * @param[in] generation 申请时获得的租约代次
 * @param[in] timeout_ms 命令被网络 Task 认领前的截止时间，单位毫秒
 * @return ESP_OK 已释放或此前已释放；其他值表示参数、类型、代次或超时错误
 */
esp_err_t app_network_release_web_file_lease(uint32_t generation, uint32_t timeout_ms);

/**
 * @brief 获取互斥网络产品租约状态的只读值快照
 *
 * 返回整结构副本，不暴露内部锁、回执槽或其他可变所有权。
 *
 * @param[out] out 输出快照；NULL 时不执行操作
 */
void app_network_get_lease_snapshot(app_network_lease_snapshot_t *out);
