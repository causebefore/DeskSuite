"""语音识别和语音合成供应商的最小公共接口。"""

from collections.abc import Iterator
import threading
from typing import Protocol


class SpeechProvider(Protocol):
    """Voice 工作流依赖的语音供应商能力。"""

    @property
    def configured(self) -> bool:
        """当前供应商是否已经配置调用凭据。"""

    def transcribe(self, wav_bytes: bytes) -> str:
        """把 16kHz 单声道 WAV 转成文字。"""

    def synthesize_stream(
        self,
        text: str,
        cancel_event: threading.Event | None = None,
    ) -> Iterator[bytes]:
        """把文字转成 24kHz 单声道 16-bit PCM 分片。"""
