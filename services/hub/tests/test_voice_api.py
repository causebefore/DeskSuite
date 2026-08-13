"""voice 路由端点边界测试：客户端上传中断、空 body 等场景。"""

import asyncio
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from fastapi import FastAPI, HTTPException
from fastapi.testclient import TestClient
from starlette.requests import ClientDisconnect

from app.api import voice
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
            thread_id=None,
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
                thread_id=None,
                voice_service=MagicMock(),
            )
        )
    assert exc_info.value.status_code == 400


def test_16k_pcm_passes_device_id_to_voice_service():
    """16kHz HTTP 上传不得再经历 16k -> 24k -> 16k 二次重采样。"""
    pcm = b"\x01\x00\x02\x00"
    request = MagicMock()
    request.body = AsyncMock(return_value=pcm)
    request.app.state.server_settings = SimpleNamespace(
        voice_debug_audio_enabled=False
    )
    voice_service = MagicMock()
    voice_service.chat_stream.return_value = iter([])

    async def run_and_drain():
        with patch("app.api.voice._save_debug_wav") as save_debug_wav:
            response = await voice_chat(
                request=request,
                x_audio_sample_rate=16000,
                device_id="dev-http",
                thread_id=None,
                voice_service=voice_service,
            )
            async for _ in response.body_iterator:
                pass
            save_debug_wav.assert_not_called()

    asyncio.run(run_and_drain())
    voice_service.chat_stream.assert_called_once_with(
        pcm, sample_rate=16000, device_id="dev-http", thread_id=None
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
            thread_id=None,
            voice_service=MagicMock(),
            )
        )
    assert exc_info.value.status_code == 400


def test_debug_audio_save_runs_only_when_enabled():
    pcm = b"\x01\x00\x02\x00"
    request = MagicMock()
    request.body = AsyncMock(return_value=pcm)
    request.app.state.server_settings = SimpleNamespace(
        voice_debug_audio_enabled=True
    )
    voice_service = MagicMock()
    voice_service.chat_stream.return_value = iter([])

    async def run_and_drain():
        with patch(
            "app.api.voice._save_debug_wav",
            return_value=Path("debug.wav"),
        ) as save_debug_wav:
            response = await voice_chat(
                request=request,
                x_audio_sample_rate=16000,
                device_id="dev-http",
                thread_id=None,
                voice_service=voice_service,
            )
            async for _ in response.body_iterator:
                pass
            save_debug_wav.assert_called_once_with(pcm, 16000, "upload")

    asyncio.run(run_and_drain())


def test_debug_audio_routes_reuse_device_token(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    app = FastAPI()
    app.state.server_settings = SimpleNamespace(device_api_token="debug-token")
    app.include_router(voice.debug_router, prefix="/api/v1/voice")
    monkeypatch.setattr(voice, "_DEBUG_AUDIO_DIR", tmp_path)
    client = TestClient(app)

    missing = client.get("/api/v1/voice/debug/audio/list")
    wrong = client.get(
        "/api/v1/voice/debug/audio/list",
        headers={"Authorization": "Bearer wrong-token"},
    )
    accepted = client.get(
        "/api/v1/voice/debug/audio/list",
        headers={"Authorization": "Bearer debug-token"},
    )

    assert missing.status_code == 401
    assert wrong.status_code == 401
    assert accepted.status_code == 200
    assert accepted.json() == {"files": []}


def test_hub_registers_all_debug_audio_routes_only_when_enabled(tmp_path: Path):
    from app.core.config import ServerSettings
    from app.main import create_app

    project_root = Path(__file__).resolve().parents[1]
    env_path = tmp_path / ".env"
    env_path.write_text("", encoding="utf-8")
    settings = ServerSettings(project_root / "config.toml", env_path, project_root)
    settings.device_api_token = "debug-token"
    settings.runtime_log_dir = tmp_path / "runtime_logs"
    settings.device_status_dir = tmp_path / "device_status"
    settings.ota_manifest_dir = tmp_path / "manifests"
    settings.ota_artifact_dir = tmp_path / "artifacts"

    debug_paths = {
        "/api/v1/voice/debug/audio/last",
        "/api/v1/voice/debug/audio/list",
        "/api/v1/voice/debug/audio/{filename}",
    }
    assert debug_paths.isdisjoint(create_app(settings).openapi()["paths"])

    settings.voice_debug_audio_enabled = True
    paths = create_app(settings).openapi()["paths"]

    assert debug_paths <= paths.keys()
