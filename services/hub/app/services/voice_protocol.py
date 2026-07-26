"""
语音流式帧协议与文本切分工具。

设计说明：
- 流式对话通过 HTTP chunked transfer 持续下发二进制帧，设备端边收边解析。
- 每个帧格式：[1 字节 type][4 字节 big-endian length][payload]，头部共 5 字节。
- 帧类型独立、可按任意顺序出现，设备端按 type 分发处理。
- SentenceSplitter 把 LLM 流式吐出的 token 按句切分，让 TTS 能逐句合成、
  设备端能更早开始播放第一句回复。
"""

import struct

# ── 帧类型 ──────────────────────────────────────────────
# 结束标记：整个流式响应到此结束，payload 为空。
FRAME_TYPE_END = 0x00
# ASR 识别出的用户原话（UTF-8），整段只发送一次。
FRAME_TYPE_ASR_TEXT = 0x01
# LLM 回复文本片段（UTF-8），用于设备端显示字幕，按句增量下发。
FRAME_TYPE_REPLY_TEXT = 0x02
# TTS 合成的 PCM 分片（16-bit 单声道 24kHz raw PCM），设备直接送播放缓冲。
FRAME_TYPE_TTS_PCM = 0x03
# 错误信息（UTF-8），出现后通常紧跟 END 结束流。
FRAME_TYPE_ERROR = 0x80
# 思考中心跳帧：工具调用期间周期性下发，保持 TCP 连接活跃，避免设备端
# 在长时间无数据时触发 HTTP 读超时。payload 为空，设备端可安全忽略。
FRAME_TYPE_THINKING = 0x04

# 帧头：1 字节类型 + 4 字节载荷长度（big-endian uint32），单帧最大约 4GB。
_FRAME_HEADER = struct.Struct(">BI")
_FRAME_HEADER_SIZE = _FRAME_HEADER.size


def encode_frame(frame_type: int, payload: bytes = b"") -> bytes:
    """把帧类型与载荷编码成 wire bytes（帧头 + 载荷）。"""
    if not (0 <= frame_type <= 0xFF):
        raise ValueError(f"帧类型超出单字节范围: {frame_type}")
    if not isinstance(payload, (bytes, bytearray)):
        raise TypeError("payload 必须是 bytes")
    return _FRAME_HEADER.pack(frame_type, len(payload)) + bytes(payload)


class FrameDecoder:
    """
    流式帧解码器。

    持续 feed 收到的原始字节，每次返回已凑齐的完整帧列表
    [(frame_type, payload), ...]。未收齐的半截帧缓存在内部，等下次 feed 补全。
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self._buf.extend(data)
        frames: list[tuple[int, bytes]] = []
        while True:
            # 帧头没收齐，等更多字节
            if len(self._buf) < _FRAME_HEADER_SIZE:
                break
            frame_type, length = _FRAME_HEADER.unpack_from(self._buf)
            # 载荷没收齐，等更多字节
            end = _FRAME_HEADER_SIZE + length
            if len(self._buf) < end:
                break
            payload = bytes(self._buf[_FRAME_HEADER_SIZE:end])
            del self._buf[:end]
            frames.append((frame_type, payload))
        return frames


# 中英文句末标点：遇到即视为一句结束，触发一次 TTS 合成。
_SENTENCE_END_CHARS = set("。！？!?；;\n\r")


class SentenceSplitter:
    """
    按句切分器。

    把 LLM 流式吐出的零散 token 喂进来，遇到句末标点就产出一个完整句子。
    句子内可能包含逗号等停顿标点，但只在句末标点切分，保证 TTS
    合成的句子语义完整、断句自然。结束时调用 flush 取回剩余文本。
    """

    def __init__(self) -> None:
        self._buf = ""

    def feed(self, text: str) -> list[str]:
        self._buf += text
        sentences: list[str] = []
        while True:
            # 找最早的句末标点位置
            cut = -1
            for i, ch in enumerate(self._buf):
                if ch in _SENTENCE_END_CHARS:
                    cut = i
                    break
            if cut < 0:
                break
            # 句末标点并入本句
            end = cut + 1
            sentence = self._buf[:end]
            self._buf = self._buf[end:]
            if sentence.strip():
                sentences.append(sentence)
        return sentences

    def flush(self) -> str:
        """返回缓冲区剩余文本（已 strip），并清空缓冲区。可能为空串。"""
        rest = self._buf.strip()
        self._buf = ""
        return rest
