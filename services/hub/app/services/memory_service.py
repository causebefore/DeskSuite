"""长期记忆服务：封装自托管 mem0，供语音与网页渲染使用。

设计要点：
- 模块顶层不 import mem0，禁用或未装 extra 时不影响 app 启动。
- mem0 的 Memory 客户端懒加载（首次 query/save 时），初始化失败置 _broken=True。
- 所有运行期异常都被吞掉 + 日志告警，绝不影响语音回合。
- 记忆按 device_id（ESP32 注册时上报）隔离。
"""

import threading

from loguru import logger

# 智谱 BigModel 的 OpenAI 兼容端点；mem0 的 openai provider 用它复用现有 key。
_ZHIPU_OPENAI_BASE_URL = "https://open.bigmodel.cn/api/paas/v4"


class MemoryService:
    def __init__(self, settings) -> None:
        self._settings = settings
        self._client = None        # mem0.Memory 实例，懒加载
        self._broken = False       # 初始化失败标记
        self._lock = threading.Lock()  # 保护 _client/_broken 的并发懒加载
        self._config_enabled = bool(getattr(settings, "memory_enabled", False))
        self._api_key = getattr(settings, "zhipu_api_key", "") or ""

    @property
    def enabled(self) -> bool:
        return self._config_enabled and bool(self._api_key) and not self._broken

    def _build_config(self) -> dict:
        s = self._settings
        return {
            # 让 GLM 抽取事实时保留用户原始语言（中文对话就用中文记录，不罗马化/翻译），
            # 否则 mem0 默认 prompt 会把"张三"存成"Zhang San"，注入中文语音 prompt 不自然。
            "custom_instructions": (
                "你是记忆抽取助手。从对话中提取值得长期记住的事实（用户身份、偏好、"
                "重要事件、关系等）。必须使用用户对话的原始语言记录（中文对话就用中文，"
                "不要翻译或罗马化）；每条一个独立事实，用简洁陈述句；只记长期有用的事实，"
                "忽略客套、即时问答与无关细节；没有可记的事实时返回空。"
            ),
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
        # 快路径：已初始化或已禁用，无需加锁。
        if self._client is not None or self._broken:
            return self._client
        if not self._config_enabled or not self._api_key:
            return None
        # 慢路径：加锁 + 双检，避免并发线程重复 from_config / 竞争置 _broken。
        with self._lock:
            if self._client is not None or self._broken:
                return self._client
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
        # mem0 2.x 的 search() 返回 {"results": [...]}（dict）；兼容裸 list 形状以防版本差异。
        items = results.get("results", []) if isinstance(results, dict) else (results or [])
        memories = [r.get("memory") for r in items if isinstance(r, dict) and r.get("memory")]
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
