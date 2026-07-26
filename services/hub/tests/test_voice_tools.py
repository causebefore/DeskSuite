"""VoiceService 工具调用（function calling）单元测试。

验证 LLM 通过 tools 调用 get_weather 后，能读取真实天气 JSON 数据
并组织口语回复。覆盖同步 _llm、流式 _llm_stream 以及天气摘要格式。
"""

import json
from unittest.mock import MagicMock, patch

import pytest

from app.services.voice_service import VoiceService, _THINKING


def _delta(content=None, tool_calls=None):
    """构造智谱流式 SSE 的 data 字典。"""
    d = {}
    if content is not None:
        d["content"] = content
    if tool_calls is not None:
        d["tool_calls"] = tool_calls
    return {"choices": [{"delta": d}]}


def _tc(index, tc_id=None, name=None, args=None):
    """构造流式 tool_calls 分片。"""
    d = {"index": index}
    if tc_id is not None:
        d["id"] = tc_id
    fn = {}
    if name is not None:
        fn["name"] = name
    if args is not None:
        fn["arguments"] = args
    if fn:
        d["function"] = fn
    return d


def _make_service(weather=None, calendar=None, mail=None, quota=None, default_city="苏州"):
    settings = MagicMock()
    settings.zhipu_api_key = "test-key"
    settings.zhipu_llm_model = "glm-4-flash"
    settings.zhipu_asr_model = "glm-asr-2512"
    settings.zhipu_tts_model = "glm-tts"
    settings.zhipu_tts_voice = "female"
    settings.default_city = default_city
    settings.default_timezone = "Asia/Shanghai"
    return VoiceService(settings, weather_service=weather, calendar_service=calendar, mail_service=mail, quota_service=quota)


def _make_weather(city="苏州", temp=26):
    weather = MagicMock()
    weather.location.city = city
    weather.now.temp_c = temp
    weather.now.feels_like_c = 28
    weather.now.text = "晴"
    weather.now.humidity_percent = 55
    weather.now.wind_dir = "东南风"
    weather.now.wind_scale = "3"
    weather.daily.items = []
    weather.minutely.summary = "未来 2 小时暂无降水"
    weather.air = None
    weather.error = ""
    return weather


def _make_calendar(events=None, error="", range_days=7):
    cal = MagicMock()
    cal.source = "icloud"
    cal.error = error
    cal.range_days = range_days
    cal.items = events or []
    return cal


def _make_cal_event(title="团队周会", relative="今天 10:00", location="会议室A"):
    ev = MagicMock()
    ev.title = title
    ev.relative = relative
    ev.location = location
    return ev


def _make_mail(unread=3, messages=None, error=""):
    mail = MagicMock()
    mail.source = "qq-imap"
    mail.error = error
    mail.unread_count = unread
    mail.messages = messages or []
    return mail


def _make_mail_msg(date_text="07-08 09:00", sender="张三", subject="项目周报", unread=True):
    msg = MagicMock()
    msg.date_text = date_text
    msg.from_name = sender
    msg.subject = subject
    msg.unread = unread
    return msg


def _make_quota(level="VIP", limits=None, available=True, error=None):
    quota = MagicMock()
    quota.available = available
    quota.level = level if available else None
    quota.error = error
    quota.limits = limits or []
    return quota


def _make_quota_item(type_name="GLM-4-FLASH_TOKEN", used=45.0, remaining=55.0, reset="2026-07-09 00:00"):
    item = MagicMock()
    item.type = type_name
    item.used_percent = used
    item.remaining_percent = remaining
    item.next_reset = reset
    return item


