# DeskMate 命名迁移审计

> 状态：已执行，更新于 2026-07-28。
>
> 本文记录 DeskMate 当前代码相对
> [嵌入式 C/C++ 术语与命名规范](../../../../docs/standards/c_cpp_naming_conventions.md)和
> [受控术语表](../../../../docs/standards/c_cpp_terminology.md)的迁移结果与剩余边界，不定义
> 通用规则。

## 1. 审计范围

- 本轮执行范围：`main/application/`、`components/services/`、`components/device/`、
  `components/bsp/`、`components/drivers/` 及其直接调用方、测试和组件文档。
- 不修改 ESP-IDF、FreeRTOS、LVGL、第三方代码和生成文件的既有名称。
- `shared/` 公共 API 需要跨设备评审，不纳入 DeskMate 单项目改名。
- Data 与 UI / Presentation 候选保留在“后续独立迁移”，不混入本轮 Services / Application
  公共 API 变更。

## 2. 本轮已收敛

| 语义组 | 已采用名称或规则 | 说明 |
| --- | --- | --- |
| 异步提交 | `request_<operation>()` | Portal、OTA、语音对话和显示刷新返回成功时只表示请求已接受 |
| 长期回调 | `_callback_t`、`register_*_callback_borrow()` | `listener` 不再作为 callback 的同义词；唯一可替换槽仍使用 `set_*_callback_borrow()` |
| 缓存快照 | `get_*_snapshot_copy()` | 网络租约、环境联合数据和番茄钟领域数据均返回完整内存副本 |
| 硬件快照 | `read_*_snapshot()` | 按键、RTC 和 PCF85063 路径会直接执行 GPIO 或 I2C 读取 |
| 私有运行数据 | `_runtime_t` / `_runtime_data_t` | 番茄钟 Task、锁、Timer 与可变数据不再命名为复合 `_state_t` |
| 运行摘要 | `_status_t` + `get_status_copy()` | Doxygen 统一称“有界运行摘要”，不再与领域快照混称 |
| 操作结果 | `_result_t` | Light-sleep 唤醒来源属于一次阻塞事务的返回结果，不命名为快照 |
| 输入认领 | `consume_*()` | 只表达当前所有者是否认领输入；异步请求失败必须单独记录 |
| 页面迁移 | `navigate_*()` | 只用于 Application 顶层页面迁移；Presentation 消息路由继续使用 `dispatch` |
| 清除目标 | `clear_*()` | 清除尚未安装的 OTA 目标，不引入未登记的 `discard` 公共动作词 |

本轮同时删除了没有调用方的 `app_page_set_current()` 与
`app_page_dispatch_current()`，避免保留绕过页面迁移契约的公共入口。

## 3. 已确认不应机械修改

- `button_service_set_event_callback_borrow()`、`device_button_set_activity_callback_borrow()` 等
  管理唯一可替换回调槽，并明确允许 `NULL` 清除，继续使用 `set`；`register` 只用于加入集合、
  禁止原位替换或具有显式注销对偶的订阅。
- `app_network_suspend_for_power_save()` 与 `app_network_resume_from_power_save()` 等待 Task
  回执后才返回，是同步完成 API，不添加 `request_`。
- `NETWORK_COMMAND_START_*`、`STOP`、`SUSPEND` 是所有者 Task 已接收后的内部执行动作，不把
  公共提交阶段的 `request` 前缀机械复制到内部命令。
- `app_power_state_t` 等离散阶段 enum 和 `app_power_status_t` 等复合运行摘要原命名正确；
  只修正文档中的形态称谓。
- 局部 `ctx`、`cb` 已在受控短缩写表登记，不进行无收益的全量展开。

## 4. 后续独立迁移

以下候选不属于本轮 Services / Application 审查范围，应各自按完整调用链单独处理：

| 当前名称或边界 | 候选方向 | 原因 |
| --- | --- | --- |
| `calendar_get_snapshot()` 等 Data Getter | 增加 `_copy` | 返回所有者缓存的完整快照副本 |
| `dashboard_store_get_snapshot()` | `dashboard_store_get_snapshot_copy()` | 与其他缓存快照 Getter 对齐 |
| `presentation_data_status_t` | 核对为 `_state_t` | `EMPTY/OK/STALE/ERROR` 可能是单一呈现阶段 |
| `ui_platform_font_status_t` | 核对为 `_state_t` | `READY/FALLBACK/UNAVAILABLE` 可能是单一阶段 |
| `rlcd_font_container_status_t` | 核对为 `_result_t` | 值描述一次容器解析结果 |
| `UI_USER_INTENT_SCREEN_LOADED` | 拆分 UI 生命周期事实 | Screen 加载完成不是用户意图，需跨 UI/Application 协议迁移 |
| Dashboard `sync` | 产品确认后决定是否改为 `refresh` | 当前实现接近单向拉取，但名称涉及既有产品协议与文档 |

## 5. 迁移约束

1. 按“公共声明 → 实现 → 调用方 → 测试 → README/架构描述”修改完整调用链。
2. DeskMate 自有且无外部消费者的旧名不保留兼容包装、宏别名或 deprecated 转发函数。
3. 修改后静态搜索旧术语，并解释确需保留的外部名称。
4. 受控术语表不存在的公共动作词，必须先登记定义、边界和反例。
