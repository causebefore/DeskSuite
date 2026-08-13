"""智谱 ASR/TTS 的标准库 HTTP 适配。"""

import base64
import http.client
import json
import ssl
import threading
from collections.abc import Iterator


_ZHIPU_HOST = "open.bigmodel.cn"
_ZHIPU_TIMEOUT_SECONDS = 30


class ZhipuSpeechProvider:
    """使用同一个 ``ZHIPU_API_KEY`` 提供语音识别和流式语音合成。"""

    def __init__(self, settings) -> None:
        self._api_key = settings.zhipu_api_key
        self._asr_model = settings.zhipu_asr_model
        self._tts_model = settings.zhipu_tts_model
        self._tts_voice = settings.zhipu_tts_voice

    @property
    def configured(self) -> bool:
        return bool(self._api_key)

    def transcribe(self, wav_bytes: bytes) -> str:
        """上传 16kHz 单声道 WAV，并返回识别文本。"""
        self._require_key()
        boundary = "----DeskSuiteVoiceBoundary"
        body = _build_multipart(
            boundary,
            {"model": self._asr_model, "stream": "false"},
            ("file", "audio.wav", "audio/wav", wav_bytes),
        )
        response = _zhipu_post(
            "/api/paas/v4/audio/transcriptions",
            body,
            headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": f"multipart/form-data; boundary={boundary}",
            },
        )
        text = str(response.get("text", "")).strip()
        if not text:
            raise RuntimeError("ASR 返回空文本")
        return text

    def synthesize_stream(
        self,
        text: str,
        cancel_event: threading.Event | None = None,
    ) -> Iterator[bytes]:
        """通过 SSE 逐块返回 24kHz 单声道 16-bit PCM。"""
        self._require_key()
        payload = json.dumps(
            {
                "model": self._tts_model,
                "input": text,
                "voice": self._tts_voice,
                "response_format": "pcm",
                "encode_format": "base64",
                "stream": True,
            }
        ).encode("utf-8")
        connection = http.client.HTTPSConnection(
            _ZHIPU_HOST,
            timeout=_ZHIPU_TIMEOUT_SECONDS,
            context=ssl.create_default_context(),
        )
        try:
            connection.request(
                "POST",
                "/api/paas/v4/audio/speech",
                body=payload,
                headers={
                    "Authorization": f"Bearer {self._api_key}",
                    "Content-Type": "application/json",
                },
            )
            response = connection.getresponse()
            if response.status != 200:
                response.read()
                raise RuntimeError(f"TTS 流式请求失败: HTTP {response.status}")
            for data in _iter_sse(response):
                if cancel_event is not None and cancel_event.is_set():
                    return
                choices = data.get("choices") or []
                if not choices:
                    continue
                content = choices[0].get("delta", {}).get("content", "")
                if content:
                    yield base64.b64decode(content)
        finally:
            connection.close()

    def _require_key(self) -> None:
        if not self._api_key:
            raise RuntimeError("智谱 API Key 未配置（请设置 ZHIPU_API_KEY）")


def _iter_sse(response) -> Iterator[dict]:
    """解析智谱流式接口返回的 SSE ``data`` 行。"""
    buffer = ""
    while True:
        chunk = response.read1(4096)
        if not chunk:
            return
        buffer += chunk.decode("utf-8", errors="replace")
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            line = line.strip()
            if not line.startswith("data:"):
                continue
            raw = line[5:].strip()
            if raw == "[DONE]":
                return
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if isinstance(data, dict):
                yield data


def _zhipu_post(path: str, body: bytes, headers: dict[str, str]) -> dict:
    """发送智谱 JSON 响应 POST 请求，不在异常中输出响应正文。"""
    connection = http.client.HTTPSConnection(
        _ZHIPU_HOST,
        timeout=_ZHIPU_TIMEOUT_SECONDS,
        context=ssl.create_default_context(),
    )
    try:
        connection.request("POST", path, body=body, headers=headers)
        response = connection.getresponse()
        raw = response.read()
        if response.status != 200:
            raise RuntimeError(f"智谱 API {path} 失败: HTTP {response.status}")
    finally:
        connection.close()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"智谱 API {path} 响应不是 JSON") from exc
    if not isinstance(data, dict):
        raise RuntimeError(f"智谱 API {path} 响应格式无效")
    return data


def _build_multipart(
    boundary: str,
    fields: dict[str, str],
    file_value: tuple[str, str, str, bytes],
) -> bytes:
    """构造单文件 multipart/form-data 请求体。"""
    chunks: list[bytes] = []
    crlf = b"\r\n"
    for name, value in fields.items():
        chunks.extend(
            [
                f"--{boundary}".encode(),
                crlf,
                f'Content-Disposition: form-data; name="{name}"'.encode(),
                crlf,
                crlf,
                value.encode("utf-8"),
                crlf,
            ]
        )
    field_name, filename, content_type, data = file_value
    chunks.extend(
        [
            f"--{boundary}".encode(),
            crlf,
            (
                f'Content-Disposition: form-data; name="{field_name}"; '
                f'filename="{filename}"'
            ).encode(),
            crlf,
            f"Content-Type: {content_type}".encode(),
            crlf,
            crlf,
            data,
            crlf,
            f"--{boundary}--".encode(),
            crlf,
        ]
    )
    return b"".join(chunks)
