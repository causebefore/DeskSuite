# 按键事件触发扫描设计

## 目标

把 `button_service` 从清醒期间永久每 10 ms 扫描，改为“GPIO 边沿触发、交互期间临时
每 10 ms 推进、状态稳定后停止”的按需调度。

本次必须保持左右键短按、长按、去抖和双按键独立事件语义，并保持 GPIO18、GPIO0 通过
EXT1 `ANY_LOW` 唤醒 Light Sleep 的现有能力。改动只减少空闲 CPU 唤醒，不宣称减少 Task
栈；`button_service` 仍不创建独立 Task。

环境采样继续使用现有 2 秒电池、30 秒温湿度周期。OTA 生命周期和其他 Task 本次不修改。

## 当前问题

当前 `button_service` 在 `start()` 中启动永久周期 `esp_timer`。即使两个按键均已释放且没有
任何待确认状态，ESP Timer Task 仍每 10 ms 被唤醒一次并读取两个 GPIO。

`device_button` 已经拥有候选电平、稳定电平、去抖截止时间、按下时间和长按已发送状态，但其
扫描结果只返回一个事件，不能告诉调度方是否仍需继续推进。Service 因此无法在状态稳定后安全
停表。

## 方案选择

### 采用：边沿触发有限扫描窗口

- 清醒空闲时只保留 GPIO 双边沿监听，不运行扫描 Timer。
- 任一边沿出现后启动一次 10 ms one-shot Timer。
- Timer 回调调用 `device_button_scan()`，并依据返回的“仍需推进”事实决定是否再启动下一次
  one-shot Timer。
- 去抖尚未完成，或稳定按下但尚未达到长按阈值时继续推进。
- 两键都没有候选变化，且不存在尚未发送的长按时停止推进，等待下一次 GPIO 边沿。

该方案复用现有 10 ms 状态机时间粒度，只改变空闲调度方式，行为风险最低。

### 不采用：为每个截止时间创建独立 Timer

分别为左右键去抖、长按和未来双击创建 Timer，可以进一步减少一次按键交互期间的扫描次数，
但会引入多 Timer 竞态、取消顺序和组合键同步问题。本次收益主要来自消除长期空闲轮询，没有
必要同时重写完整按键状态机。

### 不采用：创建专用 Button Task

GPIO ISR 通知专用 Task 可以得到清晰的阻塞入口，但会新增 Task 控制块和栈，与当前
`button_service` 无独立 Task 的资源边界冲突。现有 ESP Timer Task 足以执行有界 GPIO
采样和快速事件转发。

## 架构与数据流

清醒期间的数据流调整为：

```text
GPIO18 / GPIO0 任一边沿
  → BSP GPIO ISR 复制“发生按键活动”事实
  → Device ISR 快速回调
  → button_service 锁存边沿并启动 10 ms one-shot Timer
  → ESP Timer Task 调用 device_button_scan()
  → Device 去抖与长短按状态机
  → 零到两个不可变按键事件
  → app_key 快速投递默认事件循环
  → Application 解释产品动作
```

职责保持不变：

- BSP 只拥有引脚、GPIO ISR 注册和原始电平。
- Device 只拥有去抖与长短按状态机，不创建 Timer 或 Task。
- `button_service` 拥有扫描调度、ISR 到普通上下文的衔接和事件转发。
- Application 继续决定页面、语音、设置和电源活动语义。

## Device 扫描契约

`device_button_scan()` 调整为返回有界结果结构：

- 本轮左右键产生的事件数组，容量固定为 2；
- 有效事件数量；
- `follow_up_required`，表示至少一个按键仍处于去抖中，或已稳定按下但尚未发送长按。

`follow_up_required` 的判定规则为：

```text
candidate_pressed != stable_pressed
  → true

stable_pressed && !long_sent
  → true

稳定释放
  → false

稳定按下且长按事件已发送
  → false，释放边沿会重新触发扫描
```

返回两个事件而不是只保留第一个，可以避免左右键在同一扫描周期同时到达阈值时静默丢失第二个
事件。

## Service 调度与并发

