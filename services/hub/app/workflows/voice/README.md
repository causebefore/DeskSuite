# Voice 工作流

Voice 是设备音频通道的适配层，入口仍为 `POST /api/v1/voice/chat` 和
`/api/v1/voice/ws`。它不实现第二套 AI 对话，只负责以下数据流：

- `workflow.py`：重采样、ASR/Assistant/TTS 编排、取消和心跳。
- `protocol.py`：设备二进制帧编解码与流式句子切分。

```text
设备 raw PCM → 16kHz WAV → SpeechProvider ASR
→ AssistantWorkflow → 工具状态事件 + 回复文字
→ SpeechProvider 流式 TTS → 24kHz PCM → 设备帧
```

## 输入与会话

- 输入为单声道 16-bit PCM，HTTP 请求可通过 `X-Audio-Sample-Rate` 指定采样率。
- `X-Device-Id` 仍标识物理设备，不再作为用户身份。
- 可选 `X-Thread-Id` 用于让文字和语音进入同一会话；旧设备不发送时，稳定映射为
  `voice:<device_id>`。
- 唯一用户身份由服务器 `[assistant].principal_id` 固定配置。

## 下行协议

每帧为 `[type:1][length:4 big-endian][payload]`。帧类型保持兼容：END `0x00`、
ASR_TEXT `0x01`、REPLY_TEXT `0x02`、TTS_PCM `0x03`、THINKING `0x04`、
ERROR `0x80`。收到 `tool_started` 时立即发送 THINKING，长时间等待时继续发送周期性
THINKING 保活；错误以 ERROR 后 END 收口。
TTS PCM 保证是 24kHz、单声道、16-bit，并且每帧为偶数字节。

## 状态与失败边界

Voice 本身不保存消息和长期记忆。短期会话属于 Assistant 的 SQLite Checkpoint，
长期事实属于 Assistant 的 LangGraph Store。ASR/TTS 失败只终止当前语音回合；Assistant、MCP 或记忆不可用
不会改变设备帧格式。智谱密钥缺失时返回中文 ERROR，不发起网络请求。

## 诊断日志

Voice 在 `info` 日志中记录同一 `device_id` 和 `thread_id` 下的 ASR 识别文本、
Assistant 输入与完整回复、工具名/调用 ID/状态、每段 TTS 输入及生成的 PCM 字节数。
Assistant 日志表示模型请求工具，MCP 日志表示实际远程调用开始与结束；日志不记录原始
音频、工具返回内容或密钥，可据此判断异常文本最早出现在哪一段处理链路。

## 本地测试

在 `services/hub` 运行 `uv run pytest -q tests/test_voice_workflow.py
tests/test_voice_api.py tests/test_voice_websocket.py`。这些测试使用假 SpeechProvider 和
假 Assistant，不调用智谱、邮箱或 MCP。
