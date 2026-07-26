"""并发聚合现有数据服务并投影为 DeskMate Dashboard schema 3。"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from datetime import UTC, datetime
from typing import Any

from loguru import logger

from app.core.config import ServerSettings
from app.schemas.calendar import CalendarPayload
from app.schemas.dashboard import (
    DashboardCalendar,
    DashboardCalendarItem,
    DashboardDailyItem,
    DashboardMail,
    DashboardMailMessage,
    DashboardQuota,
    DashboardQuotaLimit,
    DashboardResponse,
    DashboardWeather,
)
from app.schemas.mail import MailPayload
from app.schemas.quota import ProviderQuota
from app.schemas.weather import WeatherPayload
from app.services.calendar_service import CalendarService
from app.services.display_refresh_service import next_refresh_at_utc
from app.services.mail_service import MailService
from app.services.quota_service import QuotaService
from app.services.weather_service import WeatherService


_SOURCE_BYTES = 15
_ERROR_BYTES = 95
_ISO_BYTES = 31


def _clip(value: Any, max_bytes: int) -> str:
    """按 ESP32 UTF-8 缓冲区可用字节数裁剪字符串。"""
    text = "" if value is None else str(value)
    encoded = text.encode("utf-8")
    if len(encoded) <= max_bytes:
        return text
    return encoded[:max_bytes].decode("utf-8", errors="ignore")


def _utc_iso(value: datetime | None = None) -> str:
    """输出无微秒的 UTC ISO 8601 文本。"""
    current = value or datetime.now(UTC)
    if current.tzinfo is None:
        current = current.replace(tzinfo=UTC)
    return current.astimezone(UTC).replace(microsecond=0).isoformat().replace(
        "+00:00",
        "Z",
    )


class DashboardService:
    """复用 Weather、Calendar、Mail、Quota 单例生成设备投影。"""

    def __init__(
        self,
        settings: ServerSettings,
        *,
        weather_service: WeatherService,
        calendar_service: CalendarService,
        mail_service: MailService,
        quota_service: QuotaService,
    ) -> None:
        self._settings = settings
        self._weather_service = weather_service
        self._calendar_service = calendar_service
        self._mail_service = mail_service
        self._quota_service = quota_service

    async def build(self, device_id: str) -> DashboardResponse:
        """并发执行四个数据源，单源失败或超时不影响整体 200 响应。"""
        generated_at_value = datetime.now(UTC)
        generated_at = _utc_iso(generated_at_value)
        next_refresh = next_refresh_at_utc(
            self._settings.display_refresh_interval_seconds,
            generated_at_value,
            daily_times=self._settings.display_refresh_daily_times,
            timezone_name=self._settings.display_refresh_schedule_timezone,
        )
        source_calls: dict[str, Callable[[], Any]] = {
            "weather": lambda: self._weather_service.get_current_weather(
                self._settings.display_default_city
            ),
            "calendar": lambda: self._calendar_service.get_upcoming_events(
                self._settings.display_default_timezone
            ),
            "mail": lambda: self._mail_service.get_mail_summary(
                self._settings.display_default_timezone
            ),
            "quota": self._quota_service.check_glm,
        }
        results = await asyncio.gather(
            *(self._run_source(name, call) for name, call in source_calls.items())
        )
        by_name = dict(zip(source_calls, results, strict=True))
        return DashboardResponse(
            device_id=device_id,
            generated_at=generated_at,
            next_refresh_at_utc=next_refresh,
            weather=self._weather_projection(by_name["weather"], generated_at),
            calendar=self._calendar_projection(by_name["calendar"]),
            mail=self._mail_projection(by_name["mail"]),
            quota=self._quota_projection(by_name["quota"], generated_at),
        )

    async def _run_source(
        self,
        name: str,
        call: Callable[[], Any],
    ) -> Any:
        """在线程中执行阻塞数据源，并把超时与异常收敛为内部失败标记。"""
        try:
            return await asyncio.wait_for(
                asyncio.to_thread(call),
                timeout=self._settings.dashboard_source_timeout_seconds,
            )
        except TimeoutError:
            logger.warning(
                "Dashboard 数据源超时: source={} timeout={}s",
                name,
                self._settings.dashboard_source_timeout_seconds,
            )
            return _SourceFailure("数据源超时")
        except Exception:
            logger.exception("Dashboard 数据源异常: source={}", name)
            return _SourceFailure("数据源异常")

    def _weather_projection(
        self,
        value: WeatherPayload | _SourceFailure,
        generated_at: str,
    ) -> DashboardWeather:
        if isinstance(value, _SourceFailure):
            return DashboardWeather(
                source=_clip(self._settings.weather_provider, _SOURCE_BYTES),
                updated_at=generated_at,
                error=_clip(value.error, _ERROR_BYTES),
            )
        first_alert = value.alerts[0] if value.alerts else None
        air = value.air
        return DashboardWeather(
            source=_clip(value.source, _SOURCE_BYTES),
            updated_at=_clip(_utc_iso(value.updated_at), _ISO_BYTES),
            error=_clip(value.error, _ERROR_BYTES),
            city=_clip(value.location.city, 31),
            text=_clip(value.now.text, 23),
            icon=_clip(value.now.icon, 7),
            temp_c=value.now.temp_c,
            feels_like_c=value.now.feels_like_c,
            humidity_percent=value.now.humidity_percent,
            wind_dir=_clip(value.now.wind_dir, 23),
            wind_scale=_clip(value.now.wind_scale, 11),
            pressure_hpa=value.now.pressure_hpa,
            precip_mm=value.now.precip_mm,
            vis_km=value.now.vis_km,
            daily=[
                DashboardDailyItem(
                    fx_date=_clip(item.fx_date, 15),
                    text_day=_clip(item.text_day, 23),
                    text_night=_clip(item.text_night, 23),
                    icon_day=_clip(item.icon_day, 7),
                    temp_min_c=item.temp_min_c,
                    temp_max_c=item.temp_max_c,
                    sunrise=_clip(item.sunrise, 7),
                    sunset=_clip(item.sunset, 7),
                )
                for item in value.daily.items[:3]
            ],
            aqi=air.aqi if air is not None else None,
            aqi_category=_clip(air.category if air is not None else "", 23),
            minutely_summary=_clip(value.minutely.summary, 63),
            alert_title=_clip(first_alert.title if first_alert is not None else "", 63),
            alert_severity=_clip(
                first_alert.severity if first_alert is not None else "",
                15,
            ),
        )

    def _calendar_projection(
        self,
        value: CalendarPayload | _SourceFailure,
    ) -> DashboardCalendar:
        if isinstance(value, _SourceFailure):
            return DashboardCalendar(
                source=_clip(self._settings.calendar_provider, _SOURCE_BYTES),
                error=_clip(value.error, _ERROR_BYTES),
            )
        return DashboardCalendar(
            source=_clip(value.source, _SOURCE_BYTES),
            error=_clip(value.error, _ERROR_BYTES),
            items=[
                DashboardCalendarItem(
                    title=_clip(item.title, 47),
                    relative=_clip(item.relative, 23),
                    all_day=item.all_day,
                    location=_clip(item.location, 31),
                )
                for item in value.items[:5]
            ],
        )

    def _mail_projection(
        self,
        value: MailPayload | _SourceFailure,
    ) -> DashboardMail:
        if isinstance(value, _SourceFailure):
            return DashboardMail(
                source=_clip(self._settings.mail_provider, _SOURCE_BYTES),
                error=_clip(value.error, _ERROR_BYTES),
            )
        return DashboardMail(
            source=_clip(value.source, _SOURCE_BYTES),
            error=_clip(value.error, _ERROR_BYTES),
            unread_count=value.unread_count,
            messages=[
                DashboardMailMessage(
                    from_name=_clip(item.from_name, 31),
                    subject=_clip(item.subject, 63),
                    date_text=_clip(item.date_text, 23),
                    unread=item.unread,
                )
                for item in value.messages[:5]
            ],
        )

    @staticmethod
    def _quota_projection(
        value: ProviderQuota | _SourceFailure,
        generated_at: str,
    ) -> DashboardQuota:
        if isinstance(value, _SourceFailure):
            return DashboardQuota(
                source="zhipu",
                error=_clip(value.error, _ERROR_BYTES),
                updated_at=generated_at,
            )
        return DashboardQuota(
            available=value.available,
            source="zhipu",
            level=_clip(value.level, 15),
            error=_clip(value.error, _ERROR_BYTES),
            updated_at=generated_at,
            limits=[
                DashboardQuotaLimit(
                    type=_clip(item.type, 31),
                    used_percent=item.used_percent,
                    remaining_percent=item.remaining_percent,
                    next_reset=_clip(item.next_reset, 23),
                )
                for item in value.limits[:4]
            ],
        )


class _SourceFailure:
    """Dashboard 内部单源失败标记，不进入公开 Schema。"""

    def __init__(self, error: str) -> None:
        self.error = error
