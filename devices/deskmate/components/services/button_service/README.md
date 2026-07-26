# `button_service`

> `button_service` 是按键 Service，拥有短时 one-shot 调度和并发收敛状态，不拥有 GPIO、
> Device 生命周期或产品按键语义。

## 1. 定位

- 层级：Service。
- 触发方：Composition Root 启停；BSP GPIO 双边沿经 Device 回调上报活动事实；
  `app_power` 提交 Light Sleep 按键唤醒事实。
- 主要输出：在 ESP Timer Task 上下文同步调用 `button_service_event_cb_t`，产生稳定的左右键
  短按或长按事件。

## 2. 职责边界

负责：

- 拥有一个 `ESP_TIMER_TASK` one-shot Timer，按需推进 Device 双按键状态机。
- 合并 ISR 边沿、扫描期间新边沿和 Light Sleep 按键事实。
- 在同步停止时等待 GPIO 活动回调、Timer 调度、Device 扫描和上层事件回调全部退出。
- 连续扫描故障只记录首次失败和恢复，错误期间继续有限 one-shot 重试。

不负责：

- 不初始化或释放 `device_button`，该生命周期由 Composition Root 管理。
- 不读取 GPIO、不解释引脚编号；GPIO 与 ISR 注册属于 BSP。
- 不决定页面、语音、OTA 或其他产品动作；这些语义属于 Application。
- 不创建独立 Task，不宣称减少 Task 栈。

## 3. 主要流程

清醒按键链路：

```text
GPIO 任一边沿
    → BSP GPIO ISR
    → Device 活动事实
    → Button Service 锁存 edge_pending
    → 10 ms one-shot Timer
    → device_button_scan()
    → 最多两个稳定事件
    → Application 回调
```

清醒空闲时没有永久 10 ms 周期 Timer。按键消抖或长按判定尚未收敛时，Service 每 10 ms
安排下一次 one-shot；稳定且没有新边沿后停表。Timer 回调采样期间到达的新边沿只合并为下一轮
请求，不重复启动同一个 Timer。

Light Sleep 唤醒链路：

```text
app_power 按值提交 EXT1 左右键事实
    → wake_pending_mask
    → one-shot 扫描
    ├─ Device 状态机产生短按/长按 → 清除对应 pending
    └─ 唤醒键已快速释放且 Device 无事件 → 合成一次 SHORT
```

左右键分别占一个事实位和事件槽，同一实体键一轮最多产生一个产品事件。持续按住的键继续由
Device 判定短按或长按；睡眠期间已经快速释放的键重放一次短按。

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_button` | 注册活动回调、推进状态机、复制物理按下状态 |
| 私有依赖 | `esp_timer` | one-shot 调度和 ESP Timer Task 回调 |
| 私有依赖 | FreeRTOS | 临界区、同步停止信号量 |
| 被调用 | Composition Root | 初始化、启动、停止和释放 |
| 被调用 | `app_key` / `app_power` | 消费稳定事件、提交 Light Sleep 唤醒事实 |

Service 只依赖 Device 公共 API，不包含 BSP、Driver、Board 或 Application 私有头文件。

## 5. 公共接口

公共头文件：[`include/button_service.h`](include/button_service.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `button_service_init()` | 同步 | 创建停止信号和未运行的 one-shot Timer |
| `button_service_set_event_callback_borrow()` | 同步 | 在 INITIALIZED 状态设置长期借用回调 |
| `button_service_start()` | 同步 | 注册 Device 活动回调并安排一次初始扫描 |
| `button_service_request_light_sleep_wakeup_copy()` | 异步提交 | 按值复制唤醒事实并请求扫描，最终事件仍由回调返回 |
| `button_service_stop(timeout_ms)` | 同步有界等待 | 禁止新入口并等待所有在途回调退出 |
| `button_service_deinit()` | 同步 | 只在完全停止后删除 Timer 和信号量 |

事件回调在 ESP Timer Task 上下文同步执行，必须快速返回，不得阻塞或访问 LVGL。借用回调在
成功 `deinit()` 前有效，运行期间不能替换。

## 6. 状态、生命周期与并发

```text
UNINITIALIZED → INITIALIZED → RUNNING → STOPPING → INITIALIZED → UNINITIALIZED
                                      └─ 超时后重复 stop() ─┘
```

- 状态所有者：本组件静态上下文；ISR 与普通上下文只在临界区内修改小型运行时事实。
- Task：本组件不创建 Task；Timer 使用 ESP-IDF 已有的 ESP Timer Task。
- `button_service_stop(timeout_ms)` 首次调用进入 STOPPING，先注销 GPIO 活动回调，再等待
  ISR 调度和 Timer 回调退出。
- `ESP_ERR_TIMEOUT` 返回时保持 STOPPING；调用方可以重复 `stop()` 收敛，`start()` 和
  `deinit()` 在此状态均被拒绝。
- `ESP_OK` 返回后保证 GPIO 活动回调、扫描回调和上层事件回调均不再执行。

## 7. 故障与恢复

- Device 扫描或物理状态读取失败时保留待处理事实，并安排后续 one-shot 重试。
- ISR 中不记录普通日志；Timer 调度错误锁存后由 ESP Timer Task 上下文报告。
- Light Sleep 唤醒请求若调度失败，已经复制的事实不会回滚；`app_power` 将其视为恢复错误并
  进入 BLOCKED。
- 产品级降级、重启或继续运行策略仍由 Application 决定。

## 8. 配置与文件

- `button_service_config_t::scan_period_ms` 是活动窗口推进间隔，不是永久扫描周期。
- [`src/button_service.c`](src/button_service.c) 拥有 one-shot 调度、事件合并和同步停止状态。
- 无持久化格式和独立构建开关。

## 9. 验证

- 静态契约：[`../../../tools/tests/check_button_event_driven.ps1`](../../../tools/tests/check_button_event_driven.ps1)。
- 实机检查：左右短按/长按、双键并发、60 秒空闲 Timer 计数和 Light Sleep 唤醒去重。
- 固件构建只能在用户明确要求后通过仓库根目录 `.\dm.ps1 build` 执行。
