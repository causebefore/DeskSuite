# mem0 本地长期记忆系统 — 设计文档

- 日期：2026-07-10
- 状态：待评审
- 作者：liu + Claude
- 关联分支：`codex/voice-realtime-stability`
- 参考实现：`xiaozhi-esp32-server`（`core/providers/memory/`，provider 模式 + 读前查/写后异步存）

## 1. 背景与目标

当前 `VoiceService`（`app/services/voice_service.py`）每次 `chat` / `chat_stream` 都重建
`messages = [system_prompt, user_text]`——**跨轮、跨会话完全无状态**。每条 WebSocket/HTTP
语音回合彼此孤立，助手记不住用户的身份、习惯、偏好。

本设计给语音助手加一层**按 `device_id` 归属的长期事实/偏好记忆**，技术选型用**开源 `mem0`
库自托管**（`from mem0 import Memory`），记忆向量数据落本机磁盘；抽取 LLM 与 embedder
**复用现有智谱 GLM**（`glm-4-flash` + `embedding-3`，OpenAI 兼容端点，同一 `ZHIPU_API_KEY`）。

> 明确区分：**不**用 xiaozhi 的 `mem0.MemoryClient`（那是 mem0 云端 SaaS，数据出本机，违背
> "本地"要求）。我们用开源 `Memory` 类 + 本地 Chroma 向量库。

## 2. 范围

**纳入（v1）：**
- 新增 `MemoryService`，封装 mem0 的 `add`（抽取+存储）与 `search`（语义召回）。
- 在语音回合中接入：生成回复**前**召回记忆拼进 system prompt；回复完成**后**异步抽取存储。
- 按 `device_id`（ESP32 注册时上报的稳定标识）归属记忆。
- `config.toml` 新增 `[memory]` 段，默认关闭、配置开启；关闭或初始化失败 → 静默降级。

**排除（YAGNI，留待后续）：**
- 短期多轮对话上下文（"刚才那个再说详细点"）——这是另一类问题，mem0 不擅长，留待环形缓冲方案。
- 记忆管理 API（查看 / 清除 / 导出）。
- 记忆 TTL / 自动清理 / 容量上限。
- 多用户分租户（当前单设备 = 单用户，按 device_id 隔离即足够）。
- 读路径结果缓存（v1 每次实时召回，~300ms 可接受）。

## 3. 架构与组件

### 3.1 新增 `app/services/memory_service.py`（`MemoryService`）

沿用项目"service 单例 + 构造注入"模式（与 `WeatherService`/`MailService` 等一致）。

同步 API（`VoiceService` 本身即阻塞同步设计，无需 async）：

```python
class MemoryService:
    def __init__(self, settings: ServerSettings) -> None
    @property
    def enabled(self) -> bool          # settings.memory_enabled and zhipu_api_key and 非损坏
    def query_memory(self, device_id: str, query: str) -> str   # 格式化字符串；无/禁用 → ""
    def save_memory(self, device_id: str, user_text: str, assistant_text: str) -> None
```

**懒加载与降级**（关键，保证 app 在未装 mem0 / 未配置时正常启动）：
- `__init__` 不 import mem0、不建 `Memory`；仅记录 settings 与 `enabled` 初判。
- 首次 `query_memory` / `save_memory` 调用时才 `import mem0`、`Memory.from_config(...)`、缓存。
- 任意环节异常（import 失败 / from_config 失败 / key 缺失）→ 置内部 `_broken=True`，`enabled` 归
  `False`，`logger.error` 告警；后续调用全部 no-op。语音回合绝不受影响。

**mem0 配置（智谱 OpenAI 兼容端点）：**
```python
ZHIPU_OPENAI_BASE_URL = "https://open.bigmodel.cn/api/paas/v4"
config = {
    "vector_store": {"provider": "chroma", "config": {
        "collection_name": settings.memory_collection_name,
        "path": str(settings.memory_vector_store_path),
    }},
    "embedder": {"provider": "openai", "config": {
        "model": settings.memory_embedder_model,            # embedding-3
        "openai_base_url": ZHIPU_OPENAI_BASE_URL,
        "api_key": settings.zhipu_api_key,
        "embedding_dims": settings.memory_embedder_dims,     # 1024
    }},
    "llm": {"provider": "openai", "config": {
        "model": settings.memory_llm_model,                  # glm-4-flash
        "openai_base_url": ZHIPU_OPENAI_BASE_URL,
        "api_key": settings.zhipu_api_key,
        "temperature": 0,
    }},
}
m = Memory.from_config(config)
```

> 风险：mem0 各版本里 openai provider 的自定义端点键名（`openai_base_url` vs `base_url`）、
> `embedding_dims` 键名会变；自托管 `m.search` 返回的是 **list**（非云端 `{"results":[...]}`）。
> 实现时按锁定的 mem0 版本实测核对（见 §9 风险 #2、#5，由 §8 的 roundtrip 测试兜底）。

