# DeskSuite Hub 工作流重构验收报告

日期：2026-08-03

范围：Hub 服务端，不含 ESP32 固件、HA、股票、日程写入或其他 MCP。

## 结论

Hub 已从“语音类集中承担 ASR、LLM、工具、记忆和 TTS”改为四个边界清楚的工作流。
文字和语音共用同一个 Assistant，短期多轮可在工作流重建后恢复，长期记忆只在用户明确要求
时写入、仅在确有需要时检索；Display 与 Dashboard 继续保持确定性。LLM 和 Speech 都通过供应商工厂装配，
当前实现为智谱，工作流不依赖供应商 HTTP 细节。

```text
POST /assistant/text ───────────────┐
                                    ├─ AssistantWorkflow
设备 PCM → VoiceWorkflow → ASR 文字 ┘    ├─ LangChain Agent
                                         ├─ 本地只读工具
                                         ├─ 可选 web-search-prime MCP
                                         ├─ SQLite 短期会话
                                         └─ SQLite 长期事实 Store
设备语音回复 ← VoiceWorkflow ← TTS ───────┘

PhotoPainter → Display 工作流 → PPF2/Manifest（不经过 LLM）
DeskMate     → Dashboard 工作流 → schema 3（不经过 LLM）
```

## 主要修改

### 1. Assistant 与文字 API

- 新增 `POST /api/v1/assistant/text`，请求只包含 `text` 和可选 `thread_id`。
- 当前是单用户部署，可信身份固定来自 `[assistant].principal_id = "owner"`，客户端不能
  伪造用户身份。
- 文字 API 必须配置非空 `DEVICE_API_TOKEN`；既有设备接口的局域网开发兼容行为不变。
- 使用 LangChain `create_agent` 代替旧手写 tool-call 循环。
- 使用 `AsyncSqliteSaver` 按 `owner:thread_id` 保存完整消息和工具结果；应用退出时显式
  关闭 Checkpointer，释放 Windows 文件句柄。
- 使用 `AsyncSqliteStore` 保存跨 thread 的显式长期事实，并与 Checkpointer 使用独立文件。
- 同一个 thread 串行执行；不同 thread 相互隔离。
- 使用 LangChain 中间件统一处理工具异常、联网搜索单回合调用上限和长会话摘要。
- 使用稳定的 LangGraph v2 `messages + updates` 流，同时输出文本增量和工具生命周期事件；
  本机版本的 v3 事件 API 仍标记为 beta，本轮不将语音主链路绑定到实验接口。

### 2. 工具、MCP 与记忆

- 本地工具：`get_weather`、`get_calendar`、`get_mail`、`get_quota`。
- `remember_user_fact` 是唯一长期记忆写入口；普通对话、天气、邮件、日程、额度和搜索
  结果不会自动写入 Store。工具通过 LangChain `ToolRuntime` 直接执行 `put`。
- `search_user_memory` 是显式长期记忆读入口。只有 Agent 判断问题涉及过去保存的事实或
  偏好、且当前消息历史不足以回答时才执行 `search`；普通问候、复述和实时查询不会请求
  embedding。语义索引异常时回退为最近事实检索。
- 长期事实按服务端 `principal_id` 命名空间隔离，使用智谱 `embedding-3` 语义检索；
  相同事实使用稳定 key，避免重复记录。
- 可选智谱联网搜索运行时复用现有 `ZHIPU_API_KEY`，只允许官方
  `webSearchPrime` 及当前 LangChain 适配后的等价名称 `web_search_prime`。
- Hub 预热时用 `langchain-mcp-adapters` 发现和白名单过滤工具；普通回合不会连接 MCP。
  `web-search-prime` 无跨调用状态，因此沿用适配器默认模式，在真正执行工具时创建并清理
  独立 session，避免长期空闲后复用过期 session。
- MCP interceptor 记录实际远程调用边界并归一化供应商错误；内容安全拒绝不会重复放宽
  查询。MCP 握手或加载失败只关闭联网搜索，本地工具和普通对话继续工作。

### 3. Voice 与供应商适配

- Voice 只负责 `PCM → ASR → Assistant → TTS → 设备帧`。
- HTTP/WebSocket 路径、START/END_INPUT/CANCEL、二进制帧头、THINKING 心跳、
  24kHz 单声道 16-bit TTS PCM 和 ERROR/END 收口保持兼容。
- Assistant 的 `tool_started` 会立即投影为既有 THINKING 帧，工具完成或失败写入带
  `device_id`、`thread_id`、工具名和调用 ID 的日志，不把工具状态送进 TTS。
- 外部 CANCEL 会传播到 Assistant 与 TTS；取消后的回合不再发送 TTS 或 END。
- 旧设备不传 `X-Thread-Id` 时映射为 `voice:<device_id>`；传入后可与文字入口共享 thread。
- 智谱 multipart ASR、SSE TTS 和错误清洗移动到 `app/providers/zhipu_speech.py`。
- `[providers].llm`、`[providers].embedding` 与 `[providers].speech` 当前均为 `zhipu`；新增供应商只扩展工厂和
  Provider 实现，不修改 Assistant、Voice 或设备协议。

