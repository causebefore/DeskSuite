"""voice 路由端点边界测试：客户端上传中断、空 body 等场景。"""

import asyncio
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from fastapi import HTTPException
from starlette.requests import ClientDisconnect

from app.api.voice import voice_chat


def test_client_disconnect_during_upload_returns_empty_stream():
    """设备上传 PCM 期间连接中断，端点应静默返回空流，不再抛 ClientDisconnect。"""
    request = MagicMock()
    request.body = AsyncMock(side_effect=ClientDisconnect())

    async def run_and_drain():
        response = await voice_chat(
            request=request,
            x_audio_sample_rate=24000,
            device_id="dev-http",
            voice_service=MagicMock(),
        )
        chunks = []
        async for chunk in response.body_iterator:
            chunks.append(chunk)
        return response, chunks

    response, chunks = asyncio.run(run_and_drain())

    # 返回空二进制帧流，设备端会立即收到流结束
    assert response.media_type == "application/octet-stream"
    assert chunks == []


def test_empty_body_returns_400():
    """上传空 body 仍应返回 400，而非走到后续音频处理。"""
    request = MagicMock()
    request.body = AsyncMock(return_value=b"")

    with pytest.raises(HTTPException) as exc_info:
        asyncio.run(
            voice_chat(
                request=request,
                x_audio_sample_rate=24000,
                device_id="dev-http",
                voice_service=MagicMock(),
            )
        )
    assert exc_info.value.status_code == 400


def test_16k_pcm_passes_device_id_to_voice_service():
    """16kHz HTTP 上传不得再经历 16k -> 24k -> 16k 二次重采样。"""
    pcm = b"\x01\x00\x02\x00"
    request = MagicMock()
    request.body = AsyncMock(return_value=pcm)
    voice_service = MagicMock()
    voice_service.chat_stream.return_value = iter([])

    async def run_and_drain():
        with patch("app.api.voice._save_debug_wav"):
            response = await voice_chat(
                request=request,
                x_audio_sample_rate=16000,
                device_id="dev-http",
                voice_service=voice_service,
            )
            async for _ in response.body_iterator:
                pass

    asyncio.run(run_and_drain())
    voice_service.chat_stream.assert_called_once_with(
        pcm, sample_rate=16000, device_id="dev-http"
    )


def test_odd_pcm_body_returns_400():
    request = MagicMock()
    request.body = AsyncMock(return_value=b"\x00")

    with pytest.raises(HTTPException) as exc_info:
        asyncio.run(
            voice_chat(
                request=request,
                x_audio_sample_rate=16000,
                device_id="dev-http",
                voice_service=MagicMock(),
            )
        )
    assert exc_info.value.status_code == 400