class TestLlmWithTools:
    def test_glm47_disables_thinking_for_voice_latency(self):
        svc = _make_service(weather=None, calendar=None, mail=None, quota=None)
        svc._llm_model = "glm-4.7"
        response = {"choices": [{"message": {"content": "你好"}}]}
        with patch("app.services.voice_service._zhipu_post", return_value=response) as post:
            result = svc._call_chat_json([{"role": "user", "content": "你好"}])

        payload = json.loads(post.call_args.args[1])
        assert payload["thinking"] == {"type": "disabled"}
        assert result["content"] == "你好"

    def test_no_weather_no_tools(self):
        svc = _make_service(weather=None, calendar=None, mail=None, quota=None)
        assert svc._tools is None

    def test_weather_injected_registers_tool(self):
        svc = _make_service(weather=MagicMock(), calendar=None, mail=None, quota=None)
        assert svc._tools is not None
        assert svc._tools[0]["function"]["name"] == "get_weather"

    def test_all_four_tools_registered(self):
        svc = _make_service(
            weather=MagicMock(), calendar=MagicMock(), mail=MagicMock(), quota=MagicMock()
        )
        names = [t["function"]["name"] for t in svc._tools]
        assert names == ["get_weather", "get_calendar", "get_mail", "get_quota"]

    def test_sync_llm_calls_weather_tool(self):
        weather_svc = MagicMock()
        weather_svc.get_current_weather.return_value = _make_weather()
        svc = _make_service(weather=weather_svc, calendar=None, mail=None, quota=None)

        responses = [
            {"tool_calls": [{"id": "c1", "function": {"name": "get_weather", "arguments": json.dumps({"city": "苏州"})}}]},
            {"content": "苏州现在 26 度，晴天。"},
        ]
        with patch.object(svc, "_call_chat_json", side_effect=responses):
            result = svc._llm("苏州今天天气怎么样")

        assert result == "苏州现在 26 度，晴天。"
        weather_svc.get_current_weather.assert_called_once_with("苏州")

    def test_sync_llm_no_tool_returns_directly(self):
        svc = _make_service(weather=MagicMock(), calendar=None, mail=None, quota=None)
        with patch.object(svc, "_call_chat_json", return_value={"content": "你好呀。"}):
            result = svc._llm("你好")
        assert result == "你好呀。"

    def test_sync_llm_default_city_when_no_arg(self):
        weather_svc = MagicMock()
        weather_svc.get_current_weather.return_value = _make_weather()
        svc = _make_service(weather=weather_svc, calendar=None, mail=None, quota=None, default_city="苏州")

        responses = [
            {"tool_calls": [{"id": "c1", "function": {"name": "get_weather", "arguments": "{}"}}]},
            {"content": "晴天。"},
        ]
        with patch.object(svc, "_call_chat_json", side_effect=responses):
            svc._llm("今天天气")
        weather_svc.get_current_weather.assert_called_once_with("苏州")

    def test_sync_llm_calls_calendar_tool(self):
        cal_svc = MagicMock()
        cal_svc.get_upcoming_events.return_value = _make_calendar(
            events=[_make_cal_event()]
        )
        svc = _make_service(weather=None, calendar=cal_svc, mail=None, quota=None)
        responses = [
            {"tool_calls": [{"id": "c1", "function": {"name": "get_calendar", "arguments": "{}"}}]},
            {"content": "今天 10 点有团队周会。"},
        ]
        with patch.object(svc, "_call_chat_json", side_effect=responses):
            result = svc._llm("今天有什么安排")
        assert result == "今天 10 点有团队周会。"
        cal_svc.get_upcoming_events.assert_called_once_with("Asia/Shanghai")

    def test_sync_llm_calls_mail_tool(self):
        mail_svc = MagicMock()
        mail_svc.get_mail_summary.return_value = _make_mail(
            messages=[_make_mail_msg()]
        )
        svc = _make_service(weather=None, calendar=None, mail=mail_svc, quota=None)
        responses = [
            {"tool_calls": [{"id": "c1", "function": {"name": "get_mail", "arguments": "{}"}}]},
            {"content": "你有 3 封未读邮件。"},
        ]
        with patch.object(svc, "_call_chat_json", side_effect=responses):
            result = svc._llm("有没有新邮件")
        assert result == "你有 3 封未读邮件。"
        mail_svc.get_mail_summary.assert_called_once_with("Asia/Shanghai")

    def test_sync_llm_calls_quota_tool(self):
        quota_svc = MagicMock()
        quota_svc.check_glm.return_value = _make_quota(
            limits=[_make_quota_item()]
        )
        svc = _make_service(weather=None, calendar=None, mail=None, quota=quota_svc)
        responses = [
            {"tool_calls": [{"id": "c1", "function": {"name": "get_quota", "arguments": "{}"}}]},
            {"content": "GLM-4-FLASH 已用 45%。"},
        ]
        with patch.object(svc, "_call_chat_json", side_effect=responses):
            result = svc._llm("还剩多少额度")
        assert result == "GLM-4-FLASH 已用 45%。"
        quota_svc.check_glm.assert_called_once()


class TestLlmStreamWithTools:
    def test_tool_round_limit_raises_instead_of_silent_end(self):
        weather_svc = MagicMock()
        weather_svc.get_current_weather.return_value = _make_weather()
        svc = _make_service(weather=weather_svc, calendar=None, mail=None, quota=None)
        tool_round = [
            _delta(tool_calls=[_tc(0, tc_id="c1", name="get_weather", args="{}")])
        ]
        with patch.object(svc, "_call_chat_stream", side_effect=lambda *_: iter(tool_round)):
            with pytest.raises(RuntimeError, match="超过最大轮数"):
                list(svc._llm_stream("天气"))

    def test_stream_tool_call_then_reply(self):
        weather_svc = MagicMock()
        weather_svc.get_current_weather.return_value = _make_weather()
        svc = _make_service(weather=weather_svc, calendar=None, mail=None, quota=None)

        args_full = json.dumps({"city": "苏州"})
        round1 = [_delta(tool_calls=[_tc(0, tc_id="c1", name="get_weather", args=args_full)])]
        round2 = [_delta(content="苏州"), _delta(content="26度。")]

        count = [0]

        def fake_stream(messages, tools):
            count[0] += 1
            return iter(round1 if count[0] == 1 else round2)

        with patch.object(svc, "_call_chat_stream", side_effect=fake_stream):
           tokens = list(svc._llm_stream("苏州天气"))

        assert tokens == [_THINKING, "苏州", "26度。"]
        weather_svc.get_current_weather.assert_called_once_with("苏州")

    def test_stream_no_tool_yields_text(self):
        svc = _make_service(weather=MagicMock(), calendar=None, mail=None, quota=None)
        stream_data = [_delta(content="你好"), _delta(content="呀。")]
        with patch.object(svc, "_call_chat_stream", return_value=iter(stream_data)):
            tokens = list(svc._llm_stream("你好"))
        assert tokens == ["你好", "呀。"]

    def test_stream_tool_calls_split_across_chunks(self):
        weather_svc = MagicMock()
        weather_svc.get_current_weather.return_value = _make_weather()
        svc = _make_service(weather=weather_svc, calendar=None, mail=None, quota=None)

        round1 = [
            _delta(tool_calls=[_tc(0, tc_id="c1", name="get_weather", args='{"ci')]),
            _delta(tool_calls=[_tc(0, args='ty":"北京"}')]),
        ]
        round2 = [_delta(content="北京晴。")]

        count = [0]

        def fake_stream(messages, tools):
            count[0] += 1
            return iter(round1 if count[0] == 1 else round2)

        with patch.object(svc, "_call_chat_stream", side_effect=fake_stream):
           tokens = list(svc._llm_stream("北京天气"))

        assert tokens == [_THINKING, "北京晴。"]
        weather_svc.get_current_weather.assert_called_once_with("北京")


