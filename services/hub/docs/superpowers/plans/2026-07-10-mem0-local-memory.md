# mem0 本地长期记忆系统 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给无状态语音助手接入按 `device_id` 归属的长期事实/偏好记忆（自托管 mem0 + 本地 Chroma + 复用智谱 GLM）。

**Architecture:** 新增 `MemoryService` 封装 mem0 的 `search`（读前召回）与 `add`（写后异步抽取）；`VoiceService` 在 `_llm`/`_llm_stream` 里把召回的记忆拼进 system prompt，在 `chat_stream` 收尾用守护线程异步保存；记忆按 `device_id` 隔离，默认关闭、配置开启，失败静默降级。

**Tech Stack:** Python 3.12 / FastAPI / uv / mem0（自托管 `Memory` 类）/ Chroma（本地磁盘）/ 智谱 GLM（`glm-4-flash` + `embedding-3`，OpenAI 兼容端点）/ pytest。

## Global Constraints

- **Shell 只用 PowerShell（pwsh）。** 路径用 Windows 原生风格。`uv run pytest` 跑测试。commit 用单行 `-m "type(scope): 中文描述"`（PowerShell 下单行 commit 无需 here-string）。
- **依赖只走 `memory` 可选 extra**：`mem0ai` + `chromadb`，基础安装不引入。`MemoryService` 内部 `import mem0` 延迟到启用时，未装 extra 且 `enabled=false` 时 app 正常启动。
- **默认关闭、静默降级**：`[memory].enabled=false` 为默认；任一环节失败（未装/key 缺失/初始化异常/运行期异常）→ 语音回合不报错、不阻塞。
- **向后兼容**：`VoiceService` 的新增参数（`device_id`、`memory_service`）一律带默认值 `None`；现有 `tests/` 在不注入 memory 时行为不变。
- **不主动启动服务/不烧录**：改完只跑 `uv run pytest -q`；冒烟实聊由用户自行开启 `[memory].enabled=true` 后验证。
- **提交格式**：`type(scope): 中文描述`，两个子仓库各自提交（本计划全部在 `server/`）。

---

## File Structure

| 文件 | 责任 | 动作 |
|---|---|---|
| `pyproject.toml` | 声明 `memory` 可选 extra | 改 |
| `config.toml` | 新增 `[memory]` 配置段（默认关闭） | 改 |
| `.env.example` | 注明 `ZHIPU_API_KEY` 复用 | 改 |
| `app/core/config.py` | `ServerSettings` 读 `[memory]` 为 memory_* 字段 | 改 |
| `app/services/memory_service.py` | `MemoryService`：懒加载 mem0 + `query_memory`/`save_memory` + 降级 | 新建 |
| `app/services/voice_service.py` | `__init__` 注入 memory；`chat`/`chat_stream`/`_llm`/`_llm_stream` 加 `device_id`；读注入 + 异步写 | 改 |
| `app/api/voice.py` | HTTP `/chat` 把 `session.device_id` 传给 `chat_stream` | 改 |
| `app/api/voice_ws.py` | WS 解析 token→session→`device_id` 透传给 `chat_stream` | 改 |
| `app/main.py` | 构造 `MemoryService` 单例并注入 `VoiceService` | 改 |
| `server/README.md` | 记忆章节（如何开启） | 改 |
| `docs/ESP32_API.md` | 注明语音对外契约不变，记忆为服务端内部 | 改 |
| `tests/test_runtime_config.py` | `[memory]` 解析测试 | 改 |
| `tests/test_memory_service.py` | `MemoryService` 单测（mock mem0） | 新建 |
| `tests/test_voice_memory.py` | 读注入 + 异步写单测 | 新建 |
| `tests/test_voice_api.py` / `tests/test_voice_websocket.py` | 更新 `chat_stream` 签名断言 + device_id 透传断言 | 改 |
| `tests/test_memory_roundtrip.py` | 真 mem0+智谱 端到端（无 key 跳过） | 新建 |

---

## Task 1: 新增 `memory` 可选依赖并安装

**Files:**
- Modify: `pyproject.toml`、`uv.lock`（由 `uv add` 自动维护）

**Interfaces:**
- Produces: `memory` extra 含 `mem0ai`、`chromadb`；`uv run python -c "from mem0 import Memory"` 可用。

- [ ] **Step 1: 用 uv 把两个包加进 memory extra（自动改 pyproject.toml + uv.lock 并安装）**

在 `server/` 目录运行：
```powershell
uv add --optional memory mem0ai chromadb
```
预期：`pyproject.toml` 出现 `[project.optional-dependencies].memory = [...]`，`uv.lock` 更新，依赖装好。

- [ ] **Step 2: 记录 mem0 实际版本（后续配置键名核对用）**

```powershell
uv run python -c "import mem0, chromadb; print('mem0', getattr(mem0,'__version__','?')); print('chroma', chromadb.__version__)"
```
把打印的 mem0 版本记在脑里/注释里——Task 3 的 openai provider 配置键（`openai_base_url`、`embedding_dims`）若在该版本叫法不同，需据此调整。

- [ ] **Step 3: 验证导入**

```powershell
uv run python -c "from mem0 import Memory; print('ok')"
```
预期输出 `ok`。若失败：不要往下做，先把依赖问题报给用户。

- [ ] **Step 4: 提交**

```powershell
git add pyproject.toml uv.lock
git commit -m "chore(memory): 新增 mem0/chromadb 可选依赖"
```

---

## Task 2: `[memory]` 配置段 + `ServerSettings` 字段

**Files:**
- Modify: `config.toml`
- Modify: `app/core/config.py`（`ServerSettings.__init__`）
- Modify: `.env.example`
- Test: `tests/test_runtime_config.py`

**Interfaces:**
- Produces: `ServerSettings` 新增字段——`memory_enabled: bool`、`memory_vector_store_path: Path`、`memory_collection_name: str`、`memory_llm_model: str`、`memory_embedder_model: str`、`memory_embedder_dims: int`、`memory_search_top_k: int`、`memory_search_threshold: float`。缺失 `[memory]` 段时 `memory_enabled=False` 且其余字段取默认值。

- [ ] **Step 1: 写失败测试（更新 `tests/test_runtime_config.py`）**

把 `_write_config` 加一个可选参数控制是否写 `[memory]` 段（默认写、默认 enabled=false），并在文件末尾追加两个测试。即把：

