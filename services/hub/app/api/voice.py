"""
语音交互接口。

路由前缀：/api/v1/voice（由 main.py 装配）

接口列表：
- POST /chat  — 流式语音对话：上传录音 PCM，返回二进制帧流

设计说明：
- 设备端上传 raw PCM（Content-Type: audio/pcm），通过 X-Audio-Sample-Rate 头告知采样率
- server 端完成 ASR(智谱) → LLM 流式(智谱) → TTS 流式(智谱) 全链路
- 响应为 HTTP chunked 二进制帧流，每帧格式：[1字节type][4字节len][payload]
- 设备端边收帧边处理：ASR_TEXT 显示识别结果，TTS_PCM 送播放，END 表示结束

帧类型（见 voice_protocol.py）：
- 0x00 END: 流结束
- 0x01 ASR_TEXT: 用户原话
- 0x02 REPLY_TEXT: LLM 回复文本片段
- 0x03 TTS_PCM: 24kHz 单声道 16-bit PCM 分片
- 0x80 ERROR: 错误信息
"""

import io
import wave
from datetime import datetime
from pathlib import Path

from fastapi import APIRouter, Depends, Header, HTTPException, Request, status
from fastapi.responses import FileResponse, StreamingResponse
from loguru import logger
from starlette.requests import ClientDisconnect

from app.api.dependencies import get_device_id
from app.services.voice_protocol import encode_frame
from app.services.voice_service import VoiceService

router = APIRouter()

# 调试：保存上传的原始 PCM 为 WAV 文件，方便检查音频质量
_DEBUG_AUDIO_DIR = Path(__file__).resolve().parent.parent.parent / "debug_audio"
_last_debug_wav: Path | None = None


def _save_debug_wav(pcm: bytes, sample_rate: int, tag: str = "upload") -> Path:
    _DEBUG_AUDIO_DIR.mkdir(exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    wav_path = _DEBUG_AUDIO_DIR / f"{ts}_{tag}_{sample_rate}hz.wav"
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)
    wav_path.write_bytes(buf.getvalue())
    return wav_path


def get_voice_service(request: Request) -> VoiceService:
    """从请求上下文获取 VoiceService 单例。"""
    return request.app.state.voice_service


@router.post("/chat")
async def voice_chat(
    request: Request,
    x_audio_sample_rate: int = Header(default=24000, alias="X-Audio-Sample-Rate"),
    device_id: str = Depends(get_device_id),
    voice_service: VoiceService = Depends(get_voice_service),
) -> StreamingResponse:
    """
    流式语音对话回合。

    请求：
        Body: raw PCM（单声道 16-bit），采样率由 X-Audio-Sample-Rate 头指定（默认 24000）
        Header: Authorization: Bearer <shared_device_token>（未配置时可省略）
        Header: X-Device-Id: <device_id>（可选）

    响应：
        Body: 二进制帧流（application/octet-stream），设备端逐帧解析
        帧格式：[1字节type][4字节big-endian len][payload]
    """
    # 设备端上传 PCM 期间可能因网络抖动断开连接（ESP32 常见），
    # Starlette 会抛 ClientDisconnect；静默处理并返回空流，避免 uvicorn
    # 把它当成 500 打印完整异常栈污染日志。
    try:
        body = await request.body()
    except ClientDisconnect:
        logger.info("语音对话：设备上传音频时连接中断，已忽略")
        return StreamingResponse(iter([]), media_type="application/octet-stream")
    if not body:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="请求体为空，期望 raw PCM 音频数据",
        )
    if len(body) % 2:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="PCM 数据长度必须为 16-bit 样本的偶数字节",
        )

    if x_audio_sample_rate not in (8000, 16000, 22050, 24000, 32000, 44100, 48000):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"不支持的采样率: {x_audio_sample_rate}",
        )

    global _last_debug_wav
    _last_debug_wav = _save_debug_wav(body, x_audio_sample_rate, "upload")

    def frame_stream():
        # 直接把设备采样率传给 ASR 入口。16kHz PCM 不再经历
        # 16kHz -> 24kHz -> 16kHz 的二次重采样。
        # device_id 用于语音助手的长期记忆按设备归属。
        for ftype, payload in voice_service.chat_stream(
            body, sample_rate=x_audio_sample_rate, device_id=device_id
        ):
            yield encode_frame(ftype, payload)

    return StreamingResponse(
        frame_stream(),
        media_type="application/octet-stream",
    )


@router.get("/debug/audio/last")
async def debug_get_last_audio() -> FileResponse:
    """调试接口：下载最近一次上传的录音 WAV 文件。"""
    if _last_debug_wav is None or not _last_debug_wav.exists():
        raise HTTPException(status_code=404, detail="暂无录音文件")
    return FileResponse(
        path=str(_last_debug_wav),
        media_type="audio/wav",
        filename=_last_debug_wav.name,
    )


@router.get("/debug/audio/list")
async def debug_list_audio() -> dict:
    """调试接口：列出所有保存的录音文件。"""
    if not _DEBUG_AUDIO_DIR.exists():
        return {"files": []}
    files = sorted(_DEBUG_AUDIO_DIR.glob("*.wav"), reverse=True)
    return {"files": [f.name for f in files]}


@router.get("/debug/audio/{filename}")
async def debug_get_audio(filename: str) -> FileResponse:
    """调试接口：按文件名下载指定录音。"""
    safe = _DEBUG_AUDIO_DIR / filename
    if not safe.exists() or not safe.is_relative_to(_DEBUG_AUDIO_DIR):
        raise HTTPException(status_code=404, detail="文件不存在")
    return FileResponse(
        path=str(safe),
        media_type="audio/wav",
        filename=filename,
    )
