# `audio_processor_service`

> Service 层 AFE 处理事务，拥有双麦重采样、降噪、VAD、WakeNet 和处理 Task。

## 1. 定位

- 层级：Service。
- 触发方：`app_main` 初始化，`voice_service` 启停一次 PCM 收集会话。
- 主要输出：16 kHz 单声道降噪 PCM、VAD 状态和唤醒事实。

## 2. 职责边界

负责：

- 将 Device 音频提供的双通道硬件 PCM 重采样后送入 ESP-SR AFE。
- 串行拥有 feed/fetch Task、AFE 状态和会话输出缓冲区。
- 根据构建配置执行常驻唤醒检测或按需启动处理 Task。

不负责：

- 不操作 BSP、Board、Codec 或 I2S。
- 不决定语音页面、联网、服务端请求和错误降级策略。

## 3. 主要流程

```text
audio_service_read
    → 24 kHz 双通道 PCM
    → 重采样为 16 kHz
    → AFE feed/fetch
    → 调用方提供的单声道输出缓冲区
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `audio_service` | 同步读取麦克风 PCM |
| 调用 | ESP-SR / Audio Effects | AFE 与重采样 |
| 调用 | `utils` | 低频输出 feed/fetch Task 的历史最小剩余栈 |
| 被调用 | `voice_service` | 语音收集会话 |

## 5. 公共接口

公共头文件：[`include/audio_processor_service.h`](include/audio_processor_service.h)

`capture_start()` 借用调用方输出缓冲区，直至配对 `capture_stop()` 完成。
`deinit()` 只在没有活动收集时协作停止 Task，并释放 AFE、模型、重采样器和缓冲。精确容量和
返回语义以头文件为准。

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → INITIALIZED → CAPTURING → INITIALIZED → deinit`。
- 状态所有者：组件私有 context。
- Task：`aps_feed` 读取 PCM，`aps_fetch` 推进 AFE 输出。
- 栈统计：两个 Task 首次运行时输出，之后按 60 秒周期节流，并在退出前输出最终值。
- 停止：会话 stop 只排空本轮 AFE 数据；组件 deinit 使用有限等待让 Task 协作退出，不强杀
  正阻塞在 AFE 内部的 Task。

## 7. 故障与恢复

初始化、任务创建和缓冲分配错误会清理已取得资源后返回调用方。Task 未在期限内退出时保留
资源并返回 `ESP_ERR_TIMEOUT`，允许调用方再次收敛；是否禁用语音或重启由 Application 决定。

## 8. 配置与文件

主要配置位于 `DeskMate Audio/Voice` Kconfig 菜单。

## 9. 验证

- 检查 24 kHz 双通道输入到 16 kHz 单通道输出。
- 检查重复会话、drain 终态和 I2S 异常时不会忙循环。
