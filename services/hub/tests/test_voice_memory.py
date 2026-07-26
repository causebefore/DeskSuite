"""VoiceService 记忆接入：读注入 system prompt、写后异步保存、无 memory 时行为不变。"""

from unittest.mock import MagicMock, patch

from app.services.voice_service import VoiceService


def _delta(content=None):
    d = {}
    if content is not None:
        d["content"] = content
    return {"choices": [{"delta": d}]}


def _make_service(memory=None, default_city="苏州"):
    settings = MagicMock()
    settings.zhipu_api_key = "test-key"
    settings.zhipu_llm_model = "glm-4-flash"
    settings.zhipu_asr_model = "glm-asr-2512"
    settings.zhipu_tts_model = "glm-tts"
    settings.zhipu_tts_voice = "female"
    settings.default_city = default_city
    settings.default_timezone = "Asia/Shanghai"
    return VoiceService(settings, memory_service=memory)


class TestReadInjection:
    def test_memory_string_injected_into_system_message(self):
        mem = MagicMock()
        mem.enabled = True
        mem.query_memory.return_value = "用户叫张三"
        svc = _make_service(memory=mem)

        captured = {}

        def fake_stream(messages, tools):
            captured["messages"] = messages
            return iter([_delta(content="你好张三。")])

        with patch.object(svc, "_call_chat_stream", side_effect=fake_stream):
            list(svc._llm_stream("你好", device_id="dev1"))

        assert captured["messages"][0]["role"] == "system"
        assert "用户叫张三" in captured["messages"][0]["content"]
        mem.query_memory.assert_called_once_with("dev1", "你好")

    def test_no_memory_keeps_plain_system_prompt(self):
        svc = _make_service(memory=None)
        captured = {}

        def fake_stream(messages, tools):
            captured["messages"] = messages
            return iter([_delta(content="你好。")])

        with patch.object(svc, "_call_chat_stream", side_effect=fake_stream):
            list(svc._llm_stream("你好", device_id="dev1"))

        assert captured["messages"][0]["content"] == svc._system_prompt


class TestDeferredSave:
    def test_chat_stream_schedules_save_with_device_user_reply(self):
        mem = MagicMock()
        mem.enabled = True
        mem.query_memory.return_value = ""
        svc = _make_service(memory=mem)

        saved = []

        def fake_schedule(dev, u, r):
            saved.append((dev, u, r))

        with patch.object(svc, "_schedule_memory_save", side_effect=fake_schedule), \
             patch.object(svc, "_asr", return_value="我叫张三"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好张三。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01"])):
            list(svc.chat_stream(b"\x00" * 100, device_id="dev1"))

        assert saved, "应触发一次异步保存"
        assert saved[0][0] == "dev1"
        assert saved[0][1] == "我叫张三"
        assert "你好张三" in saved[0][2]

    def test_no_memory_does_not_schedule_save(self):
        svc = _make_service(memory=None)
        with patch.object(svc, "_schedule_memory_save") as sched, \
             patch.object(svc, "_asr", return_value="嗨"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01"])):
            list(svc.chat_stream(b"\x00" * 100, device_id="dev1"))
        sched.assert_not_called()

    def test_disabled_memory_does_not_schedule_save(self):
        mem = MagicMock()
        mem.enabled = False  # 即使有 memory_service，关闭也不保存
        svc = _make_service(memory=mem)
        with patch.object(svc, "_schedule_memory_save") as sched, \
             patch.object(svc, "_asr", return_value="嗨"), \
             patch.object(svc, "_llm_stream", return_value=iter(["你好。"])), \
             patch.object(svc, "_tts_stream", return_value=iter([b"\x00\x01"])):
            list(svc.chat_stream(b"\x00" * 100, device_id="dev1"))
        sched.assert_not_called()
