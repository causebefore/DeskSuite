"""LangChain 向量模型供应商工厂。"""

from langchain_core.embeddings import Embeddings
from langchain_openai import OpenAIEmbeddings


_ZHIPU_OPENAI_BASE_URL = "https://open.bigmodel.cn/api/paas/v4"


def create_embeddings(settings) -> Embeddings:
    """创建用于 LangGraph Store 语义检索的智谱向量模型。"""
    provider = getattr(settings, "embedding_provider", "zhipu")
    if provider != "zhipu":
        raise ValueError(f"不支持的 embedding provider: {provider}")
    if not settings.zhipu_api_key:
        raise RuntimeError("智谱 API Key 未配置（请设置 ZHIPU_API_KEY）")

    return OpenAIEmbeddings(
        api_key=settings.zhipu_api_key,
        base_url=_ZHIPU_OPENAI_BASE_URL,
        model=settings.assistant_memory_embedder_model,
        dimensions=settings.assistant_memory_embedder_dims,
        timeout=getattr(settings, "assistant_model_timeout_seconds", 30),
        max_retries=0,
        check_embedding_ctx_length=False,
    )