**`query_memory`：**
```python
results = m.search(query=query, filters={"user_id": device_id},
                   top_k=settings.memory_search_top_k,
                   threshold=settings.memory_search_threshold)
memories = [r.get("memory") for r in (results or []) if r.get("memory")]
return "\n".join(f"- {x}" for x in memories) if memories else ""
```

**`save_memory`：**
```python
m.add(
    [{"role": "user", "content": user_text},
     {"role": "assistant", "content": assistant_text}],
    user_id=device_id,
)
```
（与 xiaozhi 一致：只存 user/assistant，丢弃 system/tool。）

### 3.2 `VoiceService` 改动（`app/services/voice_service.py`）

- `__init__` 新增 `memory_service: MemoryService | None = None` → `self._memory`。
- `chat(...)` / `chat_stream(...)` 新增 `device_id: str | None = None`。
- `_llm(...)` / `_llm_stream(...)` 新增 `device_id`，在构造 `messages` 前**读记忆**：
  ```python
  memory_str = ""
  if self._memory and self._memory.enabled and device_id:
      try:
          memory_str = self._memory.query_memory(device_id, user_text)
      except Exception as e:
          logger.warning("记忆查询失败: {}", e)
  system_content = self._system_prompt + (
      f"\n\n# 关于用户的长期记忆（按需参考）\n{memory_str}" if memory_str else "")
  messages = [{"role": "system", "content": system_content},
              {"role": "user", "content": user_text}]
  ```
- `chat_stream` 主循环中累积 `full_reply`（每个 `REPLY_TEXT` sentence 追加）；收尾处（END 之前、
  非取消、三者齐备时）**异步保存**。提取为可测试方法：
  ```python
  def _schedule_memory_save(self, device_id, user_text, reply):
      threading.Thread(target=self._memory_save_worker,
                       args=(device_id, user_text, reply), daemon=True).start()
  def _memory_save_worker(self, device_id, user_text, reply):
      try: self._memory.save_memory(device_id, user_text, reply)
      except Exception as e: logger.warning("记忆保存失败: {}", e)
  ```
  默认守护线程（fire-and-forget，不阻塞回复）；测试用 monkeypatch 把 `_schedule_memory_save`
  改成同步执行，避免线程抖动。
  > 说明：`chat_stream` 是两条真实链路（HTTP `POST /chat` 与 WebSocket `/ws`）的共同入口，故保存
  > 逻辑只放这里即可覆盖全部生产流量；同步 `chat()` 仅保留作测试/兼容用，不重复实现保存。

### 3.3 `device_id` 穿透（接入层）

- `app/api/voice.py::voice_chat`：已有 `session: DeviceSession = Depends(get_current_device)`，
  调用处传 `device_id=session.device_id`。
- `app/api/voice_ws.py::voice_ws`：当前 `_authorized` 只判 token 存在；改为先
  `session = websocket.app.state.registry.get_session_by_token(token)`，鉴权失败照旧 4401；
  鉴权成功把 `session.device_id` 经 `_send_stream` 传进 `chat_stream(..., device_id=...)`。

### 3.4 生命周期装配（`app/main.py`）

在 `VoiceService` 构造**之前**新增：
```python
app.state.memory_service = MemoryService(server_settings)
```
并把 `memory_service=app.state.memory_service` 注入 `VoiceService(...)`。`MemoryService`
即便 `enabled=False` 也照常构造（轻量、懒加载）。

## 4. 数据流

```
设备语音
  → ASR → user_text
  → [读] MemoryService.query_memory(device_id, user_text)        # 同步 ~300ms：embed + 近邻检索
  → 拼 system_prompt + memory_str → LLM 流式（带工具调用循环）→ reply
  → TTS 流式下发
  → [写] 回复收尾 → daemon thread:
        MemoryService.save_memory(device_id, user_text, full_reply)
          └─ mem0 m.add([user,assistant], user_id=device_id)     # 抽取LLM(glm-4-flash) + embedder，异步不阻塞
```

## 5. 配置（`config.toml` + `.env`）

新增可选段（缺失等价 `enabled=false`）：
```toml
[memory]
# 默认关闭。开启前需 `uv add --optional memory mem0ai chromadb`（或 uv sync --extra memory），
# 并确保 .env 已配 ZHIPU_API_KEY（复用语音同款 key，无新增密钥）。
enabled = false
vector_store_path = "data/mem0_chroma"
collection_name = "deskmate"
llm_model = "glm-4-flash"        # 抽取事实用（便宜、快、非思考模型）
embedder_model = "embedding-3"
embedder_dims = 1024
search_top_k = 5
search_threshold = 0.3
```

`.env.example` 增注：`ZHIPU_API_KEY` 同时供语音与记忆复用（不新增密钥项）。

## 6. 依赖（`pyproject.toml`）

新增可选 extra（不污染基础安装；不装 extra 且 `enabled=false` 时 app 正常跑）：
```toml
[project.optional-dependencies]
memory = ["mem0ai>=0.1", "chromadb>=0.5"]   # 版本以实现时 uv 解析锁定为准
```