```python
def _write_config(root: Path) -> Path:
    config_path = root / "config.toml"
    config_path.write_text(
        """
[app]
...
[imap.cache]
inbox_seconds = 180
""".strip(),
        encoding="utf-8",
    )
    return config_path
```

改为（仅改签名 + 末尾拼接 `[memory]`，正文不变）：

```python
def _write_config(root: Path, *, include_memory: bool = True,
                  memory_enabled: bool = False) -> Path:
    config_path = root / "config.toml"
    base = """
[app]
title = "DeskMate ESP32 Server"
version = "9.9.9"
description = "test config"

[server]
host = "0.0.0.0"
port = 4321
log_level = "debug"

[providers]
weather = "qweather"
stock = "mock"
calendar = "mock"
mail = "mock"
llm = "local"

[zhipu]
asr_model = "glm-asr-2512"
llm_model = "glm-4-flash"
tts_model = "glm-tts"
tts_voice = "female"

[qweather]
host = "api.example.test"
timeout_seconds = 2.5
city_lookup_path = "/geo/v2/city/lookup"
now_path = "/v7/weather/now"
daily_path = "/v7/weather/{days}"
daily_days = "3d"
minutely_path = "/v7/minutely/5m"
alert_path = "/weatheralert/v1/current/{latitude}/{longitude}"
air_path = "/airquality/v1/current/{latitude}/{longitude}"

[weather.cache]
location_seconds = 11
now_seconds = 12
daily_seconds = 13
minutely_seconds = 14
alert_seconds = 15
air_seconds = 15

[device.defaults]
city = "上海"
timezone = "Asia/Shanghai"
stock_symbols = ["BABA", "TSLA"]
refresh_seconds = 600
llm_provider = "local"

[storage]
runtime_log_dir = "logs"
log_keep_sessions = 7
firmware_dir = "bins"
ota_manifest = "bins/test-manifest.json"
firmware_mount_path = "/downloads"
device_db = "data/test-devices.db"

[caldav]
url = "https://caldav.icloud.com/"
timeout_seconds = 8
range_days = 7
max_events = 10

[imap]
host = "imap.qq.com"
port = 993
use_ssl = true
timeout_seconds = 8
max_messages = 5

[caldav.cache]
events_seconds = 600

[imap.cache]
inbox_seconds = 180
""".strip()
    if include_memory:
        base += f"""

[memory]
enabled = {"true" if memory_enabled else "false"}
vector_store_path = "data/mem0_chroma"
collection_name = "deskmate"
llm_model = "glm-4-flash"
embedder_model = "embedding-3"
embedder_dims = 1024
search_top_k = 5
search_threshold = 0.3
"""
    config_path.write_text(base, encoding="utf-8")
    return config_path
```

在文件末尾追加：

```python
def test_server_settings_reads_memory_config_when_enabled(tmp_path, monkeypatch):
    config_path = _write_config(tmp_path, memory_enabled=True)
    env_path = tmp_path / ".env"
    env_path.write_text("ZHIPU_API_KEY=zh-key\n", encoding="utf-8")
    monkeypatch.delenv("ZHIPU_API_KEY", raising=False)

    settings = ServerSettings(
        config_path=config_path, env_path=env_path, project_root=tmp_path
    )

    assert settings.memory_enabled is True
    assert settings.memory_vector_store_path == tmp_path / "data" / "mem0_chroma"
    assert settings.memory_collection_name == "deskmate"
    assert settings.memory_llm_model == "glm-4-flash"
    assert settings.memory_embedder_model == "embedding-3"
    assert settings.memory_embedder_dims == 1024
    assert settings.memory_search_top_k == 5
    assert settings.memory_search_threshold == 0.3


def test_memory_disabled_with_defaults_when_section_absent(tmp_path, monkeypatch):
    config_path = _write_config(tmp_path, include_memory=False)
    env_path = tmp_path / ".env"
    env_path.write_text("\n", encoding="utf-8")
    monkeypatch.delenv("ZHIPU_API_KEY", raising=False)

    settings = ServerSettings(
        config_path=config_path, env_path=env_path, project_root=tmp_path
    )

    assert settings.memory_enabled is False
    assert settings.memory_llm_model == "glm-4-flash"
    assert settings.memory_embedder_dims == 1024
    assert settings.memory_vector_store_path == tmp_path / "data" / "mem0_chroma"
```

- [ ] **Step 2: 跑测试确认失败**

```powershell
uv run pytest tests/test_runtime_config.py::test_server_settings_reads_memory_config_when_enabled -v
```
预期：FAIL（`AttributeError: 'ServerSettings' object has no attribute 'memory_enabled'`）。

- [ ] **Step 3: 实现 `ServerSettings` memory 字段（`app/core/config.py`）**

在 `ServerSettings.__init__` 末尾（`self.ota_firmware_base_version = ...` 之后）追加：

```python
        # ── 记忆模块（自托管 mem0，默认关闭）──────────────────
        # 缺失 [memory] 段等价 enabled=false，各字段走默认值。
        memory = data.get("memory") or {}
        self.memory_enabled = bool(memory.get("enabled", False))
        self.memory_vector_store_path = _path_from_root(
            self.project_root,
            str(memory.get("vector_store_path", "data/mem0_chroma")),
        )
        self.memory_collection_name = str(memory.get("collection_name", "deskmate"))
        self.memory_llm_model = str(memory.get("llm_model", "glm-4-flash"))
        self.memory_embedder_model = str(memory.get("embedder_model", "embedding-3"))
        self.memory_embedder_dims = int(memory.get("embedder_dims", 1024))
        self.memory_search_top_k = int(memory.get("search_top_k", 5))
        self.memory_search_threshold = float(memory.get("search_threshold", 0.3))
```

- [ ] **Step 4: 在 `config.toml` 末尾加默认 `[memory]` 段**

```toml

[memory]
# 长期记忆（自托管 mem0）：默认关闭。开启前需 `uv add --optional memory mem0ai chromadb`
# （或 uv sync --extra memory），并确保 .env 已配 ZHIPU_API_KEY（与语音复用同一 key）。
enabled = false
vector_store_path = "data/mem0_chroma"
collection_name = "deskmate"
llm_model = "glm-4-flash"        # 抽取事实用（便宜、快、非思考模型）
embedder_model = "embedding-3"
embedder_dims = 1024
search_top_k = 5
search_threshold = 0.3
```

- [ ] **Step 5: 在 `.env.example` 注明 ZHIPU_API_KEY 复用**

