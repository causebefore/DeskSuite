"""ZhipuSpeechProvider 的纯本地 HTTP、multipart 与 SSE 适配测试。"""

import base64
import json
import threading
from types import SimpleNamespace

import pytest

import app.providers.zhipu_speech as speech_module
from app.providers.zhipu_speech import ZhipuSpeechProvider


class FakeResponse:
    def __init__(
        self,
        *,
        status: int = 200,
        body: bytes = b"",
        chunks: list[bytes] | None = None,
    ) -> None:
        self.status = status
        self.body = body
        self.chunks = list(chunks or [])
        self.read_calls = 0

    def read(self) -> bytes:
        self.read_calls += 1
        return self.body

    def read1(self, size: int) -> bytes:
        del size
        return self.chunks.pop(0) if self.chunks else b""


class FakeConnection:
    def __init__(self, response: FakeResponse) -> None:
        self.response = response
        self.request_call = None
        self.closed = False

    def request(self, *args, **kwargs) -> None:
        self.request_call = (args, kwargs)

    def getresponse(self) -> FakeResponse:
        return self.response

    def close(self) -> None:
        self.closed = True


def _settings(*, key: str = "unit-test-key"):
    return SimpleNamespace(
        zhipu_api_key=key,
        zhipu_asr_model="asr-test",
        zhipu_tts_model="tts-test",
        zhipu_tts_voice="voice-test",
    )


def _install_connection(monkeypatch, response: FakeResponse):
    connection = FakeConnection(response)
    constructor = {}

    def factory(host, *, timeout, context):
        constructor.update(host=host, timeout=timeout, context=context)
        return connection

    monkeypatch.setattr(speech_module.http.client, "HTTPSConnection", factory)
    return connection, constructor


def _sse_audio(data: bytes) -> bytes:
    encoded = base64.b64encode(data).decode("ascii")
    payload = {"choices": [{"delta": {"content": encoded}}]}
    return f"data: {json.dumps(payload)}\n".encode()


def test_unconfigured_provider_rejects_calls_before_network(monkeypatch):
    provider = ZhipuSpeechProvider(_settings(key=""))

    def forbidden(*args, **kwargs):
        raise AssertionError("无 key 时不得构造网络连接")

    monkeypatch.setattr(speech_module.http.client, "HTTPSConnection", forbidden)

    assert provider.configured is False
    with pytest.raises(RuntimeError, match="API Key 未配置"):
        provider.transcribe(b"wav")
    with pytest.raises(RuntimeError, match="API Key 未配置"):
        list(provider.synthesize_stream("你好"))


def test_transcribe_builds_expected_multipart_without_real_http(monkeypatch):
    captured = {}

    def fake_post(path: str, body: bytes, headers: dict[str, str]):
        captured.update(path=path, body=body, headers=headers)
        return {"text": "  识别结果  "}

    monkeypatch.setattr(speech_module, "_zhipu_post", fake_post)
    provider = ZhipuSpeechProvider(_settings())
    wav_bytes = b"RIFF-unit-test-wav"

    assert provider.transcribe(wav_bytes) == "识别结果"
    assert captured["path"] == "/api/paas/v4/audio/transcriptions"
    assert captured["headers"]["Authorization"] == "Bearer unit-test-key"
    assert "multipart/form-data; boundary=" in captured["headers"]["Content-Type"]
    assert b'name="model"' in captured["body"]
    assert b"asr-test" in captured["body"]
    assert b'name="stream"' in captured["body"]
    assert b"false" in captured["body"]
    assert b'filename="audio.wav"' in captured["body"]
    assert wav_bytes in captured["body"]


def test_transcribe_rejects_empty_provider_text(monkeypatch):
    monkeypatch.setattr(speech_module, "_zhipu_post", lambda *args, **kwargs: {"text": "  "})

    with pytest.raises(RuntimeError, match="ASR 返回空文本"):
        ZhipuSpeechProvider(_settings()).transcribe(b"wav")


def test_tts_stream_decodes_fragmented_sse_audio_and_closes_connection(monkeypatch):
    first = _sse_audio(b"\x01\x00")
    second = _sse_audio(b"\x02\x00\x03\x00")
    wire = b": keepalive\n" + first + b"data: invalid-json\n" + second + b"data: [DONE]\n"
    response = FakeResponse(chunks=[wire[:17], wire[17:49], wire[49:]])
    connection, constructor = _install_connection(monkeypatch, response)
    provider = ZhipuSpeechProvider(_settings())

    chunks = list(provider.synthesize_stream("请朗读"))

    assert chunks == [b"\x01\x00", b"\x02\x00\x03\x00"]
    assert constructor["host"] == "open.bigmodel.cn"
    assert constructor["timeout"] == 30
    args, kwargs = connection.request_call
    assert args[:2] == ("POST", "/api/paas/v4/audio/speech")
    payload = json.loads(kwargs["body"])
    assert payload == {
        "model": "tts-test",
        "input": "请朗读",
        "voice": "voice-test",
        "response_format": "pcm",
        "encode_format": "base64",
        "stream": True,
    }
    assert kwargs["headers"]["Authorization"] == "Bearer unit-test-key"
    assert connection.closed is True


def test_tts_cancel_stops_before_yield_and_still_closes_connection(monkeypatch):
    response = FakeResponse(chunks=[_sse_audio(b"\x01\x00") + b"data: [DONE]\n"])
    connection, _ = _install_connection(monkeypatch, response)
    cancel_event = threading.Event()
    cancel_event.set()

    chunks = list(
        ZhipuSpeechProvider(_settings()).synthesize_stream(
            "取消",
            cancel_event=cancel_event,
        )
    )

    assert chunks == []
    assert connection.closed is True


def test_tts_http_failure_does_not_leak_response_body(monkeypatch):
    response = FakeResponse(status=429, body=b"sensitive upstream body")
    connection, _ = _install_connection(monkeypatch, response)

    with pytest.raises(RuntimeError, match="HTTP 429") as exc_info:
        list(ZhipuSpeechProvider(_settings()).synthesize_stream("你好"))

    assert "sensitive" not in str(exc_info.value)
    assert response.read_calls == 1
    assert connection.closed is True


def test_zhipu_post_parses_json_and_closes_connection(monkeypatch):
    response = FakeResponse(status=200, body=b'{"text":"ok"}')
    connection, _ = _install_connection(monkeypatch, response)

    result = speech_module._zhipu_post(
        "/unit-test",
        b"payload",
        {"Authorization": "Bearer fake"},
    )

    assert result == {"text": "ok"}
    assert connection.request_call == (
        ("POST", "/unit-test"),
        {
            "body": b"payload",
            "headers": {"Authorization": "Bearer fake"},
        },
    )
    assert connection.closed is True


@pytest.mark.parametrize(
    ("status", "body", "message"),
    [
        (500, b"secret", "HTTP 500"),
        (200, b"not-json", "响应不是 JSON"),
        (200, b"[]", "响应格式无效"),
    ],
)
def test_zhipu_post_rejects_invalid_response_without_leaking_body(
    monkeypatch,
    status: int,
    body: bytes,
    message: str,
):
    connection, _ = _install_connection(
        monkeypatch,
        FakeResponse(status=status, body=body),
    )

    with pytest.raises(RuntimeError, match=message) as exc_info:
        speech_module._zhipu_post("/unit-test", b"payload", {})

    assert body.decode(errors="ignore") not in str(exc_info.value)
    assert connection.closed is True
