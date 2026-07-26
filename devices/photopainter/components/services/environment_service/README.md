# `environment_service`

> 环境 Service 按调用方需求串行采集温湿度与电池状态，并提供一次尝试内一致的联合快照。

## 1. 定位

- 层级：Service
- 触发方：`device_status_upload_app` 在每次上传前同步触发
- 主要输出：最近成功的温湿度、电压和电量，以及每项最近采样错误与更新时间

本 Service 负责把两个同步 Device API 收敛成一份联合快照。它不创建周期 Task，采样时机由需要
数据的 Application 决定，避免设备在没有消费者时持续唤醒传感器和 ADC。

## 2. 职责边界

负责：

- 按需串行调用 `device_environment_measure()` 与 `device_battery_get_status_copy()`。
- 用采样事务锁避免多个调用方并发访问硬件。
- 在短时快照锁内提交和复制整结构状态。
- 保留每项最近一次成功值，记录最近尝试错误，并对连续故障日志限频。

不负责：

- 周期调度、上传、告警、显示刷新、休眠或重试策略。
- 传感器型号、I²C、ADC、分压控制和电量曲线换算。
- 初始化或释放两个 Device 能力，其生命周期由 Composition Root 装配。

## 3. 主要流程

```text
Application → sample()
    → 取得采样事务锁
    → 温湿度采样 → 电池采样
    → 快照锁内提交本次联合结果
    → 输出采样字段、结果和总耗时 → 返回

Application → get_status_copy() → 快照锁内整结构复制，不触发硬件 I/O
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_environment` | 同步采集温度与相对湿度 |
| 调用 | `device_battery` | 同步采集电压并换算电量 |
| 私有实现 | FreeRTOS / ESP Timer | 互斥事务和单调时间戳 |
| 被调用 | Composition Root | 装配生命周期 |
| 被调用 | `device_status_upload_app` | 上传前触发采样并读取联合快照 |

## 5. 公共接口

公共头文件：[`include/environment_service.h`](include/environment_service.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `environment_service_init()` | 同步 | 创建采样事务与快照资源，不访问硬件 |
| `environment_service_sample()` | 同步阻塞 | 完成一次联合采样并发布尝试结果，约 210 ms |
| `environment_service_get_status_copy()` | 同步 | 复制已有快照，不访问硬件 |
| `environment_service_deinit()` | 同步 | 释放 Service 内部资源 |

`sample()` 返回 `ESP_OK` 表示本次尝试已发布，不代表两个硬件项目都成功。调用方必须分别检查
温湿度与电池的 `valid`、`last_error` 和 `updated_at_ms`；单项失败时保留该项最近成功值。

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → INITIALIZED → UNINITIALIZED`。
- 状态所有者：持有采样事务锁的调用方在短时快照锁内更新状态。
- Task：本组件不创建 Task；硬件 I/O 在 `sample()` 调用方 Task 上下文执行。
- 锁：采样事务锁覆盖完整硬件访问，快照锁只覆盖整结构更新和复制。
- 回调/队列：无。

生命周期 API 由 Composition Root 串行调用；`deinit()` 期间不得并发采样或读取。

## 7. 故障与恢复

- 温湿度与电池错误彼此独立，任一项失败不会阻止另一项更新。
- 单项失败由下次按需采样自然重试，仅首次失败与恢复时记录状态日志。
- 内部锁创建失败时不进入初始化状态。
- 是否接受旧值、跳过上传或安排重试由 Application 决定。

## 8. 配置与文件

- 无采样周期配置，也无 Kconfig。
- `src/environment_service.c`：按需联合采样、快照和生命周期。
- 持久化格式：无，所有状态只保存在内存中。

## 9. 验证

- 静态检查：公共头不泄漏 Device/FreeRTOS 类型，快照锁内无 I/O、日志或延时。
- 构建：仅用户明确要求时执行 `& .\build_tools\dm.ps1 build`。
- 实机：检查上传前只采样一次、采样耗时日志、单项故障保留旧值和恢复日志。
- 已知缺口：当前没有自动化测试，实际采样阻塞时长仍需实机测量。