打开 `.env.example`，在 `ZHIPU_API_KEY` 那一行（若不存在则新增）的注释里补一句：`# 同时供语音（ASR/LLM/TTS）与长期记忆（mem0 抽取/embedding）复用`。例如：

```dotenv
# 智谱开放平台 API Key。同时供语音（ASR/LLM/TTS）与长期记忆（mem0 抽取/embedding）复用。
ZHIPU_API_KEY=
```

- [ ] **Step 6: 跑测试确认通过**

```powershell
uv run pytest tests/test_runtime_config.py -v
```
预期：全 PASS（含两个新测试 + 既有 3 个）。

- [ ] **Step 7: 提交**

```powershell
git add config.toml .env.example app/core/config.py tests/test_runtime_config.py
git commit -m "feat(memory): 新增 [memory] 配置段与 ServerSettings 字段"
```

---

## Task 3: `MemoryService`（懒加载 + 降级 + query/save）

**Files:**
- Create: `app/services/memory_service.py`
- Test: `tests/test_memory_service.py`

**Interfaces:**
- Consumes: `ServerSettings` 的 `memory_*` 字段 + `zhipu_api_key`（来自 Task 2）。
- Produces: `MemoryService(settings)`，属性 `enabled: bool`，方法 `query_memory(device_id: str, query: str) -> str`、`save_memory(device_id: str, user_text: str, assistant_text: str) -> None`。**模块顶层不 import mem0**（禁用时不影响 app 启动）；mem0 在首次使用时经 `import mem0; mem0.Memory.from_config(...)` 懒加载。

- [ ] **Step 1: 写失败测试（`tests/test_memory_service.py`）**

```python
"""MemoryService 单测：mock mem0.Memory，验证召回格式、保存入参、降级路径。"""

from unittest.mock import MagicMock, patch

from app.services.memory_service import MemoryService


def _settings(enabled=True, key="zh-key"):
    s = MagicMock()
    s.memory_enabled = enabled
    s.zhipu_api_key = key
    s.memory_vector_store_path = "/tmp/mem0_chroma"
    s.memory_collection_name = "deskmate"
    s.memory_llm_model = "glm-4-flash"
    s.memory_embedder_model = "embedding-3"
    s.memory_embedder_dims = 1024
    s.memory_search_top_k = 5
    s.memory_search_threshold = 0.3
    return s


class TestEnabled:
    def test_disabled_when_config_off(self):
        assert MemoryService(_settings(enabled=False)).enabled is False

    def test_disabled_when_no_key(self):
        assert MemoryService(_settings(key="")).enabled is False

    def test_enabled_after_lazy_init(self):
        with patch("mem0.Memory") as M:
            M.from_config.return_value = MagicMock()
            svc = MemoryService(_settings())
            svc._ensure_client()  # 触发懒加载
            assert svc.enabled is True

    def test_broken_when_from_config_raises(self):
        with patch("mem0.Memory") as M:
            M.from_config.side_effect = RuntimeError("boom")
            svc = MemoryService(_settings())
            svc._ensure_client()
            assert svc.enabled is False  # 初始化失败 → 禁用


def _svc_with_fake_client():
    """返回 (svc, fake_client)，fake_client 已注入。"""
    fake = MagicMock()
    fake.search.return_value = [{"memory": "用户叫张三"}, {"memory": "喜欢科幻"}]
    with patch("mem0.Memory") as M:
        M.from_config.return_value = fake
        svc = MemoryService(_settings())
        svc._ensure_client()
    return svc, fake


class TestQuery:
    def test_formats_memories_as_bullets(self):
        svc, _ = _svc_with_fake_client()
        out = svc.query_memory("dev1", "我是谁")
        assert "- 用户叫张三" in out
        assert "- 喜欢科幻" in out

    def test_empty_when_no_results(self):
        svc, fake = _svc_with_fake_client()
        fake.search.return_value = []
        assert svc.query_memory("dev1", "x") == ""

    def test_empty_when_disabled(self):
        svc = MemoryService(_settings(enabled=False))
        assert svc.query_memory("dev1", "x") == ""

    def test_filters_by_device_id(self):
        svc, fake = _svc_with_fake_client()
        svc.query_memory("devX", "q")
        assert fake.search.call_args.kwargs["filters"] == {"user_id": "devX"}

    def test_swallows_runtime_error(self):
        svc, fake = _svc_with_fake_client()
        fake.search.side_effect = RuntimeError("net")
        assert svc.query_memory("dev1", "x") == ""  # 不抛


class TestSave:
    def test_calls_add_with_user_id_and_messages(self):
        svc, fake = _svc_with_fake_client()
        svc.save_memory("dev1", "我叫张三", "你好张三")
        args, kwargs = fake.add.call_args
        assert kwargs["user_id"] == "dev1"
        assert args[0] == [
            {"role": "user", "content": "我叫张三"},
            {"role": "assistant", "content": "你好张三"},
        ]

    def test_noop_when_disabled(self):
        svc = MemoryService(_settings(enabled=False))
        svc.save_memory("dev1", "x", "y")  # 不应抛、不应触达 mem0

    def test_swallows_runtime_error(self):
        svc, fake = _svc_with_fake_client()
        fake.add.side_effect = RuntimeError("net")
        svc.save_memory("dev1", "x", "y")  # 不抛
```

- [ ] **Step 2: 跑测试确认失败**

```powershell
uv run pytest tests/test_memory_service.py -v
```
预期：FAIL（模块 `app.services.memory_service` 不存在）。

- [ ] **Step 3: 实现 `app/services/memory_service.py`**

