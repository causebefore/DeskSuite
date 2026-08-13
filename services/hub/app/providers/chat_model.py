"""LangChain 对话模型供应商工厂。"""

from langchain_core.language_models.chat_models import BaseChatModel
from langchain_openai import ChatOpenAI


_ZHIPU_OPENAI_BASE_URL = "https://open.bigmodel.cn/api/paas/v4"


def create_chat_model(settings) -> BaseChatModel:
    """根据运行配置创建智谱 OpenAI 兼容对话模型。"""
    provider = getattr(settings, "llm_provider", "zhipu")
    if provider != "zhipu":
        raise ValueError(f"不支持的 LLM provider: {provider}")
    if not settings.zhipu_api_key:
        raise RuntimeError("智谱 API Key 未配置（请设置 ZHIPU_API_KEY）")

    extra_body = None
    if settings.zhipu_llm_model.startswith("glm-4.7"):
        # 保留旧语音链路的低延迟约定；兼容性由真实 GLM 测试覆盖。
        extra_body = {"thinking": {"type": "disabled"}}

    return ChatOpenAI(
        api_key=settings.zhipu_api_key,
        base_url=_ZHIPU_OPENAI_BASE_URL,
        model=settings.zhipu_llm_model,
        temperature=0.3,
        timeout=getattr(settings, "assistant_model_timeout_seconds", 30),
        max_retries=0,
        streaming=True,
        extra_body=extra_body,
        use_responses_api=False,
    )
