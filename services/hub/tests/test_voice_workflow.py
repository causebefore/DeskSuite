"""VoiceWorkflow 的纯本地 ASR、Assistant、TTS 编排契约测试。"""

import io
import threading
import time
import wave

import pytest

import app.workflows.voice.workflow as workflow_module
from app.workflows.assistant.context import AssistantEvent, AssistantTurn
from app.workflows.voice.protocol import (
    FRAME_TYPE_ASR_TEXT,
    FRAME_TYPE_END,
    FRAME_TYPE_ERROR,
    FRAME_TYPE_REPLY_TEXT,
    FRAME_TYPE_THINKING,
    FRAME_TYPE_TTS_PCM,
)
from app.workflows.voice.workflow import VoiceWorkflow


class FakeSpeechProvider:
    def __init__(
        self,
        *,
        text: str = "用户原话",
        configured: bool = True,
        transcribe_error: Exception | None = None,
        tts_by_text: dict[str, list[bytes] | Exception] | None = None,
    ) -> None:
        self.configured = configured
        self.text = text
        self.transcribe_error = transcribe_error
        self.tts_by_text = tts_by_text or {}
        self.wav_inputs: list[bytes] = []
        self.tts_calls: list[tuple[str, threading.Event | None]] = []

    def transcribe(self, wav_bytes: bytes) -> str:
        self.wav_inputs.append(wav_bytes)
        if self.transcribe_error is not None:
            raise self.transcribe_error
        return self.text

    def synthesize_stream(
        self,
        text: str,
        cancel_event: threading.Event | None = None,
    ):
        self.tts_calls.append((text, cancel_event))
        configured = self.tts_by_text.get(text, [b"\x00\x00"])
        if isinstance(configured, Exception):
            raise configured
        yield from configured


class FakeAssistantWorkflow:
    def __init__(
        self,
        events: list[AssistantEvent] | None = None,
        *,
        error: BaseException | None = None,
        delay_seconds: float = 0,
    ) -> None:
        self.events = events or []
        self.error = error
        self.delay_seconds = delay_seconds
        self.calls: list[tuple[AssistantTurn, threading.Event | None]] = []

    def stream(
        self,
        turn: AssistantTurn,
        *,
        cancel_event: threading.Event | None = None,
    ):
        self.calls.append((turn, cancel_event))
        if self.delay_seconds:
            time.sleep(self.delay_seconds)
        if self.error is not None:
            raise self.error
        yield from self.events


def _event(text: str) -> AssistantEvent:
    return AssistantEvent(type="text_delta", text=text)


def _tool_event(event_type: str) -> AssistantEvent:
    return AssistantEvent(
        type=event_type,
        tool_name="web_search_prime",
        tool_call_id="search-call-1",
    )


def test_normal_flow_reuses_assistant_and_preserves_frame_order():
    speech = FakeSpeechProvider(
        text="请介绍一下",
        tts_by_text={
            "第一句。": [b"\x01\x00"],
            "第二句！": [b"\x02\x00"],
            "最后一句": [b"\x03\x00"],
        },
    )
    assistant = FakeAssistantWorkflow(
        [
            _event("第一句。第二"),
            _event("句！最后一句"),
            AssistantEvent(type="final", text="该事件不应重复输出"),
        ]
    )
    workflow = VoiceWorkflow(speech, assistant)
    cancel_event = threading.Event()

    frames = list(
        workflow.chat_stream(
            b"\x01\x00\x02\x00",
            sample_rate=16_000,
            cancel_event=cancel_event,
            device_id="desk-1",
            thread_id="shared:main",
        )
    )

    assert frames == [
        (FRAME_TYPE_ASR_TEXT, "请介绍一下".encode()),
        (FRAME_TYPE_REPLY_TEXT, "第一句。".encode()),
        (FRAME_TYPE_TTS_PCM, b"\x01\x00"),
        (FRAME_TYPE_REPLY_TEXT, "第二句！".encode()),
        (FRAME_TYPE_TTS_PCM, b"\x02\x00"),
        (FRAME_TYPE_REPLY_TEXT, "最后一句".encode()),
        (FRAME_TYPE_TTS_PCM, b"\x03\x00"),
        (FRAME_TYPE_END, b""),
    ]
    assert assistant.calls[0][0] == AssistantTurn(
        text="请介绍一下",
        thread_id="shared:main",
        channel="voice",
        device_id="desk-1",
    )
    assert assistant.calls[0][1] is not cancel_event
    assert assistant.calls[0][1].is_set()
    assert [text for text, _ in speech.tts_calls] == [
        "第一句。",
        "第二句！",
        "最后一句",
    ]


