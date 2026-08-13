"""DeskSuite Hub 的可插拔供应商适配器。"""

from app.providers.speech import SpeechProvider
from app.providers.zhipu_speech import ZhipuSpeechProvider


def create_speech_provider(settings) -> SpeechProvider:
    """按配置创建语音供应商；工作流不感知具体实现。"""
    provider = getattr(settings, "speech_provider", "zhipu")
    if provider == "zhipu":
        return ZhipuSpeechProvider(settings)
    raise ValueError(f"不支持的 speech provider: {provider}")


__all__ = ["SpeechProvider", "create_speech_provider"]