```python
"""语音助手的长期记忆服务：封装自托管 mem0 的事实抽取与语义召回。

设计要点：
- 模块顶层不 import mem0，禁用或未装 extra 时不影响 app 启动。
- mem0 的 Memory 客户端懒加载（首次 query/save 时），初始化失败置 _broken=True。
- 所有运行期异常都被吞掉 + 日志告警，绝不影响语音回合。
- 记忆按 device_id（ESP32 注册时上报）隔离。
"""

from loguru import logger

# 智谱 BigModel 的 OpenAI 兼容端点；mem0 的 openai provider 用它复用现有 key。
_ZHIPU_OPENAI_BASE_URL = "https://open.bigmodel.cn/api/paas/v4"


class MemoryService:
    def __init__(self, settings) -> None:
        self._settings = settings
        self._client = None        # mem0.Memory 实例，懒加载
        self._broken = False       # 初始化失败标记
        self._config_enabled = bool(getattr(settings, "memory_enabled", False))
        self._api_key = getattr(settings, "zhipu_api_key", "") or ""

    @property
    def enabled(self) -> bool:
        return self._config_enabled and bool(self._api_key) and not self._broken

    def _build_config(self) -> dict:
        s = self._settings
        return {
            "vector_store": {"provider": "chroma", "config": {
                "collection_name": s.memory_collection_name,
                "path": str(s.memory_vector_store_path),
            }},
            "embedder": {"provider": "openai", "config": {
                "model": s.memory_embedder_model,
                "openai_base_url": _ZHIPU_OPENAI_BASE_URL,
                "api_key": self._api_key,
                "embedding_dims": s.memory_embedder_dims,
            }},
            "llm": {"provider": "openai", "config": {
                "model": s.memory_llm_model,
                "openai_base_url": _ZHIPU_OPENAI_BASE_URL,
                "api_key": self._api_key,
                "temperature": 0,
            }},
        }

    def _ensure_client(self):
        """懒加载 mem0.Memory；失败置 _broken。返回 client 或 None。"""
        if self._client is not None or self._broken:
            return self._client
        if not self._config_enabled or not self._api_key:
            return None
        try:
            import mem0
            self._client = mem0.Memory.from_config(self._build_config())
            logger.info("mem0 记忆服务已启用")
        except Exception as e:  # noqa: BLE001 — 降级，不能炸语音
            self._broken = True
            logger.error("mem0 初始化失败，记忆功能禁用: {}", e)
        return self._client

    def query_memory(self, device_id: str, query: str) -> str:
        """按 device_id 语义召回相关记忆，返回带 "- " 前缀的多行字符串；无则 ""。"""
        client = self._ensure_client()
        if client is None or not device_id or not query:
            return ""
        try:
            results = client.search(
                query=query,
                filters={"user_id": device_id},
                top_k=self._settings.memory_search_top_k,
                threshold=self._settings.memory_search_threshold,
            )
        except Exception as e:  # noqa: BLE001
            logger.warning("记忆查询失败: {}", e)
            return ""
        memories = [
            r.get("memory") for r in (results or [])
            if isinstance(r, dict) and r.get("memory")
        ]
        return "\n".join(f"- {m}" for m in memories) if memories else ""

    def save_memory(self, device_id: str, user_text: str, assistant_text: str) -> None:
        """把一轮 user/assistant 存入 mem0，由其抽取事实；失败静默。"""
        client = self._ensure_client()
        if client is None or not device_id or not user_text:
            return
        try:
            client.add(
                [
                    {"role": "user", "content": user_text},
                    {"role": "assistant", "content": assistant_text},
                ],
                user_id=device_id,
            )
            logger.debug("记忆已保存: device={}", device_id)
        except Exception as e:  # noqa: BLE001
            logger.warning("记忆保存失败: {}", e)
```

> 注：若 Step 2 记录的 mem0 版本里 openai provider 的自定义端点键名不是 `openai_base_url`、或 embedder 维度键名不是 `embedding_dims`，按该版本调整 `_build_config`。Task 7 的 roundtrip 测试会兜底验证。

- [ ] **Step 4: 跑测试确认通过**

```powershell
uv run pytest tests/test_memory_service.py -v
```
预期：全 PASS。

- [ ] **Step 5: 提交**

```powershell
git add app/services/memory_service.py tests/test_memory_service.py
git commit -m "feat(memory): 新增 MemoryService 封装自托管 mem0 召回与抽取"
```

---

## Task 4: `VoiceService` 接入记忆（读注入 + 写后异步）

**Files:**
- Modify: `app/services/voice_service.py`
- Test: `tests/test_voice_memory.py`（新建）

**Interfaces:**
- Consumes: `MemoryService`（Task 3）—— 用其 `enabled`、`query_memory`、`save_memory`。
- Produces: `VoiceService.__init__(..., memory_service=None)`；`chat(pcm_24k, device_id=None)`、`chat_stream(pcm_data, sample_rate=24000, cancel_event=None, device_id=None)`；内部 `_llm/_llm_stream(user_text, device_id=None)`；新增 `_schedule_memory_save(device_id, user_text, reply)`、`_memory_save_worker(...)`。`device_id=None` 时跳过记忆（向后兼容）。

- [ ] **Step 1: 写失败测试（`tests/test_voice_memory.py`）**

```python
"""VoiceService 记忆接入：读注入 system prompt、写后异步保存、无 memory 时行为不变。"""

from unittest.mock import MagicMock, patch

from app.services.voice_service import VoiceService


def _delta(content=None):
    d = {}
    if content is not None:
        d["content"] = content
    return {"choices": [{"delta": d}]}


def _make_service(memory=None, default_city="苏州"):
    settings = MagicMock()
    settings.zhipu_api_key = "test-key"
    settings.zhipu_llm_model = "glm-4-flash"
    settings.zhipu_asr_model = "glm-asr-2512"
    settings.zhipu_tts_model = "glm-tts"
    settings.zhipu_tts_voice = "female"
    settings.default_city = default_city
    settings.default_timezone = "Asia/Shanghai"
    return VoiceService(settings, memory_service=memory)


class TestReadInjection:
    def test_memory_string_injected_into_system_message(self):
        mem = MagicMock()
        mem.enabled = True
        mem.query_memory.return_value = "用户叫张三"
        svc = _make_service(memory=mem)

        captured = {}

        def fake_stream(messages, tools):
            captured["messages"] = messages
            return iter([_delta(content="你好张三。")])

        with patch.object(svc, "_call_chat_stream", side_effect=fake_stream):
            list(svc._llm_stream("你好", device_id="dev1"))

        assert captured["messages"][0]["role"] == "system"
        assert "用户叫张三" in captured["messages"][0]["content"]
        mem.query_memory.assert_called_once_with("dev1", "你好")

    def test_no_memory_keeps_plain_system_prompt(self):
        svc = _make_service(memory=None)
        captured = {}

        def fake_stream(messages, tools):
            captured["messages"] = messages
            return iter([_delta(content="你好。")])

        with patch.object(svc, "_call_chat_stream", side_effect=fake_stream):
            list(svc._llm_stream("你好", device_id="dev1"))

        assert captured["messages"][0]["content"] == svc._system_prompt


class TestDeferredSave:
    def test_chat_stream_schedules_save_with_device_user_reply(self):
        mem = MagicMock()
        mem.enabled = True
        mem.query_memory.return_value = ""
        svc = _make_service(memory=mem)

        saved = []

        def fake_schedule(dev, u, r):
            saved.append((dev, u, r))

        with patch.object(svc, "_schedule_memory_save", side_effect=fake_schedule), \
             patch.object(svc, "_asr", return_value="我叫张三"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好张三。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01"])):
            list(svc.chat_stream(b"\x00" * 100, device_id="dev1"))

        assert saved, "应触发一次异步保存"
        assert saved[0][0] == "dev1"
        assert saved[0][1] == "我叫张三"
        assert "你好张三" in saved[0][2]

    def test_no_memory_does_not_schedule_save(self):
        svc = _make_service(memory=None)
        with patch.object(svc, "_schedule_memory_save") as sched, \
             patch.object(svc, "_asr", return_value="嗨"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01"])):
            list(svc.chat_stream(b"\x00" * 100, device_id="dev1"))
        sched.assert_not_called()

    def test_disabled_memory_does_not_schedule_save(self):
        mem = MagicMock()
        mem.enabled = False  # 即使有 memory_service，关闭也不保存
        svc = _make_service(memory=mem)
        with patch.object(svc, "_schedule_memory_save") as sched, \
             patch.object(svc, "_asr", return_value="嗨"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01"])):
            list(svc.chat_stream(b"\x00" * 100, device_id="dev1"))
        sched.assert_not_called()
```

