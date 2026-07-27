# `voice_service`

> Service 层语音会话所有者，串行完成录音、上传、协议解析和流式播放事务。

## 1. 定位

- 层级：Service。
- 触发方：App 按键、唤醒和测试流程。
- 主要输出：语音回合状态事件、服务端文本事实和 TTS 播放结果。

## 2. 职责边界

负责：

- 独占一次语音回合的取消状态、录音缓冲、网络会话和播放队列。
- 优先使用 WebSocket，连接失败时执行 HTTP 流式回退。
- 会话启动时一次性复制 App Voice 提供的完整 `protocol_backend_context_t`；HTTP 与
  WebSocket 从同一值快照读取服务地址、Token 和稳定设备 ID。
- 通过 `audio_processor_service` 收集降噪 PCM，通过 `audio_service` 播放 TTS。

不负责：

- 不包含 BSP、Board、Codec 或 I2S 头文件。
- 不决定页面展示、用户输入含义、重启和全局降级策略。
- 不实现 Wi-Fi、HTTP/WebSocket 底层协议。

## 3. 主要流程

```text
chat 请求
    → 后台会话 Task 录音
    → Transport 上传并接收协议帧
    → 播放 Task 消费 TTS PCM
    → DONE / CANCELLED / ERROR 终态
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `audio_processor_service` | 降噪录音 |
| 调用 | `audio_service` | 输入启停和 TTS 播放 |
| 调用 | `transport` / `protocols` | 网络传输与帧协议 |
| 调用 | `utils` | 输出会话与播放 Task 的最终历史最小剩余栈 |
| 被调用 | App Voice | 提供统一后端上下文、产品触发、取消和结果解释 |

## 5. 公共接口

公共头文件：[`include/voice_service.h`](include/voice_service.h)

`voice_service_chat()` 在返回前复制后端上下文并异步提交会话；实际完成结果由语音状态通知
返回。
`voice_service_deinit()` 会拒绝新会话、取消当前会话并有界等待其退出，然后释放组件资源；
它不初始化或释放所依赖的 Audio Service。

## 6. 状态、生命周期与并发

- 生命周期：`UNINITIALIZED → IDLE → BUSY → IDLE → deinit`。
- 状态所有者：会话 context 和事件组。
- Task：`voice_chat` 拥有会话；`voice_play` 串行播放 PCM。两个一次性 Task 的栈均从 PSRAM
  分配，避免 WebSocket 会话建立后的内部堆峰值阻止播放 Task 启动。
- 栈统计：两个一次性 Task 都在退出前向串口输出历史最小剩余字节数。
- 停止：`cancel` 只请求协作取消，由会话 Task 完成资源回收。
- 反初始化：先关闭新会话入口，再取消并等待活动会话；超时则保留资源和停止状态，禁止在
  Task 尚未退出时释放依赖。

## 7. 故障与恢复

无法联网、后端上下文无效、任务创建失败或协议错误均收敛为明确终态。空 Token 允许连接服务端
局域网开发模式。deinit 等待超时返回
`ESP_ERR_TIMEOUT`，可再次调用以继续收敛。是否展示错误、重试或禁用语音由 Application
决定。

## 8. 配置与文件

Task、缓冲和协议的精确契约以
[`include/voice_service.h`](include/voice_service.h) 及实现为准；通用并发规则见
[`../../../docs/architecture/data_flow.md`](../../../docs/architecture/data_flow.md)。

## 9. 验证

- 检查正常完成、用户取消、WS→HTTP 回退和播放失败终态。
- 检查第二次语音回合不会继承上一次的缓冲或取消状态。
