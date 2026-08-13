"""Assistant 四个本地只读工具的注册、入参和摘要格式测试。"""

from unittest.mock import MagicMock

from app.schemas.calendar import CalendarEvent, CalendarPayload
from app.schemas.mail import MailMessage, MailPayload
from app.schemas.quota import ProviderQuota, QuotaItem
from app.schemas.weather import (
    DailyForecast,
    DailyForecastItem,
    MinutelyRain,
    WeatherAir,
    WeatherLocation,
    WeatherNow,
    WeatherPayload,
)
from app.workflows.assistant.tools import build_local_tools


def _tool_map(**services):
    return {
        item.name: item
        for item in build_local_tools(
            default_city="苏州",
            default_timezone="Asia/Shanghai",
            **services,
        )
    }


def test_registers_only_injected_local_services():
    tools = _tool_map(
        weather_service=MagicMock(),
        calendar_service=MagicMock(),
        mail_service=MagicMock(),
        quota_service=MagicMock(),
    )

    assert list(tools) == [
        "get_weather",
        "get_calendar",
        "get_mail",
        "get_quota",
    ]
    assert _tool_map() == {}


def test_weather_tool_uses_default_city_and_formats_readable_summary():
    service = MagicMock()
    service.get_current_weather.return_value = WeatherPayload(
        source="mock",
        location=WeatherLocation(city="苏州"),
        now=WeatherNow(
            temp_c=26,
            feels_like_c=28,
            text="晴",
            humidity_percent=55,
            wind_dir="东南风",
            wind_scale="3",
        ),
        daily=DailyForecast(
            items=[
                DailyForecastItem(
                    fx_date="2026-08-04",
                    text_day="多云",
                    temp_min_c=24,
                    temp_max_c=32,
                )
            ]
        ),
        minutely=MinutelyRain(summary="未来两小时暂无降水"),
        air=WeatherAir(aqi=38, category="优"),
    )

    result = _tool_map(weather_service=service)["get_weather"].invoke({"city": ""})

    service.get_current_weather.assert_called_once_with("苏州")
    assert "城市：苏州" in result
    assert "气温26°C" in result
    assert "2026-08-04多云 24~32°C" in result
    assert "空气质量优(AQI 38)" in result


def test_calendar_and_mail_tools_keep_timezone_and_mail_read_only_service_boundary():
    calendar = MagicMock()
    calendar.get_upcoming_events.return_value = CalendarPayload(
        source="mock",
        range_days=7,
        items=[
            CalendarEvent(
                title="家庭会议",
                relative="今天 20:00",
                location="客厅",
            )
        ],
    )
    mail = MagicMock()
    mail.get_mail_summary.return_value = MailPayload(
        source="mock",
        unread_count=1,
        messages=[
            MailMessage(
                from_name="张三",
                subject="项目周报",
                date_text="08-03 09:00",
                unread=True,
            )
        ],
    )
    tools = _tool_map(calendar_service=calendar, mail_service=mail)

    calendar_result = tools["get_calendar"].invoke({})
    mail_result = tools["get_mail"].invoke({})

    calendar.get_upcoming_events.assert_called_once_with("Asia/Shanghai")
    mail.get_mail_summary.assert_called_once_with("Asia/Shanghai")
    assert "今天 20:00：家庭会议（客厅）" in calendar_result
    assert "未读邮件 1 封" in mail_result
    assert "张三：项目周报（未读）" in mail_result


def test_quota_tool_formats_remaining_percentage_and_failure():
    quota = MagicMock()
    quota.check_glm.side_effect = [
        ProviderQuota(
            available=True,
            level="VIP",
            limits=[
                QuotaItem(
                    type="TOKENS_LIMIT",
                    display_name="五小时额度",
                    used_percent=40,
                    remaining_percent=60,
                    next_reset="08-03 18:00",
                )
            ],
        ),
        ProviderQuota(available=False, error="暂不可用"),
    ]
    tool = _tool_map(quota_service=quota)["get_quota"]

    assert "五小时额度：已用 40%，剩余 60%" in tool.invoke({})
    assert tool.invoke({}) == "额度查询失败：暂不可用"
    assert quota.check_glm.call_count == 2