- [ ] **Step 2: 跑测试确认失败**

```powershell
uv run pytest tests/test_voice_memory.py -v
```
预期：FAIL（`VoiceService.__init__()` 不接受 `memory_service`，`_schedule_memory_save` 不存在等）。

- [ ] **Step 3: 改 `VoiceService.__init__` 接受 memory_service**

在 `app/services/voice_service.py` 的 `VoiceService.__init__` 签名末尾加参数，并赋值。把：

```python
    def __init__(
        self,
        settings: ServerSettings,
        weather_service=None,
        calendar_service=None,
        mail_service=None,
        quota_service=None,
    ) -> None:
```

改为：

```python
    def __init__(
        self,
        settings: ServerSettings,
        weather_service=None,
        calendar_service=None,
        mail_service=None,
        quota_service=None,
        memory_service=None,
    ) -> None:
```

并在 `__init__` 体内 `self._tools = self._build_tools()` 之后追加一行：

```python
        # 长期记忆服务（可选，None 或 enabled=False 时完全跳过）
        self._memory = memory_service
```

- [ ] **Step 4: 加 `_recall_memory` 辅助方法 + 给 `_llm`/`_llm_stream` 加 `device_id`**

在 `VoiceService` 内（`_build_tools` 之后、`chat` 之前）新增：

```python
    def _recall_memory(self, device_id: str | None, user_text: str) -> str:
        """读路径：按 device_id 召回相关记忆；失败/禁用返回 ''。"""
        if not (self._memory and self._memory.enabled and device_id and user_text):
            return ""
        try:
            return self._memory.query_memory(device_id, user_text)
        except Exception as e:  # noqa: BLE001
            logger.warning("记忆查询失败: {}", e)
            return ""
```

把 `_llm` 的签名与开头从：

```python
    def _llm(self, user_text: str) -> str:
        ...
        messages = [
            {"role": "system", "content": self._system_prompt},
            {"role": "user", "content": user_text},
        ]
```

改为：

```python
    def _llm(self, user_text: str, device_id: str | None = None) -> str:
        ...
        memory_str = self._recall_memory(device_id, user_text)
        system_content = self._system_prompt + (
            f"\n\n# 关于用户的长期记忆（按需参考）\n{memory_str}" if memory_str else "")
        messages = [
            {"role": "system", "content": system_content},
            {"role": "user", "content": user_text},
        ]
```

对 `_llm_stream` 同理，把签名与 messages 构造从：

```python
    def _llm_stream(self, user_text: str) -> Iterator[str | _ThinkingPing]:
        ...
        messages = [
            {"role": "system", "content": self._system_prompt},
            {"role": "user", "content": user_text},
        ]
```

改为：

```python
    def _llm_stream(self, user_text: str, device_id: str | None = None) -> Iterator[str | _ThinkingPing]:
        ...
        memory_str = self._recall_memory(device_id, user_text)
        system_content = self._system_prompt + (
            f"\n\n# 关于用户的长期记忆（按需参考）\n{memory_str}" if memory_str else "")
        messages = [
            {"role": "system", "content": system_content},
            {"role": "user", "content": user_text},
        ]
```

（`_llm` / `_llm_stream` 内 `messages = [...]` 之后的工具调用循环逻辑保持不变。）

- [ ] **Step 5: 给 `chat` / `chat_stream` 加 `device_id` 并在 `chat_stream` 收尾异步保存**

把同步入口 `chat` 的签名与 LLM 调用从：

```python
    def chat(self, pcm_24k: bytes) -> VoiceReply:
        ...
        # 3. LLM：生成回复
        reply_text = self._llm(user_text)
```

改为：

```python
    def chat(self, pcm_24k: bytes, device_id: str | None = None) -> VoiceReply:
        ...
        # 3. LLM：生成回复（注入记忆）
        reply_text = self._llm(user_text, device_id=device_id)
```

把 `chat_stream` 的签名从：

```python
    def chat_stream(
        self,
        pcm_data: bytes,
        sample_rate: int = 24000,
        cancel_event: threading.Event | None = None,
    ) -> Iterator[tuple[int, bytes]]:
```

改为：

```python
    def chat_stream(
        self,
        pcm_data: bytes,
        sample_rate: int = 24000,
        cancel_event: threading.Event | None = None,
        device_id: str | None = None,
    ) -> Iterator[tuple[int, bytes]]:
```

`chat_stream` 中 ASR 完成后调用 `_llm_stream` 的地方，从 `for item in self._llm_stream(user_text):` 改为：

```python
            for item in self._llm_stream(user_text, device_id=device_id):
```

在 `chat_stream` 主循环开始前初始化累积变量（放在 `splitter = SentenceSplitter()` 那一行附近）：

```python
        reply_parts: list[str] = []  # 累积整轮回复，供收尾异步保存
```

在主循环里 yield REPLY_TEXT 的地方同时累积。把：

