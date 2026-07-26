"""语音流式帧协议、句子切分器和 chat_stream 编排的单元测试。"""

import base64
import struct
from unittest.mock import MagicMock, patch

from app.services.voice_protocol import (
    FRAME_TYPE_ASR_TEXT,
    FRAME_TYPE_END,
    FRAME_TYPE_ERROR,
    FRAME_TYPE_REPLY_TEXT,
    FRAME_TYPE_TTS_PCM,
    FrameDecoder,
    SentenceSplitter,
    encode_frame,
)
from app.services.voice_service import VoiceService


# ── 帧编解码 ───────────────────────────────────────────


class TestEncodeFrame:
    def test_empty_payload(self):
        frame = encode_frame(FRAME_TYPE_END)
        assert frame == struct.pack(">BI", 0x00, 0)

    def test_text_payload(self):
        text = "你好".encode("utf-8")
        frame = encode_frame(FRAME_TYPE_ASR_TEXT, text)
        assert frame[0] == 0x01
        assert struct.unpack(">I", frame[1:5])[0] == len(text)
        assert frame[5:] == text

    def test_binary_payload(self):
        pcm = b"\x00\x01\x02\x03"
        frame = encode_frame(FRAME_TYPE_TTS_PCM, pcm)
        assert frame[0] == 0x03
        assert struct.unpack(">I", frame[1:5])[0] == 4
        assert frame[5:] == pcm


class TestFrameDecoder:
    def test_single_complete_frame(self):
        payload = "test".encode("utf-8")
        wire = encode_frame(FRAME_TYPE_ASR_TEXT, payload)
        decoder = FrameDecoder()
        frames = decoder.feed(wire)
        assert len(frames) == 1
        assert frames[0] == (FRAME_TYPE_ASR_TEXT, payload)

    def test_split_across_feeds(self):
        payload = b"hello world"
        wire = encode_frame(FRAME_TYPE_TTS_PCM, payload)
        decoder = FrameDecoder()
        # 把 wire 拆成两段 feed
        frames1 = decoder.feed(wire[:3])
        assert frames1 == []
        frames2 = decoder.feed(wire[3:])
        assert len(frames2) == 1
        assert frames2[0] == (FRAME_TYPE_TTS_PCM, payload)

    def test_multiple_frames_in_one_feed(self):
        wire = (
            encode_frame(FRAME_TYPE_END)
            + encode_frame(FRAME_TYPE_ERROR, b"err")
            + encode_frame(FRAME_TYPE_TTS_PCM, b"\x01\x02")
        )
        decoder = FrameDecoder()
        frames = decoder.feed(wire)
        assert len(frames) == 3
        assert frames[0] == (FRAME_TYPE_END, b"")
        assert frames[1] == (FRAME_TYPE_ERROR, b"err")
        assert frames[2] == (FRAME_TYPE_TTS_PCM, b"\x01\x02")

    def test_round_trip(self):
        """编解码往返：encode 出的帧能被 decoder 正确还原。"""
        cases = [
            (FRAME_TYPE_END, b""),
            (FRAME_TYPE_ASR_TEXT, "识别文本".encode("utf-8")),
            (FRAME_TYPE_REPLY_TEXT, "回复".encode("utf-8")),
            (FRAME_TYPE_TTS_PCM, bytes(range(256))),
            (FRAME_TYPE_ERROR, "出错了".encode("utf-8")),
        ]
        decoder = FrameDecoder()
        wire = b"".join(encode_frame(ft, pl) for ft, pl in cases)
        frames = decoder.feed(wire)
        assert frames == cases


# ── 句子切分器 ─────────────────────────────────────────


class TestSentenceSplitter:
    def test_single_sentence_no_end(self):
        splitter = SentenceSplitter()
        assert splitter.feed("你好") == []
        assert splitter.flush() == "你好"

    def test_chinese_period(self):
        splitter = SentenceSplitter()
        assert splitter.feed("你好。") == ["你好。"]
        assert splitter.flush() == ""

    def test_multiple_sentences_in_one_feed(self):
        splitter = SentenceSplitter()
        result = splitter.feed("你好。世界。")
        assert result == ["你好。", "世界。"]

    def test_split_across_feeds(self):
        splitter = SentenceSplitter()
        assert splitter.feed("你好") == []
        assert splitter.feed("世") == []
        assert splitter.feed("界。") == ["你好世界。"]

    def test_english_punctuation(self):
        splitter = SentenceSplitter()
        result = splitter.feed("Hello! How are you?")
        assert result == ["Hello!", " How are you?"]

    def test_flush_remaining(self):
        splitter = SentenceSplitter()
        splitter.feed("第一句。第二句没有标点")
        assert splitter.flush() == "第二句没有标点"
        # flush 后缓冲区清空
        assert splitter.flush() == ""

    def test_whitespace_only_not_emitted(self):
        splitter = SentenceSplitter()
        splitter.feed("  \n")
        assert splitter.flush() == ""


# ── chat_stream 编排（mock 智谱 API）───────────────────


def _make_voice_service(api_key: str = "test-key") -> VoiceService:
    settings = MagicMock()
    settings.zhipu_api_key = api_key
    settings.zhipu_llm_model = "glm-4-flash"
    settings.zhipu_asr_model = "glm-asr-2512"
    settings.zhipu_tts_model = "glm-tts"
    settings.zhipu_tts_voice = "female"
    return VoiceService(settings)