`button_service` 继续使用一个 `ESP_TIMER_TASK` 分发的 Timer，但 Timer 改为 one-shot。

Service 私有状态至少包含：

- 生命周期状态；
- Timer 是否已经安排；
- GPIO 边沿锁存位；
- Light Sleep 唤醒按键待收敛位；
- 最近一次调度错误。

GPIO ISR 只执行以下有界操作：

1. 在 ISR 临界区确认 Service 正在运行；
2. 锁存“出现新边沿”；
3. 若当前没有已安排扫描，调用当前固定 ESP-IDF v6.0.1 提供的 IRAM/ISR-safe
   `esp_timer_start_once()` 路径安排一次 10 ms 回调；
4. 不读写 I²C，不记录普通日志，不调用 Application，不执行外部业务回调。

Timer 回调按以下顺序处理：

1. 消费当前边沿锁存；
2. 调用 `device_button_scan()`；
3. 在内部锁之外逐个转发本轮事件；
4. 清除已经由短按或长按事件收敛的唤醒按键位；
5. 若 Device 要求继续推进、回调期间又出现边沿，或仍有唤醒按键待收敛，则再启动一次
   10 ms one-shot Timer；
6. 否则标记扫描空闲，不再安排 Timer。

ISR 与 Timer 回调之间使用临界区保护锁存位和 Timer 安排状态。边沿发生在 Timer 回调采样前，
会被本轮 GPIO 读取覆盖；发生在采样后，则通过锁存位保证至少再运行一轮，不允许在“判定空闲”
与“停止安排”之间丢失边沿。

上层 `button_service_event_cb_t` 仍在 ESP Timer Task 上下文执行，继续要求快速、非阻塞返回。
`app_key` 仍先调用 `app_power_notify_activity()`，再以零等待方式投递默认事件循环。

## Light Sleep 衔接

EXT1 唤醒完全独立于清醒期 GPIO ISR：

```text
两个按键均已释放
  → BSP 配置 GPIO18 / GPIO0 EXT1 ANY_LOW
  → Light Sleep
  → 按键拉低后 EXT1 唤醒
  → BSP 复制左右键唤醒掩码并清理 EXT1
  → app_power 把唤醒按键事实提交给 button_service
  → button_service 强制启动临时扫描
```

不能假定睡眠期间发生的 GPIO 下降沿会在唤醒后重新触发清醒期 ISR，因此增加一个显式
`button_service` 唤醒事实提交 API。该 API 按值复制左右键位并安排扫描：

- 唤醒键仍按下时，Device 状态机继续完成去抖、短按或长按。
- 唤醒键在普通上下文恢复前已经释放时，EXT1 掩码证明它曾经按下；Service 在扫描收敛后重放
  对应短按事实。
- ISR 与唤醒事实同时到达时只合并扫描请求，不重复产生同一按键事件。

当前 `docs/低功耗流程.md` 仍处于“不停止运行期组件”的阶段 1。本次不在睡眠前调用
`button_service_stop()`，也不把按钮 Service 加入电源生命周期参与者；Service 保持 RUNNING，
只在唤醒返回后接收事实。睡眠前 stop、唤醒后 start 必须等阶段 1 已完成 100 次实机验收后，
作为规范指定的下一独立阶段实施和提交。

## 公共 API 调整

Device 增加：

- `device_button_scan_result_t`：包含最多两个事件、事件数量和 `follow_up_required`；
- `device_button_activity_callback_t`：运行在 GPIO ISR 上下文的无阻塞活动回调；
- `device_button_set_activity_callback_borrow()`：注册或清除长期借用的活动回调。

`device_button_scan()` 改为通过 `device_button_scan_result_t` 返回本轮完整结果。ISR 回调不携带
GPIO、板型或产品动作，只表示“至少一个按键电平发生变化”。

Button Service 增加：

```c
typedef struct
{
    bool left_button;
    bool right_button;
} button_service_wakeup_info_t;

esp_err_t button_service_request_light_sleep_wakeup_copy(
    const button_service_wakeup_info_t *wakeup);
```

