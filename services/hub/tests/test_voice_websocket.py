"""语音 WebSocket 协议边界与正常帧序列测试。"""

import time

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.api.voice_ws import router
from app.services.voice_protocol import (
    FRAME_TYPE_ASR_TEXT,
    FRAME_TYPE_END,
    FRAME_TYPE_ERROR,
    FRAME_TYPE_TTS_PCM,
    FrameDecoder,
)


class _Settings:
    device_api_token = "valid-token"
    display_default_device_id = "default"


class _VoiceService:
    def __init__(self) -> None:
        self.calls: list[tuple[bytes, int, str | None]] = []

    def chat_stream(self, pcm: bytes, sample_rate: int = 24000,
                    cancel_event=None, device_id: str | None = None):
        self.calls.append((pcm, sample_rate, device_id))
        yield FRAME_TYPE_ASR_TEXT, "测试".encode()
        yield FRAME_TYPE_TTS_PCM, b"\x00\x00\x01\x00"
        yield FRAME_TYPE_END, b""


def _client(voice_service=None) -> tuple[TestClient, _VoiceService]:
    app = FastAPI()
    app.include_router(router, prefix="/api/v1/voice")
    app.state.server_settings = _Settings()
    app.state.voice_service = voice_service or _VoiceService()
    return TestClient(app), app.state.voice_service


def _headers(token: str = "valid-token") -> dict[str, str]:
    return {
        "Authorization": f"Bearer {token}",
        "X-Device-Id": "dev-ws",
    }


def _start(ws) -> None:
    ws.send_json({
        "type": "start",
        "codec": "pcm_s16le",
        "sample_rate": 16000,
        "channels": 1,
    })


def _receive_protocol_error(ws) -> list[tuple[int, bytes]]:
    decoder = FrameDecoder()
    frames = decoder.feed(ws.receive_bytes())
    frames.extend(decoder.feed(ws.receive_bytes()))
    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]
    return frames


def test_normal_flow_uses_native_16k_and_emits_end():
    client, service = _client()
    decoder = FrameDecoder()
    with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
        _start(ws)
        ws.send_bytes(b"\x01\x00\x02\x00")
        ws.send_json({"type": "end_input"})
        frames = []
        while not any(frame_type == FRAME_TYPE_END for frame_type, _ in frames):
            frames.extend(decoder.feed(ws.receive_bytes()))

    assert service.calls == [(b"\x01\x00\x02\x00", 16000, "dev-ws")]
    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ASR_TEXT,
        FRAME_TYPE_TTS_PCM,
        FRAME_TYPE_END,
    ]


def test_rejects_missing_bearer_token():
    client, _ = _client()
    with pytest.raises(WebSocketDisconnect) as exc:
        with client.websocket_connect("/api/v1/voice/ws"):
            pass
    assert exc.value.code == 4401


def test_rejects_audio_before_start():
    client, _ = _client()
    with pytest.raises(WebSocketDisconnect) as exc:
        with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
            ws.send_bytes(b"\x00\x00")
            _receive_protocol_error(ws)
            ws.receive_bytes()
    assert exc.value.code == 4400


def test_rejects_unsupported_start_parameters():
    client, _ = _client()
    with pytest.raises(WebSocketDisconnect) as exc:
        with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
            ws.send_json({"type": "start", "codec": "opus", "sample_rate": 16000,
                          "channels": 1})
            _receive_protocol_error(ws)
            ws.receive_bytes()
    assert exc.value.code == 4400


def test_rejects_odd_length_pcm():
    client, _ = _client()
    with pytest.raises(WebSocketDisconnect) as exc:
        with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
            _start(ws)
            ws.send_bytes(b"\x00")
            _receive_protocol_error(ws)
            ws.receive_bytes()
    assert exc.value.code == 4400


def test_cancel_stops_before_later_tts():
    class SlowVoiceService(_VoiceService):
        def chat_stream(self, pcm: bytes, sample_rate: int = 24000,
                        cancel_event=None, device_id=None):
            self.calls.append((pcm, sample_rate, device_id))
            yield FRAME_TYPE_ASR_TEXT, b"ok"
            time.sleep(1)
            yield FRAME_TYPE_TTS_PCM, b"\x00\x00"
            yield FRAME_TYPE_END, b""

    client, _ = _client(SlowVoiceService())
    decoder = FrameDecoder()
    with pytest.raises(WebSocketDisconnect) as exc:
        with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
            _start(ws)
            ws.send_bytes(b"\x00\x00")
            ws.send_json({"type": "end_input"})
            frames = decoder.feed(ws.receive_bytes())
            assert frames == [(FRAME_TYPE_ASR_TEXT, b"ok")]
            ws.send_json({"type": "cancel"})
            ws.receive_bytes()
    assert exc.value.code == 1000


def test_unexpected_service_exception_still_emits_error_and_end():
    class BrokenVoiceService(_VoiceService):
        def chat_stream(self, pcm: bytes, sample_rate: int = 24000,
                        cancel_event=None, device_id=None):
            self.calls.append((pcm, sample_rate, device_id))
            raise RuntimeError("boom")
            yield  # pragma: no cover - 保持该函数为生成器

    client, _ = _client(BrokenVoiceService())
    decoder = FrameDecoder()
    with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
        _start(ws)
        ws.send_bytes(b"\x00\x00")
        ws.send_json({"type": "end_input"})
        frames = []
        while not any(frame_type == FRAME_TYPE_END for frame_type, _ in frames):
            frames.extend(decoder.feed(ws.receive_bytes()))

    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]


def test_rejects_empty_end_input_with_error_end_and_close():
    client, _ = _client()
    with pytest.raises(WebSocketDisconnect) as exc:
        with client.websocket_connect("/api/v1/voice/ws", headers=_headers()) as ws:
            _start(ws)
            ws.send_json({"type": "end_input"})
            _receive_protocol_error(ws)
            ws.receive_bytes()
    assert exc.value.code == 4400