```python
                for sentence in splitter.feed(token):
                    yield (FRAME_TYPE_REPLY_TEXT, sentence.encode("utf-8"))
                    yield from self._iter_tts_pcm_frames(sentence, cancel_event)
```

改为：

```python
                for sentence in splitter.feed(token):
                    reply_parts.append(sentence)
                    yield (FRAME_TYPE_REPLY_TEXT, sentence.encode("utf-8"))
                    yield from self._iter_tts_pcm_frames(sentence, cancel_event)
```

把 flush 段从：

```python
            rest = splitter.flush()
            if rest:
                yield (FRAME_TYPE_REPLY_TEXT, rest.encode("utf-8"))
                yield from self._iter_tts_pcm_frames(rest, cancel_event)
```

改为：

```python
            rest = splitter.flush()
            if rest:
                reply_parts.append(rest)
                yield (FRAME_TYPE_REPLY_TEXT, rest.encode("utf-8"))
                yield from self._iter_tts_pcm_frames(rest, cancel_event)
```

在 `chat_stream` 最末尾 `yield (FRAME_TYPE_END, b"")` **之前**插入收尾保存：

```python
        # 回复完成后异步保存记忆（守护线程，不阻塞；取消/出错/无记忆时跳过）
        if (reply_parts and not cancelled()
                and llm_error[0] is None
                and self._memory and self._memory.enabled and device_id):
            self._schedule_memory_save(device_id, user_text, "".join(reply_parts))

        yield (FRAME_TYPE_END, b"")
```

> 注意：`cancelled` 是 `chat_stream` 内已定义的闭包 `def cancelled() -> bool`；`llm_error` 是已定义的 `list[BaseException | None]`。两者保持原样直接引用。

- [ ] **Step 6: 新增 `_schedule_memory_save` + `_memory_save_worker` 方法**

在 `VoiceService` 内（`_iter_tts_pcm_frames` 之后）新增：

```python
    def _schedule_memory_save(self, device_id: str, user_text: str, reply: str) -> None:
        """起守护线程异步保存，绝不阻塞当前语音回合。"""
        threading.Thread(
            target=self._memory_save_worker,
            args=(device_id, user_text, reply),
            daemon=True,
        ).start()

    def _memory_save_worker(self, device_id: str, user_text: str, reply: str) -> None:
        try:
            self._memory.save_memory(device_id, user_text, reply)
        except Exception as e:  # noqa: BLE001 — 线程内吞错，不影响主流程
            logger.warning("记忆保存失败: {}", e)
```

- [ ] **Step 7: 跑测试确认通过（含既有 voice 测试不回归）**

```powershell
uv run pytest tests/test_voice_memory.py tests/test_voice_tools.py tests/test_voice_stream.py -v
```
预期：全 PASS（新增 3 个 + 既有全绿，证明 `device_id=None` 默认值向后兼容）。

- [ ] **Step 8: 提交**

```powershell
git add app/services/voice_service.py tests/test_voice_memory.py
git commit -m "feat(voice): VoiceService 接入长期记忆（读注入 + 写后异步）"
```

---

## Task 5: 语音接入层穿透 `device_id`（HTTP + WS）

**Files:**
- Modify: `app/api/voice.py`
- Modify: `app/api/voice_ws.py`
- Test: `tests/test_voice_api.py`、`tests/test_voice_websocket.py`

**Interfaces:**
- Consumes: `VoiceService.chat_stream(..., device_id=...)`（Task 4）；`DeviceSession.device_id`（HTTP）与 `registry.get_session_by_token(token).device_id`（WS）。

- [ ] **Step 1: 更新 `tests/test_voice_api.py` 断言 device_id 透传**

把 `test_16k_pcm_is_passed_directly_to_asr_stream` 从：

```python
def test_16k_pcm_is_passed_directly_to_asr_stream():
    pcm = b"\x01\x00\x02\x00"
    request = MagicMock()
    request.body = AsyncMock(return_value=pcm)
    voice_service = MagicMock()
    voice_service.chat_stream.return_value = iter([])

    async def run_and_drain():
        with patch("app.api.voice._save_debug_wav"):
            response = await voice_chat(
                request=request,
                x_audio_sample_rate=16000,
                session=MagicMock(),
                voice_service=voice_service,
            )
            async for _ in response.body_iterator:
                pass

    asyncio.run(run_and_drain())
    voice_service.chat_stream.assert_called_once_with(pcm, sample_rate=16000)
```

改为（`session` 给定具体 `device_id`，断言透传）：

```python
def test_16k_pcm_passes_device_id_to_voice_service():
    pcm = b"\x01\x00\x02\x00"
    request = MagicMock()
    request.body = AsyncMock(return_value=pcm)
    voice_service = MagicMock()
    voice_service.chat_stream.return_value = iter([])

    async def run_and_drain():
        with patch("app.api.voice._save_debug_wav"):
            response = await voice_chat(
                request=request,
                x_audio_sample_rate=16000,
                session=MagicMock(device_id="dev-http"),
                voice_service=voice_service,
            )
            async for _ in response.body_iterator:
                pass

    asyncio.run(run_and_drain())
    voice_service.chat_stream.assert_called_once_with(
        pcm, sample_rate=16000, device_id="dev-http"
    )
```

- [ ] **Step 2: 更新 `tests/test_voice_websocket.py`：fake 服务签名 + 透传断言**

把 `_Registry` 与 `_VoiceService` 从：

```python
class _Registry:
    def get_session_by_token(self, token: str):
        return object() if token == "valid-token" else None


class _VoiceService:
    def __init__(self) -> None:
        self.calls: list[tuple[bytes, int]] = []

    def chat_stream(self, pcm: bytes, sample_rate: int = 24000, cancel_event=None):
        self.calls.append((pcm, sample_rate))
        yield FRAME_TYPE_ASR_TEXT, "测试".encode()
        yield FRAME_TYPE_TTS_PCM, b"\x00\x00\x01\x00"
        yield FRAME_TYPE_END, b""
```

改为（session 带 `device_id`；fake 记录 device_id）：

```python
class _Session:
    def __init__(self, device_id: str) -> None:
        self.device_id = device_id


class _Registry:
    def get_session_by_token(self, token: str):
        return _Session("dev-ws") if token == "valid-token" else None


class _VoiceService:
    def __init__(self) -> None:
        self.calls: list[tuple[bytes, int, str | None]] = []

    def chat_stream(self, pcm: bytes, sample_rate: int = 24000,
                    cancel_event=None, device_id: str | None = None):
        self.calls.append((pcm, sample_rate, device_id))
        yield FRAME_TYPE_ASR_TEXT, "测试".encode()
        yield FRAME_TYPE_TTS_PCM, b"\x00\x00\x01\x00"
        yield FRAME_TYPE_END, b""
```

