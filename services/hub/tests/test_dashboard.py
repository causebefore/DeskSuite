"""DeskMate Dashboard schema 3 聚合、降级、裁剪和路由契约测试。"""

import re
import time
from datetime import UTC, datetime
from types import SimpleNamespace

from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.api import dashboard
from app.schemas.calendar import CalendarEvent, CalendarPayload
from app.schemas.mail import MailMessage, MailPayload
from app.schemas.quota import ProviderQuota, QuotaItem
from app.schemas.weather import (
    DailyForecast,
    DailyForecastItem,
    MinutelyRain,
    WeatherAir,
    WeatherAlert,
    WeatherLocation,
    WeatherNow,
    WeatherPayload,
)
from app.services.dashboard_service import DashboardService


def _settings(*, token: str = "shared-secret", timeout: float = 0.2):
    return SimpleNamespace(
        device_api_token=token,
        display_default_device_id="default-device",
        display_default_city="江苏省昆山市",
        display_default_timezone="Asia/Shanghai",
        weather_provider="qweather",
        calendar_provider="icloud",
        mail_provider="qq",
        dashboard_source_timeout_seconds=timeout,
        display_refresh_interval_seconds=3600,
        display_refresh_daily_times=(),
        display_refresh_schedule_timezone="UTC",
    )


def _weather() -> WeatherPayload:
    long_text = "超长中文字段" * 40
    return WeatherPayload(
        source="qweather",
        updated_at=datetime(2026, 7, 25, 8, 9, 10, 123456, tzinfo=UTC),
        location=WeatherLocation(city=long_text),
        now=WeatherNow(
            text=long_text,
            icon="100",
            temp_c=31,
            feels_like_c=35,
            humidity_percent=72,
            wind_dir="东南风",
            wind_scale="3-4",
            pressure_hpa=1006,
            precip_mm=0.5,
            vis_km=18,
        ),
        daily=DailyForecast(
            items=[
                DailyForecastItem(
                    fx_date=f"2026-07-{25 + index:02d}",
                    text_day=long_text,
                    text_night="多云",
                    icon_day="101",
                    temp_min_c=25,
                    temp_max_c=34,
                    sunrise="05:12",
                    sunset="18:58",
                )
                for index in range(5)
            ]
        ),
        minutely=MinutelyRain(summary=long_text),
        alerts=[
            WeatherAlert(title=long_text, severity="黄色", text=long_text)
        ],
        air=WeatherAir(aqi=42, category="优"),
        error="",
    )


def _calendar() -> CalendarPayload:
    return CalendarPayload(
        source="icloud",
        items=[
            CalendarEvent(
                title=f"日程{index}" + "很长" * 30,
                relative="今天 10:00",
                all_day=False,
                location="会议室" * 20,
            )
            for index in range(7)
        ],
    )


def _mail() -> MailPayload:
    return MailPayload(
        source="qq-imap",
        unread_count=9,
        messages=[
            MailMessage(
                from_name="发件人" * 20,
                subject=f"邮件{index}" + "主题" * 40,
                date_text="07-25 09:00",
                unread=index % 2 == 0,
            )
            for index in range(7)
        ],
    )


def _quota() -> ProviderQuota:
    return ProviderQuota(
        available=True,
        level="VIP",
        limits=[
            QuotaItem(
                type=f"LIMIT_{index}",
                used_percent=10.0 + index,
                remaining_percent=90.0 - index,
                next_reset="2026-07-26 00:00",
            )
            for index in range(6)
        ],
    )


class _ValueService:
    def __init__(self, value=None, *, error: Exception | None = None, delay=0.0):
        self.value = value
        self.error = error
        self.delay = delay
        self.mail_prioritize_unread = None

    def _get(self):
        if self.delay:
            time.sleep(self.delay)
        if self.error is not None:
            raise self.error
        return self.value

    def get_current_weather(self, city: str):
        assert city == "江苏省昆山市"
        return self._get()

    def get_upcoming_events(self, timezone: str):
        assert timezone == "Asia/Shanghai"
        return self._get()

    def get_mail_summary(self, timezone: str, *, prioritize_unread: bool = False):
        assert timezone == "Asia/Shanghai"
        self.mail_prioritize_unread = prioritize_unread
        return self._get()

    def check_glm(self):
        return self._get()