该异步 API 在返回前复制唤醒事实并安排扫描，`ESP_OK` 只表示请求已接受；最终短按或长按仍通过
既有 `button_service_event_cb_t` 返回。它只接受至少包含一个按键位的输入，未初始化、未运行或
Timer 无法安排时返回对应错误。

为保证停止后没有在途 Timer 回调，现有停止接口调整为：

```c
esp_err_t button_service_stop(uint32_t timeout_ms);
```

`timeout_ms` 必须大于 0；返回 `ESP_OK` 时边沿回调已经注销、Timer 已停止且没有扫描或上层事件
回调仍在执行。超时时保留停止中状态和资源，拒绝 `start()` 与 `deinit()`。

BSP 增加只供 Device 使用的双边沿快速回调注册入口，回调类型和 API 使用 `bsp_button_` 前缀，
不向 Device、Service 或 Application 暴露 GPIO 编号。

## 生命周期与错误处理

- `init()` 创建未运行的 one-shot Timer，并完成 Device/BSP 边沿回调接线所需的本地资源；
  部分失败时逆序清理。
- `start()` 注册边沿回调、进入 RUNNING，并主动安排一次初始扫描，以识别启动时已经按下的键。
- `stop()` 先关闭业务入口并注销边沿回调，再同步停止 Timer；返回成功后保证不会再执行扫描
  或上层事件回调。
- `deinit()` 只允许在已停止且无在途回调时删除 Timer。
- GPIO ISR 调度失败时只锁存错误，不在 ISR 中记录普通日志；后续普通上下文首先报告该错误。
- 普通 `start()` 或 Light Sleep 唤醒事实提交无法安排 Timer 时返回原始错误码。
- Light Sleep 已返回但唤醒事实提交失败时，`app_power` 把它记录为恢复错误并阻止后续自动睡眠，
  避免设备在按键链路未恢复时继续循环休眠。
- 单次 `device_button_scan()` 失败时保留现有“首次失败、恢复后一次提示”语义，并继续有限重试；
  Service 不能因一次 GPIO 读取失败永久停在无监听状态。

公共 API、ISR 回调执行上下文、同步停止语义和借用期限必须在
`device_button.h`、`button_service.h` 中补充中文 Doxygen。`button_service` README、
Application README 与低功耗流程文档同步更新实际边界。

## 验证

先增加并运行 `tools/tests/check_button_event_driven.ps1`，让它在当前永久轮询实现上失败。修改后
同一检查至少确认：

- `button_service` 不再调用 `esp_timer_start_periodic()`；
- Timer 使用 one-shot 调度，空闲路径不重新安排；
- BSP 左右键配置双边沿 ISR，并有成对注册、注销；
- Device 扫描结果包含有界双事件和 `follow_up_required`；
- Light Sleep 返回路径把 EXT1 左右键事实提交给 `button_service`；
- 环境采样周期和 OTA 生命周期没有被本次改动改变；
- `git diff --check` 无格式错误。

实机清醒期验收：

- 左右键短按各 50 次，均只产生一次对应事件；
- 左右键长按各 50 次，均只产生一次长按且释放时不追加短按；
- 按住超过长按阈值后，扫描 Timer 停止增长，释放边沿能够重新进入并收敛；
- 左右键同时按下、同时释放时，两侧事件均不丢失；
- 连续快速按键和机械抖动不产生重复产品事件；
- 无按键 60 秒期间，`esp_timer_dump()` 中按钮 Timer 的触发次数保持不变。

Light Sleep 验收继续遵守现有 100 次门槛：

- 左键单独唤醒 50 次；
- 右键单独唤醒 50 次；
- 每轮 EXT1 来源正确；
- 唤醒按键事实不丢失、不重复；
- 不出现未知唤醒来源或新的 `BLOCKED`；
- 本次不停止按钮 Service，阶段 1 的其他参与者边界保持不变。

仓库规则禁止 Agent 未经明确要求主动编译；代码实现阶段默认只运行静态检查和格式核查，固件
构建与实机验收由用户明确要求后通过 `.\dm.ps1 build` 执行。
