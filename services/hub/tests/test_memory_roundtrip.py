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
    s.memory_llm_model = "glm-4-plus"
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
