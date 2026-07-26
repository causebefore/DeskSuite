# 文件说明：测试 CalendarService 的 mock 降级、正常解析、异常降级、缓存命中。
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace
from zoneinfo import ZoneInfo

import icalendar
from icalendar import Calendar as IcalCalendar

from app.services.calendar_service import CalendarService


def _settings(**overrides):
    base = dict(
        calendar_provider="icloud",
        caldav_url="https://caldav.icloud.com/",
        caldav_username="u@icloud.com",
        caldav_password="pwd",
        caldav_timeout_seconds=8,
        caldav_range_days=7,
        caldav_max_events=10,
        caldav_month_max_events=100,
        caldav_events_cache_seconds=600,
    )
    base.update(overrides)
    return SimpleNamespace(**base)


# 一条带时区的 VEVENT（北京时间 2026-07-04 10:00 = UTC 02:00）
_VEVENT_ICS = (
    "BEGIN:VCALENDAR\r\n"
    "BEGIN:VEVENT\r\n"
    "SUMMARY:周会\r\n"
    "LOCATION:会议室A\r\n"
    "DTSTART;TZID=Asia/Shanghai:20260704T100000\r\n"
    "DTEND;TZID=Asia/Shanghai:20260704T110000\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n"
)


class _FakeEvent:
    def __init__(self, ics):
        self._ics = ics

    @property
    def icalendar_component(self):
        return IcalCalendar.from_ical(self._ics)


class _FakeCalendar:
    def date_search(self, start, end, expand=True):
        return [_FakeEvent(_VEVENT_ICS)]


class _FakePrincipal:
    def calendars(self):
        return [_FakeCalendar()]


class _FakeClientOk:
    def __init__(self, **kwargs):
        pass

    def principal(self):
        return _FakePrincipal()


class _FakeClientBoom:
    def __init__(self, **kwargs):
        pass

    def principal(self):
        raise RuntimeError("auth failed")


def test_mock_when_provider_not_icloud():
    svc = CalendarService(_settings(calendar_provider="mock"))
    result = svc.get_upcoming_events("Asia/Shanghai")
    assert result.source == "mock"
    assert result.items == []


def test_mock_when_password_missing():
    svc = CalendarService(_settings(caldav_password=""))
    result = svc.get_upcoming_events("Asia/Shanghai")
    assert result.source == "mock"


def test_parses_events_from_icloud(monkeypatch):
    monkeypatch.setattr("app.services.calendar_service.caldav.DAVClient", _FakeClientOk)
    svc = CalendarService(_settings())
    result = svc.get_upcoming_events("Asia/Shanghai")
    assert result.source == "icloud"
    assert len(result.items) == 1
    ev = result.items[0]
    assert ev.title == "周会"
    assert ev.location == "会议室A"
    assert ev.start == "2026-07-04T02:00:00Z"  # 北京 10:00 → UTC 02:00
    assert ev.date == "2026-07-04"
    assert ev.time == "10:00"
    assert ev.all_day is False
    # relative 是本地化时间文本；前缀（今天/明天/周X/MM-DD）视运行日期而定，
    # 但所有分支都包含 HH:MM（北京时间 10:00）。
    assert "10:00" in ev.relative


def test_degrades_on_exception(monkeypatch):
    monkeypatch.setattr("app.services.calendar_service.caldav.DAVClient", _FakeClientBoom)
    svc = CalendarService(_settings())
    result = svc.get_upcoming_events("Asia/Shanghai")
    assert result.source == "mock"
    assert "auth failed" in result.error


def test_caches_within_ttl(monkeypatch):
    calls = {"n": 0}

    class _CountingClient(_FakeClientOk):
        def principal(self):
            calls["n"] += 1
            return super().principal()

    monkeypatch.setattr("app.services.calendar_service.caldav.DAVClient", _CountingClient)
    svc = CalendarService(_settings(caldav_events_cache_seconds=600))
    svc.get_upcoming_events("Asia/Shanghai")
    svc.get_upcoming_events("Asia/Shanghai")  # 第二次应命中缓存
    assert calls["n"] == 1


def test_month_query_starts_at_local_month_boundary(monkeypatch):
    observed = {}

    class _MonthCalendar(_FakeCalendar):
        def date_search(self, start, end, expand=True):
            observed.update(start=start, end=end, expand=expand)
            return super().date_search(start, end, expand=expand)

    class _MonthPrincipal:
        def calendars(self):
            return [_MonthCalendar()]

    class _MonthClient(_FakeClientOk):
        def principal(self):
            return _MonthPrincipal()

    monkeypatch.setattr("app.services.calendar_service.caldav.DAVClient", _MonthClient)
    result = CalendarService(_settings()).get_month_events("Asia/Shanghai")

    local_start = observed["start"].astimezone(ZoneInfo("Asia/Shanghai"))
    local_end = observed["end"].astimezone(ZoneInfo("Asia/Shanghai"))
    assert (local_start.day, local_start.hour, local_start.minute) == (1, 0, 0)
    assert 28 <= (local_end.date() - local_start.date()).days <= 38
    assert observed["expand"] is True
    assert result.source == "icloud"


def test_parses_all_day_event_with_stable_local_date():
    all_day_ics = (
        "BEGIN:VCALENDAR\r\n"
        "BEGIN:VEVENT\r\n"
        "SUMMARY:纪念日\r\n"
        "DTSTART;VALUE=DATE:20260720\r\n"
        "DTEND;VALUE=DATE:20260721\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n"
    )
    event = CalendarService(_settings())._parse_vevent(
        _FakeEvent(all_day_ics),
        "Asia/Shanghai",
    )[0]

    assert event.start == "2026-07-20"
    assert event.end == "2026-07-21"
    assert event.date == "2026-07-20"
    assert event.time == ""
    assert event.all_day is True