class TestWeatherToBrief:
    def test_brief_includes_current_weather(self):
        weather = _make_weather(city="北京", temp=30)
        brief = VoiceService._weather_to_brief(weather)
        assert "北京" in brief
        assert "30°C" in brief
        assert "晴" in brief
        assert "湿度55%" in brief
        assert "东南风3级" in brief

    def test_brief_includes_forecast(self):
        weather = _make_weather()
        item = MagicMock()
        item.fx_date = "2026-07-09"
        item.text_day = "多云"
        item.text_night = "阴"
        item.temp_min_c = 22
        item.temp_max_c = 30
        weather.daily.items = [item]
        brief = VoiceService._weather_to_brief(weather)
        assert "2026-07-09" in brief
        assert "多云" in brief
        assert "22~30°C" in brief

    def test_brief_includes_air_quality(self):
        weather = _make_weather()
        weather.air = MagicMock()
        weather.air.category = "良"
        weather.air.aqi = 78
        brief = VoiceService._weather_to_brief(weather)
        assert "良" in brief
        assert "AQI 78" in brief

    def test_execute_tool_unknown_returns_message(self):
        svc = _make_service(weather=MagicMock(), calendar=None, mail=None, quota=None)
        result = svc._execute_tool("nonexistent", {})
        assert "未知工具" in result


class TestCalendarToBrief:
    def test_empty_calendar(self):
        cal = _make_calendar(events=[])
        brief = VoiceService._calendar_to_brief(cal)
        assert "暂无日程" in brief

    def test_calendar_with_events(self):
        cal = _make_calendar(events=[
            _make_cal_event(title="周会", relative="今天 10:00", location="会议室"),
            _make_cal_event(title="午餐", relative="明天 12:00", location=""),
        ])
        brief = VoiceService._calendar_to_brief(cal)
        assert "2 条日程" in brief
        assert "今天 10:00" in brief
        assert "周会" in brief
        assert "会议室" in brief
        assert "明天 12:00" in brief
        assert "午餐" in brief

    def test_calendar_error(self):
        cal = _make_calendar(error="iCloud 请求失败")
        brief = VoiceService._calendar_to_brief(cal)
        assert "iCloud 请求失败" in brief


class TestMailToBrief:
    def test_empty_mailbox(self):
        mail = _make_mail(unread=0, messages=[])
        brief = VoiceService._mail_to_brief(mail)
        assert "未读邮件 0 封" in brief

    def test_mail_with_messages(self):
        mail = _make_mail(unread=2, messages=[
            _make_mail_msg(date_text="07-08 09:00", sender="张三", subject="周报", unread=True),
            _make_mail_msg(date_text="07-07 18:00", sender="李四", subject="通知", unread=False),
        ])
        brief = VoiceService._mail_to_brief(mail)
        assert "未读邮件 2 封" in brief
        assert "张三" in brief
        assert "周报" in brief
        assert "未读" in brief
        assert "李四" in brief

    def test_mail_error(self):
        mail = _make_mail(error="IMAP 请求失败")
        brief = VoiceService._mail_to_brief(mail)
        assert "IMAP 请求失败" in brief


class TestQuotaToBrief:
    def test_available_with_limits(self):
        quota = _make_quota(level="VIP", limits=[_make_quota_item(used=45.0, remaining=55.0)])
        brief = VoiceService._quota_to_brief(quota)
        assert "VIP" in brief
        assert "已用 45%" in brief
        assert "剩余 55%" in brief
        assert "重置" in brief

    def test_available_no_limits(self):
        quota = _make_quota(limits=[])
        brief = VoiceService._quota_to_brief(quota)
        assert "暂无额度明细" in brief

    def test_unavailable(self):
        quota = _make_quota(available=False, error="网络错误")
        brief = VoiceService._quota_to_brief(quota)
        assert "网络错误" in brief
