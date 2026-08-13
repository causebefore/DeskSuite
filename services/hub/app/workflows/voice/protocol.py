"""语音流式帧协议与文本切分工具。"""

import struct


FRAME_TYPE_END = 0x00
FRAME_TYPE_ASR_TEXT = 0x01
FRAME_TYPE_REPLY_TEXT = 0x02
FRAME_TYPE_TTS_PCM = 0x03
FRAME_TYPE_THINKING = 0x04
FRAME_TYPE_ERROR = 0x80

_FRAME_HEADER = struct.Struct(">BI")
_FRAME_HEADER_SIZE = _FRAME_HEADER.size


def encode_frame(frame_type: int, payload: bytes = b"") -> bytes:
    """把帧类型与载荷编码成帧头加载荷。"""
    if not 0 <= frame_type <= 0xFF:
        raise ValueError(f"帧类型超出单字节范围: {frame_type}")
    if not isinstance(payload, (bytes, bytearray)):
        raise TypeError("payload 必须是 bytes")
    return _FRAME_HEADER.pack(frame_type, len(payload)) + bytes(payload)


class FrameDecoder:
    """持续接收任意分块的 wire bytes，并返回已经完整的帧。"""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self._buffer.extend(data)
        frames: list[tuple[int, bytes]] = []
        while len(self._buffer) >= _FRAME_HEADER_SIZE:
            frame_type, length = _FRAME_HEADER.unpack_from(self._buffer)
            end = _FRAME_HEADER_SIZE + length
            if len(self._buffer) < end:
                break
            payload = bytes(self._buffer[_FRAME_HEADER_SIZE:end])
            del self._buffer[:end]
            frames.append((frame_type, payload))
        return frames


_SENTENCE_END_CHARS = set("。！？!?；;\n\r")


class SentenceSplitter:
    """按中英文句末标点切分模型的流式文本。"""

    def __init__(self) -> None:
        self._buffer = ""

    def feed(self, text: str) -> list[str]:
        self._buffer += text
        sentences: list[str] = []
        while True:
            cut = next(
                (
                    index
                    for index, char in enumerate(self._buffer)
                    if char in _SENTENCE_END_CHARS
                ),
                -1,
            )
            if cut < 0:
                return sentences
            end = cut + 1
            sentence = self._buffer[:end]
            self._buffer = self._buffer[end:]
            if sentence.strip():
                sentences.append(sentence)

    def flush(self) -> str:
        """返回尚未遇到句末标点的剩余文字并清空缓冲。"""
        rest = self._buffer.strip()
        self._buffer = ""
        return rest