同样更新两个子类的 `chat_stream` 签名（加 `device_id: str | None = None`，body 不变）：

- `SlowVoiceService.chat_stream(self, pcm, sample_rate=24000, cancel_event=None, device_id=None)`
- `BrokenVoiceService.chat_stream(self, pcm, sample_rate=24000, cancel_event=None, device_id=None)`

把 `test_normal_flow_uses_native_16k_and_emits_end` 里的断言从：

```python
    assert service.calls == [(b"\x01\x00\x02\x00", 16000)]
```

改为：

```python
    assert service.calls == [(b"\x01\x00\x02\x00", 16000, "dev-ws")]
```

- [ ] **Step 3: 跑测试确认失败（实现还没透传 device_id）**

```powershell
uv run pytest tests/test_voice_api.py::test_16k_pcm_passes_device_id_to_voice_service tests/test_voice_websocket.py::test_normal_flow_uses_native_16k_and_emits_end -v
```
预期：FAIL（`chat_stream` 被调用时没带 `device_id` / calls 不匹配）。

- [ ] **Step 4: 改 `app/api/voice.py` 透传 device_id**

把 `voice_chat` 内 `frame_stream` 从：

```python
    def frame_stream():
        # 直接把设备采样率传给 ASR 入口。16kHz PCM 不再经历
        # 16kHz -> 24kHz -> 16kHz 的二次重采样。
        for ftype, payload in voice_service.chat_stream(
            body, sample_rate=x_audio_sample_rate
        ):
            yield encode_frame(ftype, payload)
```

改为：

```python
    def frame_stream():
        # 直接把设备采样率传给 ASR 入口。16kHz PCM 不再经历
        # 16kHz -> 24kHz -> 16kHz 的二次重采样。
        # device_id 用于语音助手的长期记忆按设备归属。
        for ftype, payload in voice_service.chat_stream(
            body, sample_rate=x_audio_sample_rate, device_id=session.device_id
        ):
            yield encode_frame(ftype, payload)
```

- [ ] **Step 5: 改 `app/api/voice_ws.py`：解析 token→session→device_id 并透传**

删除独立的 `_authorized` 函数（不再需要），把 `voice_ws` 路由内的鉴权与下发改为内联解析 session。即把：

```python
def _authorized(websocket: WebSocket) -> bool:
    header = websocket.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    return scheme.lower() == "bearer" and bool(token) and (
        websocket.app.state.registry.get_session_by_token(token) is not None
    )
```

整段删除。并把 `voice_ws` 路由开头的鉴权块从：

```python
@router.websocket("/ws")
async def voice_ws(websocket: WebSocket) -> None:
    """协议：START JSON → 二进制 PCM* → END_INPUT JSON，CANCEL 可在任意阶段发送。"""
    if not _authorized(websocket):
        logger.warning("WebSocket 语音鉴权失败: client={}", websocket.client)
        await websocket.close(code=4401)
        return
    await websocket.accept()
```

改为：

```python
@router.websocket("/ws")
async def voice_ws(websocket: WebSocket) -> None:
    """协议：START JSON → 二进制 PCM* → END_INPUT JSON，CANCEL 可在任意阶段发送。"""
    header = websocket.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    session = None
    if scheme.lower() == "bearer" and token:
        session = websocket.app.state.registry.get_session_by_token(token)
    if session is None:
        logger.warning("WebSocket 语音鉴权失败: client={}", websocket.client)
        await websocket.close(code=4401)
        return
    device_id = getattr(session, "device_id", "")
    await websocket.accept()
```

把 `end_input` 分支里的下发调用从：

```python
                await _send_stream(websocket, b"".join(chunks))
```

改为：

```python
                await _send_stream(websocket, b"".join(chunks), device_id=device_id)
```

把 `_send_stream` 的签名与内部调用从：

```python
async def _send_stream(websocket: WebSocket, pcm_16k: bytes) -> None:
    ...
    def produce() -> None:
        try:
            for frame in voice_service.chat_stream(
                pcm_16k, sample_rate=16000, cancel_event=cancelled
            ):
```

改为：

```python
async def _send_stream(websocket: WebSocket, pcm_16k: bytes,
                       device_id: str | None = None) -> None:
    ...
    def produce() -> None:
        try:
            for frame in voice_service.chat_stream(
                pcm_16k, sample_rate=16000, cancel_event=cancelled,
                device_id=device_id,
            ):
```

- [ ] **Step 6: 跑测试确认通过（含全量 voice 接入层）**

```powershell
uv run pytest tests/test_voice_api.py tests/test_voice_websocket.py -v
```
预期：全 PASS。

- [ ] **Step 7: 提交**

```powershell
git add app/api/voice.py app/api/voice_ws.py tests/test_voice_api.py tests/test_voice_websocket.py
git commit -m "feat(voice): 语音接入层穿透 device_id 到 VoiceService"
```

---

## Task 6: `app` 装配 `MemoryService` 单例

**Files:**
- Modify: `app/main.py`
- Test: `tests/test_runtime_config.py`

**Interfaces:**
- Consumes: `MemoryService`（Task 3）、`ServerSettings.memory_*`（Task 2）。
- Produces: `app.state.memory_service` 单例；`app.state.voice_service._memory is app.state.memory_service`。

- [ ] **Step 1: 写失败测试（在 `tests/test_runtime_config.py` 末尾追加）**

```python
def test_create_app_wires_memory_service_into_voice_service(tmp_path, monkeypatch):
    config_path = _write_config(tmp_path, memory_enabled=True)
    env_path = tmp_path / ".env"
    env_path.write_text("ZHIPU_API_KEY=zh-key\n", encoding="utf-8")
    (tmp_path / "bins").mkdir()
    (tmp_path / "bins" / "test-manifest.json").write_text("{}", encoding="utf-8")
    monkeypatch.delenv("ZHIPU_API_KEY", raising=False)

    settings = ServerSettings(
        config_path=config_path, env_path=env_path, project_root=tmp_path
    )
    app = create_app(settings)

    assert app.state.memory_service is not None
    assert app.state.voice_service._memory is app.state.memory_service
```

