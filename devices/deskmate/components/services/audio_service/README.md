# `audio_service`

> Service 层音频资源所有者，统一串行管理输入、输出、音量和静音状态。

## 1. 定位

- 层级：Service。
- 触发方：`app_main`、`audio_processor_service`、`voice_service`。
- 主要输出：同步 PCM 读写结果和当前音频运行状态。

## 2. 职责边界

负责：

- 串行维护输入、输出、音量和静音状态。
- 在底层操作失败时保持 Service 状态与硬件提交结果一致。

不负责：

- 不包含 Board、BSP、Codec 或 I2S 头文件。
- 不初始化或释放 `device_audio`，其生命周期由 Composition Root 管理。
- 不创建 Task，不决定语音会话、重试或页面策略。

## 3. 主要流程

```text
Composition Root: device_audio_init → audio_service_init
app_voice_start → audio_service_start（只开放控制入口，硬件仍关闭）
语音会话 → enable_input/output(true) → device_audio 同步读写 → enable_input/output(false)
app_voice_stop → audio_service_stop（确认输入输出均关闭，保留 Codec/I2S 实例）
audio_service_deinit → 仅从 STOPPED 释放 Service 互斥锁
Composition Root: device_audio_deinit
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_audio` | 型号无关的同步音频能力 |
| 被调用 | `audio_processor_service` | 麦克风 PCM 输入 |
| 被调用 | `voice_service` | TTS PCM 输出与输入启停 |

## 5. 公共接口

公共头文件：[`include/audio_service.h`](include/audio_service.h)

`init/deinit` 只拥有 Service 自身状态与互斥锁；`start/stop` 是可逆 Runtime 生命周期，
`stop` 只关闭数据链路并保留初始化资源。
PCM API 使用 `esp_err_t` 返回错误事实，并通过输出参数返回实际样本数。

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → STOPPED ↔ RUNNING → STOPPED → deinit`。
- 状态所有者：组件私有 context，由互斥锁保护。
- Task：本组件不创建 Task。
- 回调/队列：无。
- `start()` 不打开麦克风或扬声器；只有 `RUNNING` 可执行
  `enable_input/output(true)`。
- `stop()` 与输入输出 enable 操作使用同一控制互斥量串行化；返回 `ESP_OK` 时两条硬件链路
  均已关闭。`audio_service_is_running()` 只表示输入或输出链路活跃，不表示 Runtime 状态。

## 7. 故障与恢复

Device 未初始化时拒绝初始化 Service。输出打开后若恢复音量失败，会立即关闭输出。音量和
静音字段只在底层提交成功后更新。`stop()` 的任一关闭操作失败时记录真实输入输出状态并进入
`CLEANUP_FAILED`；此状态只允许再次 `stop()` 收敛，禁止 `start()` 和 `deinit()`。

## 8. 配置与文件

- `CONFIG_DESKMATE_AUDIO_DEFAULT_VOLUME`

采样率和增益属于 Device 配置，由 Composition Root 读取产品 Kconfig 后传入。

## 9. 验证

- 构建检查全部 PCM 调用方使用 `esp_err_t + out_count`。
- 实机检查 ES8311/ES7210 初始化、失败后重试、输入输出启停和 deinit。
