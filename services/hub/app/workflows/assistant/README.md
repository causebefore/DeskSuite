# Assistant 工作流

Assistant 是文字和语音共同使用的唯一 AI 编排入口。它负责 LangChain Agent、LangGraph
SQLite Checkpoint、SQLite Store、本地工具和可选 MCP；不处理 PCM、TTS 或设备帧。

## 文件职责

- `workflow.py`：Agent 生命周期、SQLite Checkpoint/Store、线程串行和渠道无关事件。
- `SYSTEM_PROMPT.md`：Assistant 的静态系统提示词；Assistant 懒启动时读取，修改后需重启 Hub。
- `context.py`：可信的用户、会话、渠道与设备上下文。
- `tools.py`：现有只读数据服务和显式长期记忆的 LangChain 工具适配。
- `mcp.py`：智谱 `webSearchPrime` 的可选加载、鉴权和工具白名单。

## 数据流

```text
用户文字或 ASR 文字
  → AssistantTurn(text, thread_id, channel, device_id)
  → 读取同一 thread 的 LangGraph Checkpoint
  → GLM 判断直接回答或调用工具
  → 仅在问题涉及旧事实时，由 search_user_memory 主动 search
  → 明确要求记住时，由 remember_user_fact 主动 put
  → 流式 text/tool_started/tool_finished/tool_error/final AssistantEvent
  → 文字 API 收集最终文字 / Voice 工作流按句送 TTS
```

当前为单用户部署，可信身份固定为配置中的 `owner`。`device_id` 只表示请求来源，
`thread_id` 表示短期连续对话；SQLite 内部使用 `owner:thread_id`，避免未来扩展身份时串话。

## 工具

- `get_weather`：只读天气。
- `get_calendar`：只读近期日程。
- `get_mail`：只读邮件摘要，不标记已读。
- `get_quota`：只读智谱额度。
- `search_user_memory`：仅在需要过去明确保存的个人事实或偏好时检索；语义索引异常时
  回退最近记录。
- `remember_user_fact`：只在用户明确要求时保存个人事实或偏好；单条规范化后最多 500 字符。
- `webSearchPrime`：可选智谱联网搜索 MCP，由配置控制并按白名单加载；当前
  LangChain 适配器暴露的运行时名称为 `web_search_prime`，两者视为同一能力。

邮件、天气、日程、额度和联网搜索结果不得写入长期记忆。数据服务由 `app.main`
创建并注入，工具不能自行创建第二个客户端，以继续复用既有缓存。

## 状态与恢复

- 短期完整消息历史：`data/assistant/checkpoints.sqlite3`。
- 长期事实/偏好：`data/assistant/memories.sqlite3`，由 LangGraph Store 按
  `("assistant_memories", principal_id)` 隔离。
- Store 使用智谱 `embedding-3` 为 `fact` 字段建立语义索引；缺少 Key 或 embedding
  暂时失败时回退为最近事实检索，不影响短期多轮。
- 相同事实使用稳定 SHA-256 key，重复保存会更新同一条记录。
- 同一 `thread_id` 的请求串行执行，避免并发覆盖对话顺序。
- Hub lifespan 在启动时预热 Agent 并发现 MCP 工具；Assistant 使用专用事件循环统一管理
  异步 Checkpointer 与 Store，`close()` 在应用退出时统一释放。
- `web-search-prime` 不保存跨调用状态，因此采用适配器默认的无状态模式：每次真正调用
  工具时创建并清理独立 session，避免长时间空闲后复用过期 session。
- Agent 使用稳定的 LangGraph v2 `messages + updates` 流：`messages` 输出模型文本，`updates`
  投影工具请求、结果和错误；摘要中间件内部输出不会进入 TTS。
- 对联网搜索设置单回合一次的工具调用上限；长会话在接近上下文窗口前自动摘要并保留
  最近消息。

## 错误与降级

- 未配置 GLM Key 时，首次 Assistant 请求返回明确错误，不影响显示、Dashboard、OTA 和日志。
- MCP 未启用、缺 Key、握手失败或工具不在白名单时，只停用联网搜索，本地工具继续可用。
- MCP 供应商错误会转换为稳定工具结果；内容安全错误不会按相同或更宽条件重复搜索。
- Store 初始化、语义检索或写入失败时只降级长期记忆，LangGraph 短期多轮仍可工作；
  写失败时工具不会错误确认“已保存”。

## 测试约束

普通 `uv run pytest -q` 使用 fake model、fake MCP 和 fake 数据服务，覆盖普通闲聊、多轮、
显式记忆、联网工具、工具错误、取消和语音帧顺序，外部调用为零。
真实 GLM、embedding 和 MCP 测试必须显式启用、串行运行并遵守五分钟调用预算。