- [ ] **Step 2: 跑测试确认失败**

```powershell
uv run pytest tests/test_runtime_config.py::test_create_app_wires_memory_service_into_voice_service -v
```
预期：FAIL（`app.state` 没有 `memory_service`）。

- [ ] **Step 3: 改 `app/main.py`：构造并注入**

在 `app/main.py` 顶部 import 区，`from app.services.quota_service import QuotaService` 之后追加：

```python
from app.services.memory_service import MemoryService
```

在 `create_app` 内、`app.state.voice_service = VoiceService(...)` **之前**插入：

```python
    # 长期记忆服务：自托管 mem0（默认关闭；未配置或初始化失败时静默降级）
    app.state.memory_service = MemoryService(server_settings)
```

并把 `VoiceService(...)` 的构造从：

```python
    app.state.voice_service = VoiceService(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
    )
```

改为：

```python
    app.state.voice_service = VoiceService(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
        memory_service=app.state.memory_service,
    )
```

- [ ] **Step 4: 跑测试确认通过**

```powershell
uv run pytest tests/test_runtime_config.py -v
```
预期：全 PASS。

- [ ] **Step 5: 提交**

```powershell
git add app/main.py tests/test_runtime_config.py
git commit -m "feat(memory): app 装配 MemoryService 单例并注入 VoiceService"
```

---

## Task 7: 文档同步 + roundtrip 测试 + 全量回归

**Files:**
- Modify: `server/README.md`、`docs/ESP32_API.md`
- Create: `tests/test_memory_roundtrip.py`

- [ ] **Step 1: 写 roundtrip 测试（无 ZHIPU_API_KEY 自动跳过，`tests/test_memory_roundtrip.py`）**

```python
"""端到端：真起 mem0 + 临时 Chroma，验证智谱 OpenAI 兼容 + mem0 抽取确实可跑。

无 ZHIPU_API_KEY 时自动跳过，不阻塞 CI。
"""

import os
from unittest.mock import MagicMock

import pytest

pytestmark = pytest.mark.skipif(
    not os.getenv("ZHIPU_API_KEY"),
    reason="需要 ZHIPU_API_KEY 才能做智谱 + mem0 端到端验证",
)


def _settings(tmp_path):
    s = MagicMock()
    s.memory_enabled = True
    s.zhipu_api_key = os.getenv("ZHIPU_API_KEY")
    s.memory_vector_store_path = str(tmp_path / "chroma")
    s.memory_collection_name = "roundtrip"
    s.memory_llm_model = "glm-4-flash"
    s.memory_embedder_model = "embedding-3"
    s.memory_embedder_dims = 1024
    s.memory_search_top_k = 5
    s.memory_search_threshold = 0.0  # 放宽阈值确保能召回
    return s


def test_add_then_search_roundtrip(tmp_path):
    from app.services.memory_service import MemoryService

    svc = MemoryService(_settings(tmp_path))
    assert svc.enabled, "mem0 初始化失败——检查智谱兼容性与 mem0 配置键名"

    svc.save_memory("dev1", "我叫张三，住在苏州", "你好张三")
    out = svc.query_memory("dev1", "我叫什么名字")
    assert "张三" in out
```

- [ ] **Step 2: 更新 `docs/ESP32_API.md`**

在语音相关章节末尾补一段说明（语音对外接口契约不变）：

```markdown
> 长期记忆为服务端内部增强：服务端在生成回复前召回、回复后异步抽取，**不改变语音
> HTTP/WS 接口的请求与响应格式**，ESP32 固件无需为记忆做任何改动。记忆按设备
> `device_id` 隔离，默认关闭，由服务端 `config.toml` 的 `[memory]` 段控制。
```

- [ ] **Step 3: 更新 `server/README.md`**

新增「长期记忆」小节：

```markdown
## 长期记忆（可选，默认关闭）

语音助手支持按设备的长期事实/偏好记忆（基于自托管 mem0 + 本地 Chroma，复用智谱 GLM）。

开启步骤：
1. 安装可选依赖：`uv sync --extra memory`（或 `uv add --optional memory mem0ai chromadb`）。
2. 确保 `.env` 已配置 `ZHIPU_API_KEY`（与语音复用同一 key）。
3. 在 `config.toml` 把 `[memory].enabled` 改为 `true`。
4. 重启服务，正常对话；自报信息后重启再询问即可验证召回。

记忆数据落盘在 `config.toml` 的 `[memory].vector_store_path`（默认 `data/mem0_chroma`），
仅存本机。未启用或初始化失败时语音功能不受影响。
```

- [ ] **Step 4: 全量回归**

```powershell
uv run pytest -q
```
预期：全 PASS（roundtrip 在无 key 环境 SKIPPED，其余全绿）。

- [ ] **Step 5: 提交**

```powershell
git add docs/ESP32_API.md README.md tests/test_memory_roundtrip.py
git commit -m "docs(memory): 同步配置/README/ESP32_API 文档并补 roundtrip 测试"
```

- [ ] **Step 6: 手动冒烟（交给用户；不主动启动服务）**

提示用户：把 `[memory].enabled` 改 `true` + 确保 `ZHIPU_API_KEY` 后 `.\start_server.ps1`，实聊自报信息（"我叫 XX"）→ 重启服务 → 再问"我叫什么"，确认能召回。并可选跑 `uv run pytest tests/test_memory_roundtrip.py -v`（会真实调用智谱）。

---

## Self-Review 结果

- **Spec 覆盖**：spec §3.1 MemoryService → Task 3；§3.2 VoiceService → Task 4；§3.3 device_id 穿透 → Task 5；§3.4 装配 → Task 6；§5 配置 → Task 2；§6 依赖 → Task 1；§8 测试三类 → Task 3/4/7；§10 文档 → Task 7；§11 实现顺序（装依赖第 1 步）→ Task 1 居首。全覆盖。
- **占位符**：无 TBD/TODO；每步含可执行命令或完整代码。
- **类型一致**：`MemoryService.enabled`、`query_memory(device_id, query)`、`save_memory(device_id, user_text, assistant_text)` 在 Task 3 定义、Task 4/6/7 使用一致；`chat_stream(..., device_id=None)`、`_schedule_memory_save(device_id, user_text, reply)` 在 Task 4 定义、Task 5 透传一致。
- **向后兼容**：所有新参数默认 `None`，既有测试在 Task 4/5 已显式覆盖"无 memory / device_id=None 不回归"。
