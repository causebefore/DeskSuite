"""AI 供应商工厂的纯本地选择与参数测试。"""

from types import SimpleNamespace

import pytest

import app.providers.chat_model as chat_model_module
import app.providers.embeddings as embeddings_module
from app.providers import create_speech_provider
from app.providers.chat_model import create_chat_model
from app.providers.embeddings import create_embeddings
from app.providers.zhipu_speech import ZhipuSpeechProvider


def _settings(**overrides):
    values = {
        "llm_provider": "zhipu",
        "speech_provider": "zhipu",
        "embedding_provider": "zhipu",
        "zhipu_api_key": "unit-test-key",
        "zhipu_llm_model": "glm-4.7",
        "zhipu_asr_model": "asr",
        "zhipu_tts_model": "tts",
        "zhipu_tts_voice": "voice",
        "assistant_model_timeout_seconds": 30,
        "assistant_memory_embedder_model": "embedding-3",
        "assistant_memory_embedder_dims": 1024,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def test_glm47_chat_factory_keeps_low_latency_and_no_retry(monkeypatch):
    captured = {}
    sentinel = object()

    def fake_chat_openai(**kwargs):
        captured.update(kwargs)
        return sentinel

    monkeypatch.setattr(chat_model_module, "ChatOpenAI", fake_chat_openai)

    assert create_chat_model(_settings()) is sentinel
    assert captured["base_url"] == "https://open.bigmodel.cn/api/paas/v4"
    assert captured["extra_body"] == {"thinking": {"type": "disabled"}}
    assert captured["max_retries"] == 0
    assert captured["streaming"] is True


def test_speech_factory_returns_zhipu_adapter():
    assert isinstance(create_speech_provider(_settings()), ZhipuSpeechProvider)


def test_embedding_factory_keeps_dimensions_timeout_and_no_retry(monkeypatch):
    captured = {}
    sentinel = object()

    def fake_openai_embeddings(**kwargs):
        captured.update(kwargs)
        return sentinel

    monkeypatch.setattr(
        embeddings_module,
        "OpenAIEmbeddings",
        fake_openai_embeddings,
    )

    assert create_embeddings(_settings()) is sentinel
    assert captured["api_key"] == "unit-test-key"
    assert captured["base_url"] == "https://open.bigmodel.cn/api/paas/v4"
    assert captured["model"] == "embedding-3"
    assert captured["dimensions"] == 1024
    assert captured["timeout"] == 30
    assert captured["max_retries"] == 0
    assert captured["check_embedding_ctx_length"] is False


def test_embedding_factory_rejects_missing_key_before_client_creation(monkeypatch):
    def unexpected_client_creation(**kwargs):
        del kwargs
        raise AssertionError("缺少 Key 时不应创建 embedding 客户端")

    monkeypatch.setattr(
        embeddings_module,
        "OpenAIEmbeddings",
        unexpected_client_creation,
    )

    with pytest.raises(RuntimeError, match="ZHIPU_API_KEY"):
        create_embeddings(_settings(zhipu_api_key=""))


@pytest.mark.parametrize(
    ("factory", "settings"),
    [
        (create_chat_model, _settings(llm_provider="unknown")),
        (create_speech_provider, _settings(speech_provider="unknown")),
        (create_embeddings, _settings(embedding_provider="unknown")),
    ],
)
def test_provider_factories_reject_unknown_provider(factory, settings):
    with pytest.raises(ValueError, match="不支持"):
        factory(settings)
