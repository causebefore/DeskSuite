"""页面依赖注册与请求驱动显示刷新协调测试。"""

from concurrent.futures import ThreadPoolExecutor
from threading import Barrier, Event, Lock
import time
from types import SimpleNamespace

from app.services.display_context_service import DisplayContextService
from app.services.display_page_registry import (
    required_sources_for_pages,
    select_page_context,
)
from app.services.display_refresh_service import DisplayRefreshService


def _settings(pages=("demo", "calendar"), default_page="demo"):
    return SimpleNamespace(display_pages=pages, display_default_page=default_page)


def test_page_registry_selects_context_and_availability_by_dependency():
    page_data = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "weather": {"text": "晴"},
        "moon": {"current": {"name": "亏凸月"}},
        "calendar": [{"title": "评审"}],
        "calendar_month": {"year": 2026, "month": 7, "events": []},
        "mail": {"unread_count": 2},
        "quota": {"available": True},
        "rss": {"article_count": 1},
        "device_status": {"available": True},
        "memory": ["不可见"],
        "availability": {
            "weather": True,
            "moon": True,
            "calendar": False,
            "calendar_month": True,
            "mail": True,
            "quota": True,
            "rss": True,
        },
    }

    calendar = select_page_context("calendar", page_data)
    month_calendar = select_page_context("month-calendar", page_data)
    weather = select_page_context("weather", page_data)
    moon = select_page_context("moon", page_data)
    rss = select_page_context("rss", page_data)

    assert calendar == {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "calendar": [{"title": "评审"}],
        "availability": {"calendar": False},
    }
    assert weather == {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "weather": {"text": "晴"},
        "availability": {"weather": True},
    }
    assert moon == {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "moon": {"current": {"name": "亏凸月"}},
        "availability": {"moon": True},
    }
    assert month_calendar == {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "calendar_month": {"year": 2026, "month": 7, "events": []},
        "availability": {"calendar_month": True},
    }
    assert rss == {
        "rss": {"article_count": 1},
        "availability": {"rss": True},
    }
    assert required_sources_for_pages(("calendar",)) == frozenset({"calendar"})
    assert required_sources_for_pages(("month-calendar",)) == frozenset(
        {"calendar_month"}
    )
    assert required_sources_for_pages(("weather",)) == frozenset({"weather"})
    assert required_sources_for_pages(("moon",)) == frozenset({"moon"})
    assert required_sources_for_pages(("rss",)) == frozenset({"rss"})
    assert required_sources_for_pages(("demo", "calendar")) == frozenset(
        {"weather", "calendar", "mail", "quota", "device_status"}
    )
    assert required_sources_for_pages(("demo", "calendar", "weather")) == frozenset(
        {"weather", "calendar", "mail", "quota", "device_status"}
    )


def test_unregistered_page_uses_legacy_visible_context():
    page_data = {
        "date": "2026年07月18日",
        "weather": {"text": "晴"},
        "calendar": [],
        "mail": {"unread_count": 1},
        "quota": {"available": True},
        "device_status": {"available": True},
        "memory": ["不应进入旧页面快照"],
        "availability": {"weather": True, "mail": True},
    }

    context = select_page_context("custom-page", page_data)

    assert context["weather"] == {"text": "晴"}
    assert context["mail"] == {"unread_count": 1}
    assert "memory" not in context


def test_context_service_only_calls_required_sources():
    calls: list[str] = []
    calendar = SimpleNamespace(items=[])
    service = DisplayContextService(
        SimpleNamespace(
            display_default_timezone="Asia/Shanghai",
            display_default_city="苏州",
        ),
        weather_service=SimpleNamespace(
            get_current_weather=lambda city: calls.append("weather")
        ),
        calendar_service=SimpleNamespace(
            get_upcoming_events=lambda timezone: (calls.append("calendar"), calendar)[1]
        ),
        mail_service=SimpleNamespace(
            get_mail_summary=lambda timezone: calls.append("mail")
        ),
        quota_service=SimpleNamespace(check_glm=lambda: calls.append("quota")),
        memory_service=SimpleNamespace(
            query_memory=lambda device_id, query: calls.append("memory")
        ),
        device_status_service=SimpleNamespace(
            get=lambda device_id: calls.append("device_status")
        ),
        rss_service=SimpleNamespace(
            get_latest_articles=lambda timezone: calls.append("rss")
        ),
    )

    context = service.build("calendar-only", required_sources={"calendar"})

    assert calls == ["calendar"]
    assert context["calendar"] == []
    assert context["availability"] == {"calendar": True}


