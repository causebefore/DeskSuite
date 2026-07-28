# `environment_service`

环境 Service 按调用方需求串行访问 `device_environment` 与 `device_battery`，保存一份线程
安全联合快照，并用无 payload 的轻量事件通知 Application 重新拉取数据。

## 职责

- 串行化同步硬件采样，避免多个调用方并发访问设备。
- 提供联合、仅电池和仅温湿度三种同步采样入口。
- 单项失败保留最近成功值，同时记录该项 `last_error`。
- 不创建 Task、Timer 或 Queue；采样周期由 Application 的 `app_environment` 决定。
- 不初始化或释放两个 Device，不决定 UI 显示与产品故障策略。

生命周期为：

```text
UNINITIALIZED -> INITIALIZED -> UNINITIALIZED
```

`environment_service_sample()` 是同步联合采样；单项入口用于保留 DeskMate 原有的 2 秒电池
周期和 30 秒温湿度周期。返回 `ESP_OK` 表示尝试结果已提交，不代表硬件采样成功，消费者
应通过 `environment_service_get_snapshot_copy()` 复制联合快照，再读取 `valid`、
`last_error` 和 `updated_at_ms`。

该组件保持 PhotoPainter 的按需采样与采样事务设计。DeskMate 的首页和状态栏是常驻消费者，
所以产品周期由 `app_environment` 调用 Service，Presentation 不直接监听 Device。