## 7. 错误处理与降级（三级防线）

| 场景 | 行为 |
|---|---|
| `[memory]` 缺失 / `enabled=false` | `MemoryService.enabled=False`；VoiceService 完全跳过读写，零开销 |
| `mem0` 未安装 / key 缺失 / `from_config` 异常 | 懒加载时捕获，`_broken=True`、`enabled=False`，启动/首调日志告警；语音不受影响 |
| 运行期 query/save 抛错 | `_llm*` 与 `_memory_save_worker` 各自 try/except 吞错 + `logger.warning`，当轮照常继续 |

## 8. 测试（`server/tests/`）

1. **`test_memory_service.py`**：monkeypatch `Memory.from_config` 返回 fake Memory；验证
   - `query_memory` 格式化 list 结果、空结果返回 `""`、`enabled=False` 时返回 `""`；
   - `save_memory` 以正确 `user_id` + messages 调 `m.add`；
   - 降级路径（key 缺失 / from_config 抛错 → `enabled=False`、调用 no-op）。
2. **`test_voice_memory.py`**：注入 stub MemoryService 进 `VoiceService`（mock 智谱网络层复用现有测试手法）；
   - 读：断言记忆字符串被拼进 `_call_chat_json`/`_call_chat_stream` 的 system message；
   - 写：monkeypatch `_schedule_memory_save` 同步执行，断言收尾后以 `(device_id, user_text, reply)` 调 `save_memory`；
   - 关闭：`memory_service=None` 时行为与现有 `test_voice_stream.py` / `test_voice_tools.py` 一致。
3. **`test_memory_roundtrip.py`**（`@pytest.mark.skipif(not ZHIPU_API_KEY)`）：真起 mem0 + 临时
   Chroma 目录，`add` 两条 → `search` 召回，端到端验证**智谱 OpenAI 兼容 + mem0 抽取确实可跑**
   （覆盖 §9 风险 #1/#2/#5）。无 key 环境自动跳过，不阻塞 CI。

## 9. 风险登记

1. **mem0 抽取依赖 function calling，智谱 GLM 兼容性**——运行期风险，由 §8-3 roundtrip 测试兜底。
2. **mem0 openai provider 配置键名随版本漂移**（`openai_base_url`/`embedding_dims` 等）——按锁定的
   mem0 版本实测，roundtrip 测试覆盖。
3. **读路径新增 ~300ms 延迟**（embed 查询）在 LLM 首 token 前——语音可接受，v1 不缓存。
4. **mem0/chromadb 依赖较重**——靠 `memory` extra 隔离，基础安装不受影响。
5. **自托管 `m.search` 返回 list**（与 xiaozhi 处理的云端 dict 形状不同）——实现时按实际返回解析。
6. **Chroma 本地路径在 Windows**——path 模式，roundtrip 测试在本机验证。

## 10. 文档同步（项目规约要求）

改服务端配置结构时同步：`config.toml`（加 `[memory]`）、`.env.example`（注明复用
`ZHIPU_API_KEY`）、`server/README.md`（记忆章节：如何开启）、`server/docs/ESP32_API.md`
（说明**语音对外 API 契约不变**，记忆为服务端内部增强，ESP32 固件无需改动）。

## 11. 实现顺序（高层，供 writing-plans 细化）

1. **先装依赖**：`uv add --optional memory mem0ai chromadb`（在 `server/` 下），确认 `uv.lock` 更新、
   `from mem0 import Memory` 可导入。**这是第 1 步，未装好不做后续。**
2. 配置：`config.toml` 加 `[memory]`；`config.py`（`ServerSettings`）加 memory 字段（`data.get("memory") or {}`）；
   `.env.example` 加注。
3. `MemoryService`：实现 + 懒加载 + 降级；单测 `test_memory_service.py`。
4. `VoiceService`：`__init__` 注入、`chat`/`chat_stream`/`_llm`/`_llm_stream` 加 `device_id`、读注入、
   异步写；单测 `test_voice_memory.py`。
5. 接入层：`voice.py` 传 `device_id`；`voice_ws.py` 取 session 后传 `device_id`。
6. `main.py` 装配 `MemoryService` 单例并注入。
7. 文档同步（§10）；roundtrip 测试（§8-3，需 key 手动跑）。
8. 全量 `uv run pytest -q`；按 CLAUDE.md 提示用户自行冒烟（开启 `[memory].enabled=true` 实聊验证）。

## 12. 验收标准

- `[memory].enabled=false`（默认）时：`uv run pytest -q` 全绿，语音行为与改动前完全一致。
- `[memory].enabled=true` 且 key 就位时：首次实聊自报信息（如"我叫 XX"）→ 重启服务 → 再问"我叫
  什么"能正确召回（roundtrip 测试 + 手动冒烟双重验证）。
- 任意降级场景（未装 extra / key 缺失 / mem0 异常）：服务正常启动，语音回合不报错、不阻塞。