### 4. 确定性产品工作流

- 原 Dashboard 编排移动到 `app/workflows/dashboard/`。
- 原 Display 取数、页面注册、刷新和渲染移动到 `app/workflows/display/`。
- 旧实现文件已删除，没有保留空壳转发层。
- 四个工作流目录均有中文 README，说明入口、数据流、状态、降级和测试。

## 配置与依赖

新增主要依赖：

- `langchain`
- `langchain-openai`
- `langchain-mcp-adapters`
- `langgraph-checkpoint-sqlite`
- `mcp<2`

`config.toml` 新增 `[assistant]`、`[assistant.memory]`、`[mcp.web_search_prime]` 及 AI provider 选择。
MCP URL 可以进入普通配置，Authorization 只在运行时从 `.env` 构造。本次没有读取、
输出或改写真实密钥，也没有修改 `.env`。

选择这些包的依据是官方能力边界：LangChain `create_agent` 提供图式 Agent 与工具循环，
LangGraph Checkpointer 按 thread 持久化状态，`langchain-mcp-adapters` 直接把 MCP 工具转换
成 LangChain 工具，LangGraph Store 则保存跨 thread 的用户事实。参考：

- [LangChain Agents](https://docs.langchain.com/oss/python/langchain/agents)
- [LangGraph Persistence](https://docs.langchain.com/oss/python/langgraph/persistence)
- [LangGraph Memory](https://docs.langchain.com/oss/python/langgraph/add-memory)
- [LangChain MCP](https://docs.langchain.com/oss/python/langchain/mcp)
- [LangChain Streaming](https://docs.langchain.com/oss/python/langchain/streaming)
- [智谱联网搜索 MCP](https://docs.bigmodel.cn/cn/coding-plan/mcp/search-mcp-server)

语音状态和取消语义参考了小智官方实现：它将 listening、speaking、idle、abort、STT、TTS
和 MCP JSON-RPC 作为显式事件处理。本轮只借鉴“状态可观察、工具调用有 ID、取消可传播”
三个边界，没有改变 DeskMate 既有二进制协议。未来 HA、日程写入等高权限工具应像小智的
user-only tool 一样与普通模型可调用的只读工具分层。

- [小智 WebSocket 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md)
- [小智 MCP 工具使用](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md)

## 验收结果

| 验收项 | 结果 | 说明 |
|---|---|---|
| 普通全量测试 | 255 passed，2 skipped | 2 项均为默认跳过的真实 GLM/MCP 测试；即使 `.env` 有 Key 也不联网 |
| 文字 API | 通过 | 鉴权、自动/显式 thread、错误映射和 4000 字符边界 |
| 本地 Agent 工具循环 | 通过 | fake model 实际发出 tool-call，工具结果进入当前和下一回合 |
| 同 thread 多轮 | 通过 | 本地 fake 与真实 GLM 三回合均验证，并覆盖工作流重建恢复 |
| thread 隔离 | 通过 | 不同 thread 不共享短期消息 |
| Hub 重建恢复 | 通过 | 同一 SQLite 文件重建工作流后恢复历史，并能释放文件句柄 |
| Hub 启停 | 通过 | lifespan 预热/降级/关闭契约通过，禁用外部调用的 OpenAPI 启动冒烟返回 200 |
| 长期记忆 | 通过 | 本地 LangGraph SQLite Store 完成按需 search、显式 put、跨 thread、身份隔离和降级 |
| Voice 协议 | 通过 | ASR/TTS、工具状态、心跳、工具中取消、错误、帧顺序、偶数字节与分片边界 |
| web-search-prime | 通过 | 无状态 session、白名单和错误归一化契约通过；真实 MCP 执行一次只读搜索 |
| 邮箱真实调用 | 0 次 | 所有邮箱相关测试均使用 fake，未登录 IMAP |

普通全量测试没有外部调用。随后分两批显式运行受控 live 验收：三次 GLM 短期多轮和
两次 MCP 只读搜索（第二次用于确认最终无状态 session 实现），结果分别为 `2 passed` 与
`1 passed`。该组关闭长期记忆，因此没有 embedding 请求，也没有邮箱调用。普通测试的
live 标记仍需分别显式传入 `--run-live-glm`、`--run-live-mcp` 或 `--run-live-mail`。

## 当前边界

- 文字 API 当前返回完整 JSON，不做 SSE 流式输出；这是本阶段的最小入口。
- SQLite Checkpoint 适合当前单用户、单 Hub 进程；未来横向多进程时应换共享 Checkpointer。
- SQLite Store 同样以单进程为当前边界；LangGraph 不会自动合并互相矛盾的显式事实，
  后续增加记忆修改/删除能力时需定义冲突策略。
- 当前只实现查询型日历和邮件工具；没有添加、删除日程或控制 HA。
- `web-search-prime` 是否启用由配置控制；真实调用仍需确认当前 Key 具备对应 MCP 权限。
- 本次未编译或刷写任何固件，设备实机语音体验仍需单独验收。