def test_voice_diagnostic_logs_identify_asr_assistant_and_tts_boundaries(
    monkeypatch,
):
    recorded: list[str] = []

    class RecordingLogger:
        def info(self, message: str, *args) -> None:
            recorded.append(message.format(*args))

        def warning(self, message: str, *args) -> None:
            recorded.append(message.format(*args))

    monkeypatch.setattr(workflow_module, "logger", RecordingLogger())
    workflow = VoiceWorkflow(
        FakeSpeechProvider(
            text="用户问天气",
            tts_by_text={"模型回答。": [b"\x00\x00"]},
        ),
        FakeAssistantWorkflow(
            [
                _tool_event("tool_started"),
                _tool_event("tool_finished"),
                _event("模型回答。"),
            ]
        ),
    )

    list(
        workflow.chat_stream(
            b"\x00\x00",
            sample_rate=16_000,
            device_id="desk-log",
            thread_id="voice:test-log",
        )
    )

    assert any("ASR 识别完成" in entry and "用户问天气" in entry for entry in recorded)
    assert any("Assistant 生成开始" in entry and "用户问天气" in entry for entry in recorded)
    assert any("Assistant 回复完成" in entry and "模型回答。" in entry for entry in recorded)
    assert any("TTS 合成开始" in entry and "模型回答。" in entry for entry in recorded)
    assert any(
        "语音 Assistant 工具事件" in entry
        and "event=tool_started" in entry
        and "tool=web_search_prime" in entry
        for entry in recorded
    )
    assert all("desk-log" in entry and "voice:test-log" in entry for entry in recorded)


def test_tool_lifecycle_emits_immediate_thinking_without_entering_tts():
    speech = FakeSpeechProvider(
        text="搜索美团今天",
        tts_by_text={"搜索结果。": [b"\x01\x00"]},
    )
    assistant = FakeAssistantWorkflow(
        [
            _tool_event("tool_started"),
            _tool_event("tool_finished"),
            _event("搜索结果。"),
        ]
    )

    frames = list(
        VoiceWorkflow(speech, assistant).chat_stream(
            b"\x00\x00",
            sample_rate=16_000,
        )
    )

    assert frames == [
        (FRAME_TYPE_ASR_TEXT, "搜索美团今天".encode()),
        (FRAME_TYPE_THINKING, b""),
        (FRAME_TYPE_REPLY_TEXT, "搜索结果。".encode()),
        (FRAME_TYPE_TTS_PCM, b"\x01\x00"),
        (FRAME_TYPE_END, b""),
    ]
    assert [text for text, _ in speech.tts_calls] == ["搜索结果。"]


def test_native_16k_pcm_is_wrapped_as_mono_wav_without_resampling():
    pcm = b"\x01\x00\x02\x00\x03\x00"
    speech = FakeSpeechProvider()
    workflow = VoiceWorkflow(speech, FakeAssistantWorkflow([_event("收到。")]))

    list(workflow.chat_stream(pcm, sample_rate=16_000, device_id="desk-1"))

    with wave.open(io.BytesIO(speech.wav_inputs[0]), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        assert wav.getframerate() == 16_000
        assert wav.readframes(wav.getnframes()) == pcm


def test_non_16k_pcm_is_resampled_before_asr():
    # 24kHz 下 6 个样本应得到 16kHz 下 4 个样本。
    pcm_24k = b"\x00\x00\x10\x00\x20\x00\x30\x00\x40\x00\x50\x00"
    speech = FakeSpeechProvider()
    workflow = VoiceWorkflow(speech, FakeAssistantWorkflow([_event("收到。")]))

    list(workflow.chat_stream(pcm_24k, sample_rate=24_000))

    with wave.open(io.BytesIO(speech.wav_inputs[0]), "rb") as wav:
        assert wav.getframerate() == 16_000
        assert wav.getnframes() == 4


def test_missing_explicit_thread_uses_stable_voice_device_thread():
    speech = FakeSpeechProvider()
    assistant = FakeAssistantWorkflow([_event("收到。")])
    workflow = VoiceWorkflow(speech, assistant)

    list(workflow.chat_stream(b"\x00\x00", sample_rate=16_000, device_id="desk-1"))
    list(workflow.chat_stream(b"\x00\x00", sample_rate=16_000, device_id="desk-1"))

    assert [turn.thread_id for turn, _ in assistant.calls] == [
        "voice:desk-1",
        "voice:desk-1",
    ]


def test_unconfigured_speech_provider_returns_error_and_end_without_work():
    speech = FakeSpeechProvider(configured=False)
    assistant = FakeAssistantWorkflow([_event("不应调用")])

    frames = list(VoiceWorkflow(speech, assistant).chat_stream(b"\x00\x00"))

    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]
    assert speech.wav_inputs == []
    assert assistant.calls == []


def test_asr_failure_returns_error_and_end_without_assistant_call():
    speech = FakeSpeechProvider(transcribe_error=RuntimeError("bad audio"))
    assistant = FakeAssistantWorkflow([_event("不应调用")])

    frames = list(VoiceWorkflow(speech, assistant).chat_stream(b"\x00\x00"))

    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]
    assert "语音识别失败" in frames[0][1].decode()
    assert assistant.calls == []


def test_assistant_failure_returns_error_then_end():
    workflow = VoiceWorkflow(
        FakeSpeechProvider(),
        FakeAssistantWorkflow(error=RuntimeError("model failed")),
    )

    frames = list(workflow.chat_stream(b"\x00\x00", sample_rate=16_000))

    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ASR_TEXT,
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]
    assert "回复生成失败" in frames[-2][1].decode()


