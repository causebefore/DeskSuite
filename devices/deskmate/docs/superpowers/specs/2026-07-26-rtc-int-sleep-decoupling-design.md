# RTC INT 与轻睡眠解耦设计

## 目标

删除临时的 PCF85063 每 30 秒闹钟测试，并让 RTC INT GPIO15 完全退出轻睡眠控制链路。
GPIO15 的电平、告警标志或输出异常不再阻止设备进入轻睡眠，也不能唤醒设备。

保留 RTC 日历读写和清醒期间的 `rtc_service` 告警消费能力。轻睡眠无活动窗口继续使用
`DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC=60`。

## 边界

本次删除：

- `app_rtc_alarm` Application 及其构建、Kconfig、启动和事件回调接线；
- 轻睡眠事务对 `rtc_service` 的暂停、恢复及失败回滚；
- BSP 进入轻睡眠前对 RTC INT 电平的检查；
- GPIO15 在 EXT1 唤醒掩码中的配置；
- BSP 和 Device 电源唤醒结果中的 `rtc_interrupt` 字段；
- 只服务于轻睡眠暂停/恢复的 `rtc_service` 公共 API；
- 文档中 RTC INT 可唤醒、可阻止睡眠及 30 秒测试的描述。

本次保留：

- PCF85063 I2C 日历、告警寄存器和 AF 操作 API；
- RTC INT GPIO 输入、GPIO ISR 与 `rtc_service` 的清醒期消费链路；
- RTC Service 发布的告警事件和现有活动窗口通知；
- 左右按键 GPIO18、GPIO0 的 EXT1 任意低电平唤醒；
- 60 秒轻睡眠无活动窗口。

## 架构与数据流

清醒期间的数据流保持不变：

```text
PCF85063 INT 下降沿
  → BSP GPIO ISR
  → Device RTC 回调
  → rtc_service Task 读取并清除 AF
  → app_main 接收告警事件
  → app_power_notify_activity()
```

轻睡眠数据流调整为：

```text
60 秒无活动
  → Application 停止网络、环境、UI 和按键扫描
  → BSP 只确认左右按键已释放
  → EXT1 只配置 GPIO18、GPIO0 任意低电平唤醒
  → 进入轻睡眠
  → 左键或右键唤醒
  → 恢复运行期组件
```

`rtc_service` 不再参与轻睡眠准备或恢复。CPU 处于轻睡眠时不会执行 Service Task；该期间
GPIO15 的变化既不属于唤醒契约，也不保证在按键唤醒后补消费。清醒期间的新下降沿仍按现有
RTC Service 链路处理。

## API 与实现调整

### Application

- 从 `app_main` 删除 `app_rtc_alarm` 包含、周期重排和启动调用；
- 从 `app_power_task` 删除 RTC 消费暂停状态、暂停步骤和恢复步骤；
- 保留普通 RTC 告警事件对活动窗口的通知。

### Service

- 删除 `rtc_service_pause_interrupt_consumption()`；
- 删除 `rtc_service_resume_interrupt_consumption()`；
- 删除只为暂停屏障存在的 `RTC_SERVICE_STATE_PAUSED`、`s_pause_requested` 和消费互斥锁；
- RTC Service Task 继续作为 AF I2C 事务的唯一串行消费者；
- 保留 Service 的初始化、启动、停止、ISR 通知、AF 消费和技术性重试。

### Device 与 BSP

- `device_power_wakeup_info_t` 和 `bsp_power_wakeup_info_t` 只保留左右按键字段；
- BSP 唤醒掩码只包含 GPIO18 和 GPIO0；
- BSP 准备阶段只检查两个按键是否释放；
- 睡眠前后日志只描述左右按键；
- RTC BSP 自身的 GPIO ISR、告警标志和中断电平诊断 API 不因本次解耦而扩展或重构。

## 错误处理

- 左右按键未释放时仍返回 `ESP_ERR_INVALID_STATE`，阻止本轮轻睡眠；
- RTC INT 读取不再发生于睡眠准备，因此 GPIO15 持续低或 RTC I2C 诊断失败不会影响睡眠；
- EXT1 配置与取消仍按现有错误路径回滚；
- `rtc_service` 清醒期消费失败仍使用现有 1、2、4 秒重试策略。

## 验证

先增加并运行 `tools/tests/check_rtc_sleep_decoupling.ps1`，确认它因当前代码仍包含以下耦合
而失败：

- `bsp_power.c` 的唤醒掩码或睡眠准备引用 `BOARD_RTC_PIN_INT`；
- `app_power_task.c` 调用 RTC Service 暂停或恢复 API；
- Power 唤醒结构包含 `rtc_interrupt`；
- `app_rtc_alarm` 仍在构建或启动链路中。

完成修改后重复运行同一检查，要求上述引用全部消失，并确认：

- 左右按键仍存在于 EXT1 唤醒掩码；
- 60 秒配置保持不变；
- `rtc_service` 的清醒期初始化、启动和事件回调仍存在；
- `git diff --check` 无格式错误。

硬件验收时，GPIO15 即使保持低电平，设备也应在 60 秒无活动后进入轻睡眠；轻睡眠只由左右
按键唤醒。清醒期间产生新的 RTC INT 下降沿时，`rtc_service` 仍应消费 AF 并发布告警事件。
