/*
 * 文件职责：提供 OTA 业务入口，负责 App 层 OTA 流程编排。
 * 主要依赖：App 网络编排入口。
 * 调用方：app_main、设置菜单用户意图入口。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief 初始化 OTA 业务模块
 *
 * 初始化自动检查开关；OTA 呈现由 `ota_presenter` 负责。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t app_ota_init(void);

/**
 * @brief 请求异步执行一次 OTA 检查
 *
 * 请求检查是否有可用更新。返回值只表示命令是否成功提交，最终结果通过 OTA 事实事件通知。
 *
 * @return ESP_OK 请求已发送；其他值表示请求失败
 */
esp_err_t app_ota_request_check(void);

/**
 * @brief 请求异步安装最近一次检查发现的固件
 *
 * 提交失败时保留待安装目标和 Presenter 中的目标信息，允许用户再次确认。
 *
 * @return ESP_OK 安装命令已提交；其他值表示当前状态或命令队列不接受请求
 */
esp_err_t app_ota_request_install(void);

/**
 * @brief 清除尚未开始安装的目标并重置 OTA 呈现状态
 *
 * 只有底层目标和网络 Application 待安装标记都清除成功后才返回 ESP_OK。
 *
 * @return ESP_OK 已清除；其他值表示事务正在执行或底层清理失败
 */
esp_err_t app_ota_clear_pending_update(void);

/**
 * @brief 查询 OTA 是否正处于禁止页面导航的事务阶段
 *
 * @return true 检查、下载或安装提交正在进行；false 可安全离开设置页
 */
bool app_ota_is_navigation_locked(void);
