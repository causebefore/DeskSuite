# DeskMate 命名迁移审计

> 状态：审阅清单，更新于 2026-07-28。
>
> 本文只记录 DeskMate 当前代码相对
> [嵌入式 C/C++ 术语与命名规范](../../../../docs/standards/c_cpp_naming_conventions.md)和
> [受控术语表](../../../../docs/standards/c_cpp_terminology.md)的迁移候选，不定义通用规则。

## 1. 审计范围

- `main/` 与 `components/` 中项目自有 C/C++ 头文件和实现。
- 不修改 ESP-IDF、FreeRTOS、LVGL、第三方代码和生成文件的既有名称。
- `shared/` 公共 API 需要跨设备评审，不纳入 DeskMate 单项目机械改名。

## 2. 可机械收敛的候选

| 当前名称 | 拟统一名称 | 原因 |
| --- | --- | --- |
| `UI_RUNTIME_STATE_UNINIT` | `UI_RUNTIME_STATE_UNINITIALIZED` | 公共生命周期状态不使用局部缩写 |
| `calendar_get_snapshot()` | `calendar_get_snapshot_copy()` | 返回所有者缓存的完整快照副本 |
| `mail_get_snapshot()` | `mail_get_snapshot_copy()` | 返回所有者缓存的完整快照副本 |
| `quota_get_snapshot()` | `quota_get_snapshot_copy()` | 返回所有者缓存的完整快照副本 |
| `weather_get_snapshot()` | `weather_get_snapshot_copy()` | 返回所有者缓存的完整快照副本 |
| `dashboard_store_get_snapshot()` | `dashboard_store_get_snapshot_copy()` | 返回所有者缓存的完整快照副本 |
| `app_network_get_lease_snapshot()` | `app_network_get_lease_snapshot_copy()` | 返回所有者缓存的完整快照副本 |
| `device_button_pressed_state_t` | `device_button_pressed_snapshot_t` | 复合物理电平数据，不是离散阶段 |
| `device_button_get_pressed_state_copy()` | `device_button_read_pressed_snapshot()` | 直接读取 GPIO，不是读取所有者缓存 |
| `app_pomodoro_context_t` | `app_pomodoro_runtime_t` | 实际拥有 Task、锁、Timer 和工作数据 |
| `app_pomodoro_state_t` | `app_pomodoro_runtime_data_t` | 私有可变数据，不是离散状态枚举 |
| `app_pomodoro_status_t` | `app_pomodoro_snapshot_t` | 完整番茄钟领域数据；现有 Doxygen 已称为快照 |
| `app_pomodoro_get_status_copy()` | `app_pomodoro_get_snapshot_copy()` | Getter 与输出类型语义一致 |
| `presentation_data_status_t` | `presentation_data_state_t` | `EMPTY/OK/STALE/ERROR` 是单一呈现阶段 |
| `ui_platform_font_status_t` | `ui_platform_font_state_t` | `READY/FALLBACK/UNAVAILABLE` 是单一阶段 |
| `rlcd_font_container_status_t` | `rlcd_font_container_result_t` | `OK/INVALID_*` 描述单次解析结果 |
| `environment_service_environment_status_t` | `environment_service_environment_snapshot_t` | 温湿度领域值及质量信息 |
| `environment_service_battery_status_t` | `environment_service_battery_snapshot_t` | 电池领域值及质量信息 |
| `environment_service_status_t` | `environment_service_snapshot_t` | 两类采样数据的完整时点副本 |
| `environment_service_get_status_copy()` | `environment_service_get_snapshot_copy()` | Getter 与输出类型语义一致 |
| `pcf85063_interrupt_status_t` | `pcf85063_interrupt_snapshot_t` | 一次寄存器读取产生的硬件标志 |
| `pcf85063_driver_get_interrupt_status_copy()` | `pcf85063_driver_read_interrupt_snapshot()` | 直接执行 I2C 读取 |

`dashboard_store_get_weather/calendar/mail/quota()` 如果实现均为整结构复制，也应分别增加
`_copy`。

## 3. 需要按完整调用链收敛

| 当前词组 | 拟统一词组 | 说明 |
| --- | --- | --- |
| Dashboard `sync` | Dashboard `refresh` | 当前是服务端到设备的单向拉取 |
| `*_flush_async()` | `*_request_flush()` | 返回只表示异步提交成功 |
| `_listener_t` | `_callback_t` 或 `_cb_t` | `listener` 不作为回调同义词；最终拼写取决于组件缩写配置 |
| `emit_user_intent` | `dispatch_user_intent` | UI 将已构造意图路由给 Application |
| 公共 `handle_*` / `*_handle` | 具体动作词 | 按实际行为改为 `apply`、`dispatch` 或领域动词 |
| 产品层 `screen` | `page` | `screen` 只保留给 LVGL 根对象 |

## 4. 需要先澄清语义

- DeskMate 尚未统一选择回调、上下文、配置和消息的完整或紧凑拼写。开始批量改名前，应先确定
  全项目配置或按组件记录例外；现有 `button_service_event_cb_t` 本身不再视为错误。
- `UI_USER_INTENT_SCREEN_LOADED` 是 UI 已完成加载的事实，不是用户意图。应先拆分用户动作和
  UI 生命周期事件类型。
- `app_page_notify_screen_loaded()`、`app_page_publish_initial_ui()`、
  `app_page_dispatch_current()` 和 `app_key_dispatch_event()` 混合事实接收、状态迁移与
  Presentation 路由，不能全局替换动词。
- `consume_input()` 返回 `bool` 时需要同时说明“输入已消费”和“异步命令是否成功接受”；
  两者不相同时应改为显式结果。
- `device_rtc_get_snapshot_copy()` 等函数需要核对实现是否直接执行 I/O；直接 I/O 应使用
  `read_*_snapshot()`。

## 5. 迁移约束

1. 按“公共声明 → 实现 → 调用方 → 测试 → README/架构描述”修改完整调用链。
2. 每个提交只收敛一个可独立回滚的术语组。
3. 不在命名提交中改变业务行为、线程模型、错误码、队列策略或持久化语义。
4. DeskMate 自有且无外部消费者的旧名不保留兼容包装、宏别名或 deprecated 转发函数。
5. 修改后使用静态搜索检查旧术语残留，并解释确需保留的外部名称。
6. 需要使用受控术语表中不存在的词时，先新增术语并按术语表流程向用户说明理由。