def _client(
    *,
    settings=None,
    weather_service=None,
    calendar_service=None,
    mail_service=None,
    quota_service=None,
) -> TestClient:
    settings = settings or _settings()
    app = FastAPI()
    app.state.server_settings = settings
    app.state.dashboard_service = DashboardService(
        settings,
        weather_service=weather_service or _ValueService(_weather()),
        calendar_service=calendar_service or _ValueService(_calendar()),
        mail_service=mail_service or _ValueService(_mail()),
        quota_service=quota_service or _ValueService(_quota()),
    )
    app.include_router(dashboard.router, prefix="/api/v1/dashboard")
    return TestClient(app)


def _headers(token: str = "shared-secret") -> dict[str, str]:
    return {
        "Authorization": f"Bearer {token}",
        "X-Device-Id": "esp32-001122aabbcc",
    }


def test_dashboard_success_matches_schema_3_and_esp32_bounds():
    mail_service = _ValueService(_mail())
    response = _client(mail_service=mail_service).get(
        "/api/v1/dashboard",
        headers=_headers(),
    )

    assert response.status_code == 200
    assert mail_service.mail_prioritize_unread is True
    payload = response.json()
    assert payload["schema"] == 3
    assert payload["device_id"] == "esp32-001122aabbcc"
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", payload["generated_at"])
    assert payload["next_refresh_at_utc"] % 3600 == 0
    assert payload["next_refresh_at_utc"] > int(
        datetime.fromisoformat(payload["generated_at"].replace("Z", "+00:00")).timestamp()
    )
    assert "." not in payload["weather"]["updated_at"]
    assert len(payload["weather"]["daily"]) == 3
    assert len(payload["calendar"]["items"]) == 5
    assert len(payload["mail"]["messages"]) == 5
    assert len(payload["quota"]["limits"]) == 4
    assert "active_page" not in payload
    assert "settings" not in payload
    assert "refresh_seconds" not in payload
    assert len(payload["weather"]["city"].encode("utf-8")) <= 31
    assert len(payload["mail"]["messages"][0]["subject"].encode("utf-8")) <= 63
    assert len(response.content) < 12_288


def test_single_source_exception_returns_stable_error_block_and_http_200():
    response = _client(
        calendar_service=_ValueService(error=RuntimeError("secret provider detail"))
    ).get("/api/v1/dashboard", headers=_headers())

    assert response.status_code == 200
    payload = response.json()
    assert payload["calendar"] == {
        "source": "icloud",
        "error": "数据源异常",
        "items": [],
    }
    assert payload["weather"]["temp_c"] == 31
    assert "secret provider detail" not in response.text


def test_single_source_timeout_returns_without_waiting_for_slow_source():
    settings = _settings(timeout=0.01)
    client = _client(
        settings=settings,
        mail_service=_ValueService(_mail(), delay=0.15),
    )
    with client:
        started = time.monotonic()
        response = client.get("/api/v1/dashboard", headers=_headers())
        elapsed = time.monotonic() - started

        assert response.status_code == 200
        assert response.json()["mail"] == {
            "source": "qq",
            "error": "数据源超时",
            "unread_count": None,
            "messages": [],
        }
        assert elapsed < 0.12


def test_dashboard_rejects_wrong_token_and_invalid_device_id():
    client = _client()

    assert (
        client.get("/api/v1/dashboard", headers=_headers("wrong")).status_code
        == 401
    )
    assert (
        client.get(
            "/api/v1/dashboard",
            headers={
                "Authorization": "Bearer shared-secret",
                "X-Device-Id": "x" * 81,
            },
        ).status_code
        == 400
    )


def test_dashboard_empty_token_development_mode_echoes_device_id():
    response = _client(settings=_settings(token="")).get(
        "/api/v1/dashboard",
        headers={"X-Device-Id": "esp32-aabbccddeeff"},
    )

    assert response.status_code == 200
    assert response.json()["device_id"] == "esp32-aabbccddeeff"
