# `audio_service`

> Service 层唯一音频输出事务所有者，统一执行 PCM 流与 SD 卡 MP3 播放。

## 1. 定位

- 层级：Service。
- 触发方：Composition Root、`voice_service`、`app_pomodoro`。
- 主要输出：单写入者音频输出、文件播放终态事件和完整运行摘要。

## 2. 职责边界

负责：

- 私有 `AudioServiceRuntime` 独占播放 Task、输出启停、PCM StreamBuffer、MP3 解码器、
  声道转换与输出重采样。
- 串行化一个 PCM 流和一个文件请求槽；PCM 流始终高于文件播放。
- 把每个已接受文件请求收敛为唯一 `COMPLETED`、`CANCELLED` 或 `FAILED` 终态事件。

不负责：

- 不拥有麦克风输入，不包含 Board、BSP、Codec 或 I2S 头文件。
- 不初始化或释放 `device_audio`，其生命周期由 Composition Root 管理。
- 不决定番茄钟状态、语音网络重试、页面或文件选择策略。

## 3. 主要流程

```text
Composition Root: device_audio_init → audio_service_init → audio_service_start

Voice TTS: open_pcm_stream(24 kHz mono)
    → write_pcm_stream_borrow
    → close_pcm_stream(drain/discard)

Pomodoro: request_play_mp3_file_copy
    → 4 KiB 分块读取与 MP3 Simple Decoder
    → 双声道按需转单声道
    → 按需重采样到 Device 采样率
    → FILE_PLAYBACK_FINISHED
```

Audio Service 在设备运行期只启动一次。Light-sleep 期间播放 Task 保留并无限期停泊；只有整机
启动回滚或显式关闭才执行 `stop(timeout_ms) → deinit → device_audio_deinit`。

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_audio` | 独占扬声器启停、默认音量提交和 PCM 写入 |
| 调用 | `espressif/esp_audio_codec 2.5.0` | MP3 Simple Decoder |
| 调用 | `esp_audio_effects ~1.2.1` | 声道转换与输出重采样 |
| 被调用 | `voice_service` | 提交 24 kHz 单声道 TTS PCM |
| 被调用 | `app_pomodoro` | 提交或取消完成代次对应的 MP3 文件请求 |

## 5. 公共接口

公共头文件：[`include/audio_service.h`](include/audio_service.h)。公共边界保持 C ABI、POD
参数和中文 Doxygen；C++ 类与 `.hpp` 只在组件内部可见。

- 生命周期：`init/start/stop/deinit/get_status_copy`。
- 文件事务：`request_play_mp3_file_copy/request_cancel_file_playback`。
- PCM 事务：`open/write/close_pcm_stream`。

`request_id` 与 `stream_id` 都是非零 `uint64_t`。PCM 固定为有符号 16-bit 交错样本，
`sample_count` 表示样本值数量；写 API 返回前完成复制并明确返回部分写入数量。

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → STOPPED ↔ RUNNING → STOPPED → deinit`。
- Task：`audio_playback` 使用 20 KiB PSRAM 栈、优先级 3；空闲时等待 Task Notification，
  不执行周期轮询。
- 唯一写入者：只有播放 Task 能改变活动播放状态、启停输出和调用 `device_audio_write()`。
- PCM 优先：打开 PCM 会取消在播 MP3；PCM 期间首个文件请求进入待处理槽，排空后再播放。
- 文件槽：相同请求 ID 幂等合并，不同 ID 在槽占用时返回 `ESP_ERR_INVALID_STATE`。
- 同步关闭：`discard=false` 排空，`discard=true` 丢弃；超时保留同一流 ID 供调用方重试。

## 7. 故障与恢复

输出只在首块有效 PCM 到达时打开，排空、取消或失败后关闭。文件缺失、为空、损坏、截断、
格式不支持或转换失败都发布 `FAILED`，不改变上层产品状态。MP3 播放按解码样本时长在 30 秒
结束，不支持 Seek，抢占后不续播。

`stop()` 请求 Task 协作取消并有界等待；输出不能关闭或 Task 未退出时进入
`CLEANUP_FAILED`，保留资源供再次 `stop()` 收敛，禁止伪装为已停止。

## 8. 配置与文件

- `CONFIG_DESKMATE_AUDIO_DEFAULT_VOLUME`
- `CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES`，首轮为 262144 字节
- `CONFIG_FATFS_FS_LOCK=5`
- 组件清单固定 `espressif/esp_audio_codec: "2.5.0"` 与
  `espressif/esp_audio_effects: "~1.2.1"`

只启用 MP3 Decoder；其他解码器、编码器和容器均关闭。

## 9. 验证

- 运行 `tools/tests/check_audio_runtime_contract.ps1`。
- 实机覆盖单/双声道与 16/22.05/24/44.1/48 kHz MP3、TTS 抢占、排队、取消、30 秒上限和
  文件异常。
- 构建、声音质量、Heap/栈水位和物理设备验收分别记录，不能互相替代。
