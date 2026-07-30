# DeskMate Services / Application 命名语义审查确认与修复结果

> 审查日期：2026-07-28
> 修复日期：2026-07-28
> 范围：`main/application/`、`components/services/`、`components/device/`、
> `components/bsp/`、`components/drivers/` 及直接调用方、测试和组件文档

## 1. 确认结论

原摘要不能整体确认，原因有三类：

1. 编号实际只有 H1～H4、M1～M5、L1～L3，共 12 项，不是文中声称的 15 项；H5～H8
   没有正文。
2. H2、H3、H4、M3 混合了真实问题和不成立的建议，不能按表格机械改名。
3. M2、M4、L1、L2 属于误判或没有证据支持的风格建议，不应制造代码改动。

按实现、调用链和规范重新核对后，真实问题已经修复；误判项保留原有正确语义，并在受控术语表
和命名审计中写明边界。

## 2. 已修复问题

### 2.1 异步提交语义

以下公共 API 返回成功时只表示请求已接受，现已统一为 `request_<operation>()`：

- `app_network_request_start_portal()`
- `app_ota_request_check()`
- `app_ota_request_install()`
- `voice_service_request_chat()`
- `bsp_display_request_flush()`
- `device_display_request_flush()`

显示刷新是原摘要漏报的同类问题。声明、实现、调用方和组件文档已同步迁移，不保留旧名兼容层。

### 2.2 回调注册语义

- `app_network_register_link_change_callback_borrow()` 是进程期固定、禁止原位替换的订阅，使用
  `register`。
- `system_clock_callback_t`、`system_clock_register_callback_borrow()` 和
  `system_clock_unregister_callback()` 统一使用 callback，不再把 listener 作为同义词。
- 管理唯一可替换回调槽、并明确允许 `NULL` 清除的 `set_*_callback_borrow()` 保持不变。

受控术语表已明确 `set` 与 `register` 的边界，避免把所有回调 API 机械改成同一动词。

### 2.3 数据形态

- 环境联合数据及其子结构统一为 `_snapshot_t`，读取入口为
  `environment_service_get_snapshot_copy()`。
- 番茄钟公共领域数据改为 `app_pomodoro_snapshot_t`，私有所有权结构改为
  `app_pomodoro_runtime_t` / `app_pomodoro_runtime_data_t`。
- 按键当前 GPIO 事实改为 `device_button_pressed_snapshot_t`，直接 I/O 入口为
  `device_button_read_pressed_snapshot()`。
- Light-sleep 唤醒来源是一次阻塞事务的返回结果，改为
  `bsp_power_wakeup_result_t` / `device_power_wakeup_result_t`；Button Service 接收的跨边界
  值副本则使用 `button_service_wakeup_snapshot_t`。
- `_status_t` 继续表示有界运行摘要；相关 Doxygen 不再将其称为领域快照。

### 2.4 `get` / `read` / `_copy`

- `app_network_get_lease_snapshot_copy()` 明确返回所有者缓存副本。
- PCF85063 中断寄存器数据改为 `pcf85063_interrupt_snapshot_t`，直接 I2C 读取入口使用
  `pcf85063_driver_read_interrupt_snapshot()`。
- RTC Driver → BSP → Device 的硬件读取链统一使用 `read_*()`；
  `device_rtc_read_snapshot()` 明确会执行 I2C，不再伪装成内存 Getter。
- `bsp_button_read_level()` 明确会读取 GPIO。

### 2.5 业务动作词

- 受控术语表新增 `consume`、`navigate` 和语音领域对象 `chat`。
- `wake_arbiter_consume_detection()` 明确表示认领并解释一次唤醒检测事实，不再使用空泛
  `handle`。
- 页面动作统一为 `app_page_navigate*()`；Screen 加载完成使用
  `app_page_reconcile_screen_loaded()`；初始呈现使用
  `app_page_dispatch_initial_presentation()`。
- OTA 公共动作使用 `clear`，不引入未登记的 `discard`。
- 删除无调用方且可绕过页面迁移契约的 `app_page_set_current()` 和
  `app_page_dispatch_current()`。
- 番茄钟和语音输入已经分别记录异步请求失败；`consume_input()` 的布尔返回值只表达输入
  是否被当前所有者认领。

### 2.6 `stop()` 完成语义

- `app_pomodoro_stop()` 原实现已经支持超时后再次调用，公共契约已补充该事实。
- `rtc_service_stop()` 原实现会在 `STOPPING` 时拒绝第二次调用，现已支持继续等待同一 Task；
  Task 已停止时重复调用幂等返回 `ESP_OK`。只有成功返回才表示状态已经收敛到
  `INITIALIZED`。

## 3. 已确认不修改的误判

| 原编号 | 结论 | 原因 |
| --- | --- | --- |
| H2 部分建议 | 不统一改成 `register` | 唯一可替换回调槽应使用 `set`，`NULL` 清除语义已明确 |
| H4 的类型改名建议 | 不把 `_status_t` 改成 `_snapshot_t` | 这些结构是生命周期、活动标志和错误组成的运行摘要 |
| M2 | 不修改内部命令 enum | Task 已接收后的 `START/STOP/SUSPEND` 是执行动作，不是提交阶段 |
| M4 | 不添加 `request_` | Power-save suspend/resume 等待 Task 回执，是同步完成 API |
| L1 | 不展开所有 `ctx` / `cb` | 两者已在受控短缩写表登记 |
| L2 | 无代码问题 | 所列文件头本来就是中文，报告证据不成立 |
| L3 | 不机械消除 `environment_service_environment_*` | 前一个词是组件前缀，后一个词是快照子域，未形成数据形态冲突 |

## 4. 同类扫描后的后续候选

以下内容不属于本次 Services / Application 范围，已记录到
`docs/architecture/deskmate_naming_audit.md`，不混入本轮提交：

后续 Dashboard 单次解析收敛已经删除四个中转 Data Getter 与完整 Store snapshot，剩余候选为：

- UI / Presentation 层若干 `_status_t` enum 的形态核对；
- `UI_USER_INTENT_SCREEN_LOADED` 从用户意图协议中拆出 UI 生命周期事实；
- Dashboard `sync` 是否应改为 `refresh` 的产品语义确认。

## 5. 验证

- 唤醒仲裁纯逻辑 host 测试：`ALL PASS`。
- `tools/tests/check_button_event_driven.ps1`：通过。
- `tools/tests/check_power_rebuild_stage1.ps1`：通过。
- `tools/tests/check_rtc_sleep_decoupling.ps1`：通过。
- `tools/tests/check_voice_power_lifecycle.ps1`：通过。
- 旧公共符号静态搜索：当前范围无残留。
- `git diff --check`：通过。

根据仓库约束，本任务未执行 DeskMate 固件编译。
