# `voice_service`

> Service 层语音会话所有者，只编排录音、网络协议和 TTS PCM 提交。

## 1. 定位

- 层级：Service。
- 触发方：`app_voice` 的按键或唤醒语音流程。
- 主要输出：语音回合状态事件、服务端文本事实和会话终态。

## 2. 职责边界

负责：

- 私有 `VoiceServiceRuntime` 独占会话门、取消状态、后端快照、录音缓冲和 `voice_chat` Task。
- 通过 `audio_processor_service` 收集 16 kHz 单声道降噪 PCM。
- 优先执行 WebSocket 会话，按明确事实决定是否回退 HTTP。
- 收到首个有效 TTS 帧后打开 Audio Service 的 24 kHz 单声道 PCM 流。

不负责：

- 不拥有播放 Task、播放 StreamBuffer、输入/输出硬件状态或 AFE Task。
- 不包含 BSP、Board、Codec 或 I2S 头文件。
- 不决定页面展示、网络租约、重启或全局降级策略。

## 3. 主要流程

```text
chat 请求
    → voice_chat 采集 AFE PCM
    → WebSocket 上传并接收协议帧
    → 首个 TTS PCM 打开 Audio Service PCM 流
    → 正常 END 排空关闭；协议/网络/取消错误丢弃关闭
    → DONE / CANCELLED / ERROR
```

WebSocket 只有在尚未成功上传任何 PCM 字节且尚未收到任何响应时才允许回退 HTTP；部分上传
或已经收到响应后失败，不重复提交同一语音回合。

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `audio_processor_service` | 降噪录音事务 |
| 调用 | `audio_service` | 唯一 TTS PCM 输出事务 |
| 调用 | `transport` / `protocols` | HTTP、WebSocket、身份与帧协议 |
| 被调用 | App Voice | 提供后端上下文、产品触发、取消和终态解释 |

## 5. 公共接口

公共头文件：[`include/voice_service.h`](include/voice_service.h)。公共边界保持 C ABI 与 POD
类型；`VoiceServiceRuntime` 和 `.hpp` 仅在组件内部可见。

`voice_service_request_chat()` 返回前复制完整 `protocol_backend_context_t`，并把录音时长限制为
2000～10000 ms。`stop()` 不取消活动会话；`deinit()` 仅从 `STOPPED` 释放自身资源，不释放
Audio Service 或 Processor。

## 6. 状态、生命周期与并发

- Runtime：`UNINITIALIZED → STOPPED ↔ RUNNING → STOPPED → deinit`。
- 会话：仅在 `RUNNING` 中执行 `IDLE → BUSY → IDLE`。
- Task：仅有 `voice_chat`，使用 12288 字节 PSRAM 栈；入口、句柄和删除逻辑位于
  `src/voice_service_task.cpp`。
- 播放：协议回调只向 Audio Service 复制 PCM，不写硬件、不维护第二套播放状态。
- 取消：`cancel()` 只发布协作取消事实，由会话 Task 丢弃 PCM、关闭网络并完成资源回收。
- 终态顺序：收敛采集/网络/PCM → 发布终态 → 释放会话门 → Task 退出。

## 7. 故障与恢复

任务创建失败、无有效语音、网络错误、协议错误、PCM 写入或关闭错误都收敛为明确会话终态。
Audio Service 的同步关闭若超时，会立刻升级为丢弃请求；底层仍保留真实清理状态，不把超时
伪装成成功。是否展示错误、重试或禁用语音由 Application 决定。

## 8. 配置与文件

录音时长、VAD、HTTP/WebSocket 超时及队列参数位于 `DeskMate Audio/Voice` Kconfig 菜单；
PCM 抗抖动容量由通用 `CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES` 提供。

## 9. 验证

- 检查 WebSocket 建连前失败可回退 HTTP，部分上传或收到响应后失败不得重复提交。
- 连续与交替执行语音、MP3 和抢占场景，确认只有 Audio Service 调用输出硬件。
- 运行 `tools/tests/check_audio_runtime_contract.ps1` 与
  `tools/tests/check_voice_power_lifecycle.ps1`。
