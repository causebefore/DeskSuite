# `button_service`

> 按键 Service 持有持续扫描定时器，通过同步 Device API 推进去抖状态机，不感知 GPIO、芯片或具体 RTOS。

## 1. 定位

- 层级：Service
- 触发方：Composition Root 初始化并启动，Application 注册快速事件回调
- 主要输出：已去抖的按下、释放、单击、连击和长按事件

创建本 Service 的充分理由是按键状态机需要 10 ms 周期持续推进，并要求该执行资源不属于
Device/BSP。它不是对单个 Device API 的名称转发。

## 2. 职责边界

负责：

- 持有并启停 System 周期定时器。
- 每周期同步调用 `device_button_scan()`。
- 转发不可变设备事件，并对连续扫描故障限频记录、在下一周期自动重试。

不负责：

- GPIO、电平极性、去抖、连击和长按算法，这些由 BSP 与 `button_driver` 完成。
- 产品级事件含义、致命/降级判断和重启策略，这些由 Application 决定。
- 初始化或释放 `device_button`，其生命周期由 Composition Root 装配。

## 3. 主要流程

```text
System 10 ms 周期定时器
    ↓
device_button_scan(elapsed_ms)
    ↓
BSP 同步采样 → button_driver 状态机
    ↓
Device 事件 → button_service 回调 → Application
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_button` | 同步推进状态机并接收稳定设备事件 |
| 调用 | `sys` | 使用不泄漏具体 RTOS 类型的周期定时器 |
| 被调用 | Composition Root / Application | 生命周期装配与事件消费 |

## 5. 公共接口

公共头文件：[`include/button_service.h`](include/button_service.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `button_service_init()` | 同步 | 注册 Device 入口并创建未运行的定时器 |
| `button_service_set_event_callback_borrow()` | 同步 | 设置或显式清除停止期间的长期回调借用 |
| `button_service_start()` | 同步 | 启动 10 ms 周期执行 |
| `button_service_stop()` | 同步 | 停止周期执行并保留上层回调借用，可直接再次启动 |
| `button_service_deinit()` | 同步 | 销毁定时器并解除 Device 回调 |

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → INITIALIZED → RUNNING → INITIALIZED → UNINITIALIZED`。
- 状态所有者：Service 定时执行上下文推进按键状态机；生命周期 API 由 Composition Root 串行调用。
- Task：不创建专用 Task；拥有一个通过 System 抽象创建、运行在 System 定时上下文的周期 Timer。
- 回调：在周期定时上下文同步执行，必须快速返回；借用在替换、显式清除或 `deinit()` 时结束。
  `stop()` 成功后不再安排新的扫描，但调用时已经开始的定时回调仍可能完成；原回调保持不变，
  后续 `start()` 可以直接恢复投递。
- 队列：本组件不创建队列，不存在队列满策略。

## 7. 故障与恢复

- 初始化任一步失败都会回滚已注册的 Device 回调或已创建资源。
- 周期扫描失败只记录第一次错误，后续周期自动重试；恢复时记录一次恢复事实。
- 是否因持续按键故障降级或重启由 Application 决定。

## 8. 配置与文件

- 扫描周期固定为 10 ms，与 `button_driver` 的默认去抖时间配合。
- `src/button_service.c` 持有全部生命周期和周期调度状态。
- 无持久化格式。

## 9. 验证

- 静态检查应确认 Service 不包含 BSP、Driver、Board、FreeRTOS 或 ESP Timer 头文件。
- 实机检查应覆盖三键按下/释放、单击、双击、多击和长按事件。
- 当前没有自动化测试；Task/Timer 的最低测试门槛仍按架构未决边界处理。
