"""语音 WebSocket 会话：上行 16k PCM，下行沿用既有二进制帧协议。"""

import asyncio
import contextlib
import json
import threading
from concurrent.futures import ThreadPoolExecutor

from fastapi import APIRouter, WebSocket, WebSocketDisconnect
from loguru import logger
from starlette.websockets import WebSocketState

from app.api.dependencies import device_token_is_valid, thread_id_is_valid
from app.workflows.voice.protocol import FRAME_TYPE_END, FRAME_TYPE_ERROR, encode_frame

router = APIRouter()

_MAX_PCM_BYTES = 16_000 * 2 * 30
_START = {"type": "start", "codec": "pcm_s16le", "sample_rate": 16000, "channels": 1}
_VOICE_EXECUTOR = ThreadPoolExecutor(max_workers=4, thread_name_prefix="voice_ws_stream")


async def _send_stream(
    websocket: WebSocket,
    pcm_16k: bytes,
    device_id: str | None = None,
    thread_id: str | None = None,
) -> None:
    """在线程中消费阻塞式 AI 生成器，并让 WebSocket 接收循环可随时取消。"""
    queue: asyncio.Queue[tuple[int, bytes] | None] = asyncio.Queue(maxsize=64)
    loop = asyncio.get_running_loop()
    cancelled = threading.Event()
    voice_service = websocket.app.state.voice_service
    sent_frames = 0
    sent_bytes = 0

    def enqueue(item: tuple[int, bytes] | None) -> None:
        try:
            queue.put_nowait(item)
        except asyncio.QueueFull:
            logger.error("WebSocket 下行队列已满，取消本轮语音生成")
            cancelled.set()

    def produce() -> None:
        try:
            for frame in voice_service.chat_stream(
                pcm_16k,
                sample_rate=16000,
                cancel_event=cancelled,
                device_id=device_id,
                thread_id=thread_id,
            ):
                if cancelled.is_set():
                    break
                loop.call_soon_threadsafe(enqueue, frame)
        except Exception as exc:
            logger.exception("WebSocket 语音生成器异常")
            if not cancelled.is_set():
                message = f"语音流处理失败: {exc}".encode("utf-8")
                loop.call_soon_threadsafe(enqueue, (FRAME_TYPE_ERROR, message))
                loop.call_soon_threadsafe(enqueue, (FRAME_TYPE_END, b""))
        finally:
            with contextlib.suppress(RuntimeError):
                loop.call_soon_threadsafe(enqueue, None)

    async def wait_cancel() -> None:
        try:
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    cancelled.set()
                    return
                if message.get("text"):
                    try:
                        control = json.loads(message["text"])
                    except json.JSONDecodeError:
                        continue
                    if control.get("type") == "cancel":
                        logger.info("WebSocket 语音会话收到 CANCEL")
                        cancelled.set()
                        return
        except WebSocketDisconnect:
            cancelled.set()

    _VOICE_EXECUTOR.submit(produce)
    cancel_task = asyncio.create_task(wait_cancel())
    try:
        while not cancelled.is_set():
            try:
                frame = await asyncio.wait_for(queue.get(), timeout=0.25)
            except TimeoutError:
                continue
            if frame is None:
                return
            encoded = encode_frame(*frame)
            await websocket.send_bytes(encoded)
            sent_frames += 1
            sent_bytes += len(encoded)
    finally:
        was_cancelled = cancelled.is_set()
        cancelled.set()
        if not cancel_task.done():
            cancel_task.cancel()
        await asyncio.gather(cancel_task, return_exceptions=True)
        logger.info(
            "WebSocket 下行结束: frames={} bytes={} cancelled={}",
            sent_frames,
            sent_bytes,
            was_cancelled,
        )


async def _close_protocol_error(websocket: WebSocket, reason: str) -> None:
    """用完整 ERROR+END 帧收口协议错误，再以 4400 关闭连接。"""
    with contextlib.suppress(RuntimeError, WebSocketDisconnect):
        await websocket.send_bytes(encode_frame(FRAME_TYPE_ERROR, reason.encode("utf-8")))
        await websocket.send_bytes(encode_frame(FRAME_TYPE_END, b""))
    with contextlib.suppress(RuntimeError):
        await websocket.close(code=4400, reason=reason)


@router.websocket("/ws")
async def voice_ws(websocket: WebSocket) -> None:
    """协议：START JSON → 二进制 PCM* → END_INPUT JSON，CANCEL 可在任意阶段发送。"""
    header = websocket.headers.get("authorization", "")
    settings = websocket.app.state.server_settings
    if not device_token_is_valid(settings.device_api_token, header):
        logger.warning("WebSocket 语音鉴权失败: client={}", websocket.client)
        await websocket.close(code=4401)
        return
    device_id = (
        websocket.headers.get("x-device-id") or settings.display_default_device_id
    ).strip()
    if not device_id or len(device_id) > 80:
        await websocket.close(code=4400, reason="Invalid X-Device-Id")
        return
    thread_id = websocket.headers.get("x-thread-id")
    if thread_id is not None:
        thread_id = thread_id.strip()
        if not thread_id_is_valid(thread_id):
            await websocket.close(code=4400, reason="Invalid X-Thread-Id")
            return
    await websocket.accept()
    logger.info("WebSocket 语音会话已连接: client={}", websocket.client)
    chunks: list[bytes] = []
    total = 0
    started = False
    try:
        while True:
            message = await websocket.receive()
            if message["type"] == "websocket.disconnect":
                return
            if message.get("bytes") is not None:
                if not started:
                    await _close_protocol_error(websocket, "必须先发送 START")
                    return
                chunk = message["bytes"]
                if len(chunk) % 2 or total + len(chunk) > _MAX_PCM_BYTES:
                    await _close_protocol_error(websocket, "PCM 数据无效或过长")
                    return
                chunks.append(chunk)
                total += len(chunk)
                continue
            try:
                control = json.loads(message.get("text") or "")
            except json.JSONDecodeError:
                await _close_protocol_error(websocket, "控制帧不是 JSON")
                return
            if not started:
                if control != _START:
                    await _close_protocol_error(websocket, "START 参数不支持")
                    return
                started = True
            elif control.get("type") == "cancel":
                await websocket.close(code=1000)
                return
            elif control.get("type") == "end_input" and total:
                logger.info(
                    "WebSocket 上行完成: pcm_bytes={} duration_ms={}",
                    total,
                    total * 1000 // (16_000 * 2),
                )
                await _send_stream(
                    websocket,
                    b"".join(chunks),
                    device_id=device_id,
                    thread_id=thread_id,
                )
                if websocket.client_state == WebSocketState.CONNECTED:
                    await websocket.close(code=1000)
                return
            else:
                await _close_protocol_error(websocket, "控制帧无效")
                return
    except WebSocketDisconnect:
        return
