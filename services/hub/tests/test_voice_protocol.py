"""设备语音二进制帧与流式句子切分的兼容契约测试。"""

import struct

import pytest

from app.workflows.voice.protocol import (
    FRAME_TYPE_ASR_TEXT,
    FRAME_TYPE_END,
    FRAME_TYPE_ERROR,
    FRAME_TYPE_REPLY_TEXT,
    FrameDecoder,
    SentenceSplitter,
    encode_frame,
)


def test_encode_frame_keeps_one_byte_type_and_big_endian_length():
    payload = "你好".encode("utf-8")

    encoded = encode_frame(FRAME_TYPE_ASR_TEXT, payload)

    assert encoded[:5] == struct.pack(">BI", FRAME_TYPE_ASR_TEXT, len(payload))
    assert encoded[5:] == payload


def test_decoder_handles_header_and_payload_split_at_every_boundary():
    encoded = encode_frame(FRAME_TYPE_REPLY_TEXT, "测试分片".encode())

    for split_at in range(len(encoded)):
        decoder = FrameDecoder()
        assert decoder.feed(encoded[:split_at]) == []
        assert decoder.feed(encoded[split_at:]) == [
            (FRAME_TYPE_REPLY_TEXT, "测试分片".encode())
        ]


def test_decoder_returns_multiple_frames_and_keeps_incomplete_tail():
    first = encode_frame(FRAME_TYPE_ERROR, b"error")
    second = encode_frame(FRAME_TYPE_END)
    third = encode_frame(FRAME_TYPE_ASR_TEXT, b"next")
    decoder = FrameDecoder()

    frames = decoder.feed(first + second + third[:-2])

    assert frames == [
        (FRAME_TYPE_ERROR, b"error"),
        (FRAME_TYPE_END, b""),
    ]
    assert decoder.feed(third[-2:]) == [(FRAME_TYPE_ASR_TEXT, b"next")]


@pytest.mark.parametrize("frame_type", [-1, 256])
def test_encode_frame_rejects_type_outside_one_byte(frame_type: int):
    with pytest.raises(ValueError, match="单字节"):
        encode_frame(frame_type)


def test_encode_frame_rejects_non_bytes_payload():
    with pytest.raises(TypeError, match="bytes"):
        encode_frame(FRAME_TYPE_ASR_TEXT, "not-bytes")  # type: ignore[arg-type]


def test_sentence_splitter_handles_tokens_split_across_sentence_boundaries():
    splitter = SentenceSplitter()

    assert splitter.feed("第一") == []
    assert splitter.feed("句。第二句") == ["第一句。"]
    assert splitter.feed("！third?") == ["第二句！", "third?"]
    assert splitter.flush() == ""


def test_sentence_splitter_supports_semicolon_newline_and_flushes_remainder():
    splitter = SentenceSplitter()

    assert splitter.feed("甲；乙\n丙") == ["甲；", "乙\n"]
    assert splitter.flush() == "丙"
    assert splitter.flush() == ""


def test_sentence_splitter_discards_whitespace_only_sentences():
    splitter = SentenceSplitter()

    assert splitter.feed("\n\r有效。") == ["有效。"]
