# `rtc_service`

> RTC Service 持续等待板载 RTC 的 INT 事实，在普通 Task 上下文核实并清除告警标志，再向
> Application 发布不可变事件。

## 1. 定位

- 层级：Service。
- 触发方：Composition Root 初始化并启动；RTC GPIO15 下降沿触发运行期处理。
- 主要输出：告警成功事件、处理失败事件和可复制运行摘要。

## 2. 职责边界

负责：

- 把 Device 提供的 ISR 快速通知转换成普通 Task 上下文工作。
- 串行读取和清除 PCF85063 的 AF，确保低电平 INT 被释放，并在临时 I2C 失败时执行有界技术性重试。
- 保存本轮累计告警数和最后一次处理错误，并向 Application 发布事件。

不负责：

- 不初始化或释放 `device_rtc`，也不访问 BSP、Board、GPIO 或芯片 Driver。
- 不决定告警何时设置、触发后切换哪个页面、重试还是降级。
- 不参与 RTC、系统时钟和 SNTP 之间的可信时间校准。

Device 初始化前提是底层 Driver 已关闭项目未使用的分钟、半分钟和计时器中断源；因此 Service
只消费 AF，不猜测芯片私有中断来源。

## 3. 主要流程

```text
GPIO15 下降沿
    → Device ISR 回调只通知 rtc_service Task
    → Task 读取 AF；AF 置位时通过 I2C 清除
    → 失败时按有界退避重试
    → 更新运行摘要
    → 在 Service Task 上下文通知 Application
```

`start()` 还会主动触发一次 AF 检查，以消费 Service 启动前已经置位的告警。

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_rtc` | 注册快速 ISR 通知、读取和清除告警标志 |
| 调用 | `utils` | 低频输出 RTC Task 的历史最小剩余栈 |
| 被调用 | `main/app_main.c` | 装配生命周期并消费告警事件 |

公开头文件不泄漏 FreeRTOS 类型；Task、通知和停止信号量都是私有实现资源。

## 5. 公共接口

公共头文件：[`include/rtc_service.h`](include/rtc_service.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `rtc_service_init()` | 同步 | 创建停止同步资源，进入 `INITIALIZED` |
| `rtc_service_set_event_callback_borrow()` | 同步 | 在停止状态设置长期借用的事件回调 |
| `rtc_service_start()` | 同步 | 创建 Task、注册 Device ISR 回调并检查已有 AF |
| `rtc_service_request_check()` | 异步提交 | 通知 Task 主动检查当前 AF |
| `rtc_service_stop()` | 同步有界等待 | 注销 ISR 回调并等待 Task 终止；超时后可重复调用继续收敛 |
| `rtc_service_get_status_copy()` | 同步 | 复制生命周期、累计告警数和最后错误 |
| `rtc_service_deinit()` | 同步 | 释放已停止 Service 的资源 |

回调收到的事件地址只在回调期间有效；执行上下文是 RTC Service Task。

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → INITIALIZED → STARTING → RUNNING → STOPPING → INITIALIZED`。
- 状态所有者：Service 内部临界区保护生命周期、Task 句柄、回调和快照字段。
- Task：运行期创建一个阻塞等待 Task；无中断时不轮询。
- 栈统计：Task 首次运行和后续被唤醒时按 60 秒周期节流输出，并在退出前输出最终值。
- ISR：只复制 Task 句柄并使用 Task Notification 投递位，不做 I2C、日志或产品逻辑。
- 停止超时后保留 `STOPPING`，重复 `stop()` 会等待同一 Task；Task 实际退出后自行收敛为
  `INITIALIZED`，此后再次停止幂等返回成功。

## 7. 故障与恢复

- AF 读取或清除失败会保留错误、记录中文日志并发布 `PROCESSING_FAILED`，随后以 1、2、4 秒
  进行最多三次技术性重试；重试耗尽后保留错误，等待下一次中断或显式检查。
- GPIO15 为低但 AF 未置位时，Service 不猜测芯片私有中断来源；Driver 初始化负责清理未支持
  来源，显式电平诊断仍由 BSP 拥有。
- 启动或停止失败由 Composition Root/Application 决定是否降级；Service 不重启设备。

## 8. 配置与文件

- 当前 Task 栈为 3072 字节、优先级为 4，均为组件私有有界配置。
- `src/rtc_service_task.c`：生命周期、ISR thunk、Task 和告警消费事务。
- 不拥有持久化格式。

## 9. 验证

- 静态核查：ISR 不执行 I2C，外部回调在内部锁外调用，停止超时保留显式状态。
- 实机检查：设置未来告警后确认 GPIO15 拉低、Service 清除 AF、INT 恢复高电平且事件计数增加。
- 已知缺口：当前没有自动化硬件在环测试。
