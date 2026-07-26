"""MemoryService 单测：mock mem0.Memory，验证召回格式、保存入参、降级路径。"""

import threading
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
    # mem0 2.x 的 search() 返回 {"results": [...]}（dict，非裸 list）
    fake.search.return_value = {"results": [{"memory": "用户叫张三"}, {"memory": "喜欢科幻"}]}
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
        fake.search.return_value = {"results": []}
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


def test_concurrent_ensure_client_initializes_once():
    """多线程并发 _ensure_client，from_config 必须只调一次，所有线程拿到同一 client。"""
    calls = {"n": 0}
    real_from_config = MagicMock()
    barrier = threading.Barrier(8)  # 8 个线程同时冲过快路径，最大化竞争窗口

    def counting_from_config(_cfg):
        calls["n"] += 1
        return real_from_config

    results = []
    errors = []

    def runner():
        try:
            barrier.wait(timeout=5)
        except threading.BrokenBarrierError as e:
            errors.append(e)
            return
        results.append(svc._ensure_client())

    with patch("mem0.Memory") as M:
        M.from_config.side_effect = counting_from_config
        svc = MemoryService(_settings())
        threads = [threading.Thread(target=runner) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

    assert not errors, f"barrier broken: {errors}"
    assert calls["n"] == 1, f"from_config 应只调一次，实际 {calls['n']}"
    assert len(results) == 8
    assert all(r is real_from_config for r in results), "所有线程应拿到同一 client 实例"