def test_slow_assistant_emits_thinking_heartbeat(monkeypatch):
    monkeypatch.setattr(
        workflow_module,
        "_THINKING_PING_INTERVAL_SECONDS",
        0.01,
    )
    workflow = VoiceWorkflow(
        FakeSpeechProvider(),
        FakeAssistantWorkflow([_event("完成。")], delay_seconds=0.04),
    )

    frames = list(workflow.chat_stream(b"\x00\x00", sample_rate=16_000))

    frame_types = [frame_type for frame_type, _ in frames]
    assert FRAME_TYPE_THINKING in frame_types
    assert frame_types[-1] == FRAME_TYPE_END


def test_tts_odd_chunks_are_reassembled_and_frames_stay_even_and_bounded():
    audio = b"a" * 16_385 + b"b"
    speech = FakeSpeechProvider(tts_by_text={"长句。": [b"a" * 16_385, b"b"]})
    workflow = VoiceWorkflow(speech, FakeAssistantWorkflow([_event("长句。")]))

    frames = list(workflow.chat_stream(b"\x00\x00", sample_rate=16_000))
    pcm_frames = [payload for frame_type, payload in frames if frame_type == FRAME_TYPE_TTS_PCM]

    assert b"".join(pcm_frames) == audio
    assert all(len(payload) % 2 == 0 for payload in pcm_frames)
    assert all(len(payload) <= 16_384 for payload in pcm_frames)


def test_tts_ending_with_half_sample_emits_error_and_end():
    speech = FakeSpeechProvider(tts_by_text={"坏音频。": [b"\x01"]})
    workflow = VoiceWorkflow(speech, FakeAssistantWorkflow([_event("坏音频。")]))

    frames = list(workflow.chat_stream(b"\x00\x00", sample_rate=16_000))

    assert [frame_type for frame_type, _ in frames] == [
        FRAME_TYPE_ASR_TEXT,
        FRAME_TYPE_REPLY_TEXT,
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]
    assert "半个 16-bit 样本" in frames[-2][1].decode()


def test_tts_failure_cancels_assistant_worker():
    class WaitingAssistant:
        def __init__(self) -> None:
            self.stopped = threading.Event()

        def stream(self, turn, *, cancel_event=None):
            yield _event("第一句。")
            while not cancel_event.is_set():
                time.sleep(0.005)
            self.stopped.set()

    assistant = WaitingAssistant()
    speech = FakeSpeechProvider(
        tts_by_text={"第一句。": RuntimeError("tts failed")}
    )

    frames = list(
        VoiceWorkflow(speech, assistant).chat_stream(
            b"\x00\x00",
            sample_rate=16_000,
        )
    )

    assert [frame_type for frame_type, _ in frames][-2:] == [
        FRAME_TYPE_ERROR,
        FRAME_TYPE_END,
    ]
    assert assistant.stopped.wait(timeout=1)


def test_cancel_after_asr_stops_before_assistant_and_end():
    cancel_event = threading.Event()
    assistant = FakeAssistantWorkflow([_event("不应调用")])
    stream = VoiceWorkflow(FakeSpeechProvider(), assistant).chat_stream(
        b"\x00\x00",
        sample_rate=16_000,
        cancel_event=cancel_event,
    )

    assert next(stream)[0] == FRAME_TYPE_ASR_TEXT
    cancel_event.set()

    assert list(stream) == []
    assert assistant.calls == []


def test_cancel_while_tool_is_running_stops_worker_without_tts_or_end():
    class WaitingToolAssistant:
        def __init__(self) -> None:
            self.started = threading.Event()
            self.stopped = threading.Event()

        def stream(self, turn, *, cancel_event=None):
            del turn
            self.started.set()
            yield _tool_event("tool_started")
            while not cancel_event.is_set():
                time.sleep(0.005)
            self.stopped.set()

    cancel_event = threading.Event()
    assistant = WaitingToolAssistant()
    speech = FakeSpeechProvider(text="搜索今天新闻")
    stream = VoiceWorkflow(speech, assistant).chat_stream(
        b"\x00\x00",
        sample_rate=16_000,
        cancel_event=cancel_event,
    )

    assert next(stream)[0] == FRAME_TYPE_ASR_TEXT
    assert next(stream) == (FRAME_TYPE_THINKING, b"")
    assert assistant.started.is_set()

    cancel_event.set()

    assert list(stream) == []
    assert assistant.stopped.wait(timeout=1)
    assert speech.tts_calls == []


@pytest.mark.parametrize("device_id", [None, ""])
def test_missing_device_id_uses_default_voice_thread(device_id: str | None):
    assistant = FakeAssistantWorkflow([_event("收到。")])

    list(
        VoiceWorkflow(FakeSpeechProvider(), assistant).chat_stream(
            b"\x00\x00",
            sample_rate=16_000,
            device_id=device_id,
        )
    )

    assert assistant.calls[0][0].thread_id == "voice:default"