class TestChatStream:
    def test_tts_odd_sse_chunks_are_reassembled_into_even_pcm_frames(self):
        svc = _make_voice_service()
        with patch.object(
            svc,
            "_tts_stream",
            return_value=iter([b"\x01", b"\x00\x02", b"\x00"]),
        ):
            frames = list(svc._iter_tts_pcm_frames("测试"))

        assert all(frame_type == FRAME_TYPE_TTS_PCM for frame_type, _ in frames)
        assert all(len(payload) % 2 == 0 for _, payload in frames)
        assert b"".join(payload for _, payload in frames) == b"\x01\x00\x02\x00"

    def test_no_api_key_yields_error_then_end(self):
        svc = _make_voice_service(api_key="")
        frames = list(svc.chat_stream(b"\x00" * 100))
        assert frames[0][0] == FRAME_TYPE_ERROR
        assert frames[1][0] == FRAME_TYPE_END

    def test_asr_failure_yields_error_then_end(self):
        svc = _make_voice_service()
        with patch.object(svc, "_asr", side_effect=RuntimeError("ASR down")):
            frames = list(svc.chat_stream(b"\x00" * 100))
        types = [ft for ft, _ in frames]
        assert FRAME_TYPE_ASR_TEXT not in types
        assert FRAME_TYPE_ERROR in types
        assert types[-1] == FRAME_TYPE_END

    def test_normal_flow_frame_sequence(self):
        svc = _make_voice_service()
        with patch.object(svc, "_asr", return_value="你好"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好呀。", "再见。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01", b"\x02\x03"])):
            frames = list(svc.chat_stream(b"\x00" * 100))

        types = [ft for ft, _ in frames]
        # 第一帧是 ASR 识别结果
        assert types[0] == FRAME_TYPE_ASR_TEXT
        assert frames[0][1] == "你好".encode("utf-8")
        # 应该有 REPLY_TEXT 和 TTS_PCM 帧
        assert FRAME_TYPE_REPLY_TEXT in types
        assert FRAME_TYPE_TTS_PCM in types
        # 最后一帧是 END
        assert types[-1] == FRAME_TYPE_END

    def test_reply_text_and_tts_pcm_interleaving(self):
        """每句 REPLY_TEXT 后面应该紧跟 TTS_PCM 帧。"""
        svc = _make_voice_service()
        with patch.object(svc, "_asr", return_value="测试"), \
             patch.object(svc, "_llm_stream", return_value=iter(["句子一。", "句子二。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\xAA\xBB"])) as mock_tts:
            frames = list(svc.chat_stream(b"\x00" * 100))

        # _tts_stream 被调用两次（每句一次）
        assert mock_tts.call_count == 2
        # 验证每句的文本内容
        reply_payloads = [
            payload.decode("utf-8") for ft, payload in frames
            if ft == FRAME_TYPE_REPLY_TEXT
        ]
        assert reply_payloads == ["句子一。", "句子二。"]

    def test_flush_remaining_text(self):
        """LLM 最后一句没有句末标点时，flush 应该产出剩余文本。"""
        svc = _make_voice_service()
        with patch.object(svc, "_asr", return_value="测试"), \
             patch.object(svc, "_llm_stream", return_value=iter("你好再见。没有标点")), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x00"])):
            frames = list(svc.chat_stream(b"\x00" * 100))

        reply_texts = "".join(
            payload.decode("utf-8") for ft, payload in frames
            if ft == FRAME_TYPE_REPLY_TEXT
        )
        assert "你好再见。" in reply_texts
        assert "没有标点" in reply_texts

    def test_llm_failure_mid_stream_yields_error(self):
        """LLM 流式过程中出错，应 yield ERROR 然后正常 END。"""
        svc = _make_voice_service()

        def llm_broken(text):
            yield "第一句。"
            raise RuntimeError("LLM 断流")

        with patch.object(svc, "_asr", return_value="测试"), \
             patch.object(svc, "_llm_stream", side_effect=llm_broken), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x00"])):
            frames = list(svc.chat_stream(b"\x00" * 100))

        types = [ft for ft, _ in frames]
        assert FRAME_TYPE_ERROR in types
        assert types[-1] == FRAME_TYPE_END


# ── SSE 解析器 ─────────────────────────────────────────


class TestIterSse:
    def test_parse_standard_sse(self):
        from app.services.voice_service import _iter_sse

        lines = [
            b'data: {"choices":[{"delta":{"content":"hello"}}]}\n',
            b"\n",
            b'data: {"choices":[{"delta":{"content":"world"}}]}\n',
            b"\n",
        ]
        mock_resp = MagicMock()
        mock_resp.read1 = MagicMock(side_effect=lines + [b""])
        results = list(_iter_sse(mock_resp))
        assert len(results) == 2
        assert results[0]["choices"][0]["delta"]["content"] == "hello"
        assert results[1]["choices"][0]["delta"]["content"] == "world"

    def test_done_marker_stops(self):
        from app.services.voice_service import _iter_sse

        mock_resp = MagicMock()
        mock_resp.read1 = MagicMock(side_effect=[
            b'data: {"choices":[{"delta":{"content":"hi"}}]}\n\n',
            b"data: [DONE]\n\n",
            b'data: {"should":"not_reach"}\n\n',
        ])
        results = list(_iter_sse(mock_resp))
        assert len(results) == 1

    def test_skip_non_data_lines(self):
        from app.services.voice_service import _iter_sse

        mock_resp = MagicMock()
        mock_resp.read1 = MagicMock(side_effect=[
            b": comment\n\n",
            b'event: message\n',
            b'data: {"ok":true}\n',
            b"\n",
            b"",
        ])
        results = list(_iter_sse(mock_resp))
        assert len(results) == 1
        assert results[0]["ok"] is True
