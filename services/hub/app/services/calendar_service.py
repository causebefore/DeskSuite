"""
日程服务 — 拉取 iCloud CalDAV 近期或自然月日程，供网页渲染和语音工具使用。

设计要点（复刻 WeatherService）：
- get_upcoming_events(timezone) 保持近期日程与语音查询语义
- get_month_events(timezone) 提供当前自然月概览和月末近期日程
- _get_cached 统一 TTL 缓存（缓存键含 timezone，避免跨时区设备串味）
- caldav 库负责 CalDAV 协议与 iCloud principal 自动发现
- icalendar 库负责 VEVENT 解析与 RRULE 展开后的实例读取
- 未配置凭据或拉取失败 → 返回 mock（source="mock"），永不抛 500
"""

from datetime import UTC, datetime, timedelta
from typing import Callable, TypeVar
from zoneinfo import ZoneInfo

import caldav
from loguru import logger

from app.core.config import ServerSettings
from app.schemas.calendar import CalendarEvent, CalendarPayload

_T = TypeVar("_T")

_WEEKDAY_CN = "一二三四五六日"


class CalendarService:
    """iCloud CalDAV 日程聚合服务。"""

    def __init__(self, settings: ServerSettings) -> None:
        self._settings = settings
        # 缓存：{timezone: (时间戳, CalendarPayload)}
        self._events_cache: dict[str, tuple[datetime, CalendarPayload]] = {}
        self._month_events_cache: dict[str, tuple[datetime, CalendarPayload]] = {}

    # ── 公开 API ──────────────────────────────────────

    def get_upcoming_events(self, timezone: str) -> CalendarPayload:
        """
        获取未来 range_days 天的日程（唯一对外接口）。

        Args:
            timezone: 设备时区（如 'Asia/Shanghai'），用于生成 relative 本地化文本

        Returns:
            CalendarPayload；未配置凭据或失败时为 mock
        """
        if (
            self._settings.calendar_provider != "icloud"
            or not self._settings.caldav_password
        ):
            logger.info("日程 mock 模式：未配置 iCloud CalDAV，provider={}", self._settings.calendar_provider)
            return self._mock_calendar(timezone, "未配置 iCloud CalDAV，返回 mock 数据")
        return self._get_cached(
            self._events_cache,
            timezone,
            self._settings.caldav_events_cache_seconds,
            lambda: self._fetch_icloud(timezone),
        )

    def get_month_events(self, timezone: str) -> CalendarPayload:
        """获取当前自然月，并覆盖月末未来 range_days 天的日程。"""
        tz = self._zone(timezone)
        now = datetime.now(tz)
        month_start = now.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
        if month_start.month == 12:
            next_month = month_start.replace(year=month_start.year + 1, month=1)
        else:
            next_month = month_start.replace(month=month_start.month + 1)
        upcoming_end = now + timedelta(days=self._settings.caldav_range_days)
        range_end = max(next_month, upcoming_end)
        range_days = (range_end.date() - month_start.date()).days
        cache_key = f"{timezone}:{month_start:%Y-%m}"

        if (
            self._settings.calendar_provider != "icloud"
            or not self._settings.caldav_password
        ):
            logger.info(
                "月历 mock 模式：未配置 iCloud CalDAV，provider={}",
                self._settings.calendar_provider,
            )
            return self._mock_calendar(
                timezone,
                "未配置 iCloud CalDAV，返回 mock 数据",
                range_days=range_days,
            )
        return self._get_cached(
            self._month_events_cache,
            cache_key,
            self._settings.caldav_events_cache_seconds,
            lambda: self._fetch_icloud_range(
                timezone=timezone,
                start=month_start.astimezone(UTC),
                end=range_end.astimezone(UTC),
                max_events=self._settings.caldav_month_max_events,
                range_days=range_days,
                label=f"{month_start:%Y-%m} 月历",
            ),
        )

    # ── 通用缓存 ──────────────────────────────────────

    def _get_cached(
        self,
        cache: dict[str, tuple[datetime, _T]],
        key: str,
        ttl_seconds: int,
        fetcher: Callable[[], _T],
    ) -> _T:
        """命中且未过期直接返回；否则回源并缓存。"""
        cached = cache.get(key)
        now = datetime.now(UTC)
        if cached and now - cached[0] < timedelta(seconds=ttl_seconds):
            return cached[1]
        value = fetcher()
        cache[key] = (now, value)
        return value

    # ── iCloud 拉取 ───────────────────────────────────

    def _fetch_icloud(self, timezone: str) -> CalendarPayload:
        """调用 iCloud CalDAV；任何异常降级为 mock 并缓存（避免反复失败登录）。"""
        start = datetime.now(UTC)
        end = start + timedelta(days=self._settings.caldav_range_days)
        return self._fetch_icloud_range(
            timezone=timezone,
            start=start,
            end=end,
            max_events=self._settings.caldav_max_events,
            range_days=self._settings.caldav_range_days,
            label="近期日程",
        )

    def _fetch_icloud_range(
        self,
        *,
        timezone: str,
        start: datetime,
        end: datetime,
        max_events: int,
        range_days: int,
        label: str,
    ) -> CalendarPayload:
        """查询一个有界时间窗口，并统一完成排序、截断和降级。"""
        logger.info(
            "开始拉取 iCloud {}：timezone={} start={} end={}",
            label,
            timezone,
            start.isoformat(),
            end.isoformat(),
        )
        try:
            client = caldav.DAVClient(
                url=self._settings.caldav_url,
                username=self._settings.caldav_username,
                password=self._settings.caldav_password,
                timeout=self._settings.caldav_timeout_seconds,
            )
            principal = client.principal()  # iCloud 自动发现
            calendars = principal.calendars()
            if not calendars:
                logger.warning("iCloud 账号下未发现日历")
                return self._mock_calendar(timezone, "iCloud 账号下无日历")

            items: list[CalendarEvent] = []
            for calendar in calendars:
                # expand=True 由服务端展开重复事件（iCloud 支持）
                for ev in calendar.date_search(start, end, expand=True):
                    items.extend(self._parse_vevent(ev, timezone))

            items.sort(key=lambda e: e.start)
            items = items[:max_events]
            logger.info("iCloud {}拉取成功：共 {} 条", label, len(items))
            return CalendarPayload(
                source="icloud",
                range_days=range_days,
                items=items,
            )
        except Exception as exc:
            logger.warning("iCloud CalDAV {}拉取失败，降级 mock：{}", label, exc)
            return self._mock_calendar(
                timezone,
                f"iCloud 请求失败: {exc}",
                range_days=range_days,
            )

    def _parse_vevent(self, caldav_event, timezone: str) -> list[CalendarEvent]:
        """从 caldav Event 解析出 CalendarEvent 列表（一条 ics 可能含多个 VEVENT）。"""
        results: list[CalendarEvent] = []
        try:
            ical = caldav_event.icalendar_component
            for comp in ical.walk("VEVENT"):
                title = str(comp.get("SUMMARY", ""))
                location = str(comp.get("LOCATION", ""))
                dtstart = comp.get("DTSTART")
                dtend = comp.get("DTEND")
                if dtstart is None:
                    continue
                start_val = dtstart.dt
                all_day = not isinstance(start_val, datetime)
                if all_day:
                    end_val = dtend.dt if dtend else start_val
                    results.append(
                        CalendarEvent(
                            title=title,
                            location=location,
                            start=start_val.isoformat(),
                            end=end_val.isoformat(),
                            date=start_val.isoformat(),
                            all_day=True,
                            relative=self._relative_all_day(start_val, timezone),
                        )
                    )
                else:
                    start_utc = self._to_utc(start_val)
                    end_utc = self._to_utc(dtend.dt) if dtend else start_utc
                    local_start = start_utc.astimezone(self._zone(timezone))
                    results.append(
                        CalendarEvent(
                            title=title,
                            location=location,
                            start=self._iso_utc(start_utc),
                            end=self._iso_utc(end_utc),
                            date=local_start.strftime("%Y-%m-%d"),
                            time=local_start.strftime("%H:%M"),
                            all_day=False,
                            relative=self._relative_datetime(start_utc, timezone),
                        )
                    )
        except Exception as exc:
            logger.debug("单条 VEVENT 解析失败，跳过：{}", exc)
        return results

    # ── 时间工具 ──────────────────────────────────────

    @staticmethod
    def _to_utc(dt: datetime) -> datetime:
        if dt.tzinfo is None:
            return dt.replace(tzinfo=UTC)
        return dt.astimezone(UTC)

    @staticmethod
    def _iso_utc(dt: datetime) -> str:
        return dt.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

    @staticmethod
    def _zone(timezone: str):
        try:
            return ZoneInfo(timezone)
        except Exception:
            return UTC

    def _relative_datetime(self, dt_utc: datetime, timezone: str) -> str:
        tz = self._zone(timezone)
        local = dt_utc.astimezone(tz)
        today = datetime.now(tz).date()
        delta = (local.date() - today).days
        hhmm = local.strftime("%H:%M")
        weekday = "周" + _WEEKDAY_CN[local.weekday()]
        if delta == 0:
            return f"今天 {hhmm}"
        if delta == 1:
            return f"明天 {hhmm}"
        if delta == -1:
            return f"昨天 {hhmm}"
        if 1 < delta < 7:
            return f"{weekday} {hhmm}"
        return local.strftime("%m-%d %H:%M")

    def _relative_all_day(self, d, timezone: str) -> str:
        tz = self._zone(timezone)
        today = datetime.now(tz).date()
        delta = (d - today).days
        weekday = "周" + _WEEKDAY_CN[d.weekday()]
        if delta == 0:
            return "今天 全天"
        if delta == 1:
            return "明天 全天"
        if 1 < delta < 7:
            return f"{weekday} 全天"
        return d.strftime("%m-%d 全天")

    # ── Mock ──────────────────────────────────────────

    def _mock_calendar(
        self,
        timezone: str,
        error: str = "",
        *,
        range_days: int | None = None,
    ) -> CalendarPayload:
        return CalendarPayload(
            source="mock",
            error=error,
            range_days=range_days or self._settings.caldav_range_days,
            items=[],
        )