def test_context_service_reuses_month_query_for_agenda():
    calls: list[str] = []
    month_payload = SimpleNamespace(source="icloud", error="", items=[])
    service = DisplayContextService(
        SimpleNamespace(
            display_default_timezone="Asia/Shanghai",
            display_default_city="苏州",
        ),
        weather_service=SimpleNamespace(),
        calendar_service=SimpleNamespace(
            get_upcoming_events=lambda timezone: calls.append("calendar"),
            get_month_events=lambda timezone: (
                calls.append("calendar_month"),
                month_payload,
            )[1],
        ),
        mail_service=SimpleNamespace(),
        quota_service=SimpleNamespace(),
        memory_service=SimpleNamespace(),
        device_status_service=SimpleNamespace(),
        rss_service=SimpleNamespace(),
    )

    context = service.build(
        "calendar-combined",
        required_sources={"calendar", "calendar_month"},
    )

    assert calls == ["calendar_month"]
    assert context["calendar"] == []
    assert context["calendar_month"]["events"] == []
    assert context["availability"] == {
        "calendar": True,
        "calendar_month": True,
    }


def test_refresh_service_passes_required_sources_and_resolved_pages():
    observed: dict = {}

    def build(device_id: str, required_sources=None):
        observed["device_id"] = device_id
        observed["required_sources"] = required_sources
        return {"calendar": []}

    def render_collection(**kwargs):
        observed["render"] = kwargs
        return "manifest"

    service = DisplayRefreshService(
        _settings(),
        context_service=SimpleNamespace(build=build),
        render_service=SimpleNamespace(render_collection=render_collection),
    )

    result = service.refresh_collection(
        "screen-1",
        pages=["calendar"],
        default_page="calendar",
    )

    assert result == "manifest"
    assert observed["required_sources"] == frozenset({"calendar"})
    assert observed["render"]["pages"] == ("calendar",)
    assert observed["render"]["default_page"] == "calendar"


def test_same_device_refreshes_are_serialized():
    guard = Lock()
    first_entered = Event()
    release_first = Event()
    active = 0
    max_active = 0
    calls = 0

    def build(device_id: str, required_sources=None):
        nonlocal active, max_active, calls
        with guard:
            calls += 1
            call_number = calls
            active += 1
            max_active = max(max_active, active)
        if call_number == 1:
            first_entered.set()
            assert release_first.wait(timeout=2)
        with guard:
            active -= 1
        return {}

    service = DisplayRefreshService(
        _settings(pages=("calendar",), default_page="calendar"),
        context_service=SimpleNamespace(build=build),
        render_service=SimpleNamespace(render_collection=lambda **kwargs: kwargs),
    )

    with ThreadPoolExecutor(max_workers=2) as executor:
        first = executor.submit(service.refresh_collection, "same-device")
        assert first_entered.wait(timeout=2)
        second = executor.submit(service.refresh_collection, "same-device")
        time.sleep(0.05)
        with guard:
            assert calls == 1
        release_first.set()
        first.result(timeout=2)
        second.result(timeout=2)

    assert max_active == 1


def test_different_devices_use_independent_locks():
    barrier = Barrier(2)

    def build(device_id: str, required_sources=None):
        barrier.wait(timeout=2)
        return {}

    service = DisplayRefreshService(
        _settings(pages=("calendar",), default_page="calendar"),
        context_service=SimpleNamespace(build=build),
        render_service=SimpleNamespace(render_collection=lambda **kwargs: kwargs),
    )

    with ThreadPoolExecutor(max_workers=2) as executor:
        first = executor.submit(service.refresh_collection, "device-a")
        second = executor.submit(service.refresh_collection, "device-b")
        first.result(timeout=3)
        second.result(timeout=3)
