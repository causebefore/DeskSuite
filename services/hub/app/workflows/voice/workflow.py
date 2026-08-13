"""设备语音输入到共享 Assistant 再到语音帧的编排。"""

import io
import queue
import struct
import threading
import wave
from collections.abc import Iterator

from loguru import logger

from app.workflows.assistant.context import AssistantTurn
from app.workflows.voice.protocol import (
    FRAME_TYPE_ASR_TEXT,
    FRAME_TYPE_END,
    FRAME_TYPE_ERROR,
    FRAME_TYPE_REPLY_TEXT,
    FRAME_TYPE_THINKING,
    FRAME_TYPE_TTS_PCM,
    SentenceSplitter,
)


_TTS_FRAME_CHUNK_SIZE = 16_384
_THINKING_PING_INTERVAL_SECONDS = 3.0
_QUEUE_END = object()


class VoiceWorkflow:
    """保留设备帧协议，同时复用 Assistant 的工具、记忆与多轮状态。"""

    def __init__(self, speech_provider, assistant_workflow) -> None:
        self._speech = speech_provider
        self._assistant = assistant_workflow

    def chat_stream(
        self,
        pcm_data: bytes,
        sample_rate: int = 24_000,
        cancel_event: threading.Event | None = None,
        device_id: str | None = None,
        thread_id: str | None = None,
    ) -> Iterator[tuple[int, bytes]]:
        """执行 ASR、Assistant、TTS，并逐个返回既有二进制帧。"""
        selected_device = device_id or "default"
        selected_thread = (thread_id or f"voice:{selected_device}").strip()
        logger.info(
            "语音回合开始: device_id={!r} thread_id={!r} pcm_bytes={} sample_rate={}",
            selected_device,
            selected_thread,
            len(pcm_data),
            sample_rate,
        )
        if not self._speech.configured:
            logger.warning(
                "语音回合不可用: device_id={!r} thread_id={!r} reason=未配置语音供应商",
                selected_device,
                selected_thread,
            )
            yield FRAME_TYPE_ERROR, "智谱 API Key 未配置".encode("utf-8")
            yield FRAME_TYPE_END, b""
            return

        stop_event = threading.Event()

        def externally_cancelled() -> bool:
            return cancel_event is not None and cancel_event.is_set()

        def cancelled() -> bool:
            if externally_cancelled():
                stop_event.set()
            return stop_event.is_set()

        try:
            pcm_16k = (
                pcm_data
                if sample_rate == 16_000
                else _resample_pcm(pcm_data, sample_rate, 16_000)
            )
            user_text = self._speech.transcribe(_pcm_to_wav(pcm_16k, 16_000))
            logger.info(
                "ASR 识别完成: device_id={!r} thread_id={!r} chars={} text={!r}",
                selected_device,
                selected_thread,
                len(user_text),
                user_text,
            )
        except Exception as exc:  # noqa: BLE001
            logger.warning(
                "语音识别失败: device_id={!r} thread_id={!r} error={}",
                selected_device,
                selected_thread,
                exc,
            )
            yield FRAME_TYPE_ERROR, f"语音识别失败: {exc}".encode("utf-8")
            yield FRAME_TYPE_END, b""
            return

        if cancelled():
            return
        yield FRAME_TYPE_ASR_TEXT, user_text.encode("utf-8")
        # 消费者可能在收到识别文本后立即取消；此时不得再启动一次模型调用。
        if cancelled():
            return

        pending: queue.Queue[tuple[str, object]] = queue.Queue(maxsize=64)
        worker_error: list[BaseException | None] = [None]

        def enqueue(kind: str, value: object) -> bool:
            while not cancelled():
                try:
                    pending.put((kind, value), timeout=0.25)
                    return True
                except queue.Full:
                    continue
            return False

        def assistant_worker() -> None:
            try:
                logger.info(
                    "Assistant 生成开始: device_id={!r} thread_id={!r} input={!r}",
                    selected_device,
                    selected_thread,
                    user_text,
                )
                turn = AssistantTurn(
                    text=user_text,
                    thread_id=selected_thread,
                    channel="voice",
                    device_id=device_id,
                )
                reply_parts: list[str] = []
                for event in self._assistant.stream(turn, cancel_event=stop_event):
                    if event.type == "text_delta":
                        reply_parts.append(event.text)
                        if not enqueue("text", event.text):
                            return
                    elif event.type in {
                        "tool_started",
                        "tool_finished",
                        "tool_error",
                    }:
                        if not enqueue("assistant_event", event):
                            return
                reply_text = "".join(reply_parts).strip()
                if cancelled():
                    logger.info(
                        "Assistant 生成已取消: device_id={!r} thread_id={!r} partial={!r}",
                        selected_device,
                        selected_thread,
                        reply_text,
                    )
                else:
                    logger.info(
                        "Assistant 回复完成: device_id={!r} thread_id={!r} chars={} text={!r}",
                        selected_device,
                        selected_thread,
                        len(reply_text),
                        reply_text,
                    )
            except BaseException as exc:  # noqa: BLE001
                worker_error[0] = exc
            finally:
                enqueue("done", _QUEUE_END)

        threading.Thread(
            target=assistant_worker,
            name="voice-assistant",
            daemon=True,
        ).start()

        try:
            splitter = SentenceSplitter()
            try:
                while True:
                    if cancelled():
                        return
                    try:
                        kind, value = pending.get(
                            timeout=_THINKING_PING_INTERVAL_SECONDS
                        )
                    except queue.Empty:
                        yield FRAME_TYPE_THINKING, b""
                        continue
                    if kind == "done":
                        break
                    if kind == "assistant_event":
                        event = value
                        log = (
                            logger.warning
                            if event.type == "tool_error"
                            else logger.info
                        )
                        log(
                            "语音 Assistant 工具事件: device_id={!r} thread_id={!r} "
                            "event={} tool={} call_id={}",
                            selected_device,
                            selected_thread,
                            event.type,
                            event.tool_name or "unknown",
                            event.tool_call_id or "-",
                        )
                        if event.type == "tool_started":
                            yield FRAME_TYPE_THINKING, b""
                        continue
                    for sentence in splitter.feed(str(value)):
                        yield FRAME_TYPE_REPLY_TEXT, sentence.encode("utf-8")
                        yield from self._iter_tts_pcm_frames(
                            sentence,
                            stop_event,
                            device_id=selected_device,
                            thread_id=selected_thread,
                        )

                rest = splitter.flush()
                if rest:
                    yield FRAME_TYPE_REPLY_TEXT, rest.encode("utf-8")
                    yield from self._iter_tts_pcm_frames(
                        rest,
                        stop_event,
                        device_id=selected_device,
                        thread_id=selected_thread,
                    )
                if worker_error[0] is not None:
                    raise worker_error[0]
            except Exception as exc:  # noqa: BLE001
                stop_event.set()
                logger.warning(
                    "语音回复生成失败: device_id={!r} thread_id={!r} error={}",
                    selected_device,
                    selected_thread,
                    exc,
                )
                if not externally_cancelled():
                    yield FRAME_TYPE_ERROR, f"回复生成失败: {exc}".encode("utf-8")

            if not externally_cancelled():
                yield FRAME_TYPE_END, b""
        finally:
            # StreamingResponse 被中途关闭时也要取消后台 Agent 和 TTS 连接。
            stop_event.set()

    def _iter_tts_pcm_frames(
        self,
        text: str,
        cancel_event: threading.Event | None = None,
        *,
        device_id: str,
        thread_id: str,
    ) -> Iterator[tuple[int, bytes]]:
        """合并半个样本，并将 PCM 拆成设备可消费的小帧。"""
        logger.info(
            "TTS 合成开始: device_id={!r} thread_id={!r} chars={} text={!r}",
            device_id,
            thread_id,
            len(text),
            text,
        )
        carry = b""
        pcm_bytes = 0
        for chunk in self._speech.synthesize_stream(
            text,
            cancel_event=cancel_event,
        ):
            if cancel_event is not None and cancel_event.is_set():
                return
            data = carry + chunk
            even_length = len(data) & ~1
            for offset in range(0, even_length, _TTS_FRAME_CHUNK_SIZE):
                frame = data[offset : min(offset + _TTS_FRAME_CHUNK_SIZE, even_length)]
                if frame:
                    pcm_bytes += len(frame)
                    yield FRAME_TYPE_TTS_PCM, frame
            carry = data[even_length:]
        if carry:
            raise RuntimeError("TTS PCM 以半个 16-bit 样本结束")
        logger.info(
            "TTS 合成完成: device_id={!r} thread_id={!r} pcm_bytes={}",
            device_id,
            thread_id,
            pcm_bytes,
        )


def _resample_pcm(pcm: bytes, source_rate: int, target_rate: int) -> bytes:
    """用线性插值重采样 16-bit 单声道 PCM。"""
    if source_rate == target_rate:
        return pcm
    source_count = len(pcm) // 2
    source = struct.unpack(f"<{source_count}h", pcm)
    target_count = round(source_count * target_rate / source_rate)
    target = bytearray(target_count * 2)
    for index in range(target_count):
        source_position = index * source_rate / target_rate
        left_index = int(source_position)
        fraction = source_position - left_index
        left = source[left_index] if left_index < source_count else 0
        right = source[left_index + 1] if left_index + 1 < source_count else left
        value = int(left + (right - left) * fraction)
        struct.pack_into("<h", target, index * 2, max(-32_768, min(32_767, value)))
    return bytes(target)


def _pcm_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    """把 16-bit 单声道 PCM 封装为 WAV。"""
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return buffer.getvalue()
