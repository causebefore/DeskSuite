"""四灰阶量化、PPF2、多页面 Manifest 和路由边界测试。"""

from datetime import UTC, datetime, time, timedelta
from io import BytesIO
import json
from pathlib import Path
import shutil
from types import SimpleNamespace
import struct
import zlib

import pytest
from fastapi.testclient import TestClient
from PIL import Image
from pydantic import ValidationError

from app.main import create_app
from app.schemas.display import (
    DisplayCollectionManifest,
    DisplayPageManifest,
    DisplayRenderRequest,
    DisplayScheduledManifest,
)
from app.services.display_page_registry import validate_page_set
from app.services.display_context_service import DisplayContextService
from app.services.display_refresh_service import (
    DisplayRefreshService,
    schedule_display_manifest,
)
from app.services.display_render_service import (
    DISPLAY_PAYLOAD_SIZE,
    DisplayRenderService,
    PPF_HEADER,
)


def _source_png() -> bytes:
    image = Image.new("RGBA", (800, 480), (82, 137, 201, 210))
    buffer = BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def _solid_png(color: tuple[int, int, int, int]) -> bytes:
    image = Image.new("RGBA", (800, 480), color)
    buffer = BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


@pytest.mark.parametrize("dither", [False, True])
def test_pillow_quantization_only_emits_four_gray_levels(dither: bool):
    preview, payload = DisplayRenderService.quantize_to_frame(_source_png(), dither)

    assert preview.size == (800, 480)
    assert len(payload) == DISPLAY_PAYLOAD_SIZE
    assert set(preview.get_flattened_data()) <= {
        (0, 0, 0),
        (85, 85, 85),
        (170, 170, 170),
        (255, 255, 255),
    }


def test_four_gray_payload_uses_zero_for_black_and_three_for_white():
    _, white = DisplayRenderService.quantize_to_frame(
        _solid_png((255, 255, 255, 255)),
        False,
    )
    _, black = DisplayRenderService.quantize_to_frame(
        _solid_png((0, 0, 0, 255)),
        False,
    )

    assert white == b"\xff" * DISPLAY_PAYLOAD_SIZE
    assert black == bytes(DISPLAY_PAYLOAD_SIZE)


def test_four_gray_payload_packs_pixels_msb_first():
    image = Image.new("RGBA", (800, 480), (255, 255, 255, 255))
    for x, level in enumerate((0, 85, 170, 255)):
        image.putpixel((x, 0), (level, level, level, 255))
    buffer = BytesIO()
    image.save(buffer, format="PNG")

    _, payload = DisplayRenderService.quantize_to_frame(buffer.getvalue(), False)

    assert payload[0] == 0b00011011
    assert payload[1:200] == b"\xff" * 199


def test_display_page_count_accepts_16_and_rejects_17():
    """请求模型和页面注册表统一执行十六页上限。"""
    pages_16 = [f"page{i}" for i in range(16)]
    assert DisplayRenderRequest(pages=pages_16).pages == pages_16
    validate_page_set(tuple(pages_16), pages_16[0])

    pages_17 = [*pages_16, "page16"]
    with pytest.raises(ValidationError):
        DisplayRenderRequest(pages=pages_17)
    with pytest.raises(ValueError, match="1 到 16"):
        validate_page_set(tuple(pages_17), pages_17[0])


def test_render_writes_valid_ppf_header_and_crc(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)
    service._capture = lambda html: _source_png()  # type: ignore[method-assign]

    manifest = service.render("test-device", "demo", {"test": True})
    frame = service.frame_path(
        "test-device",
        "demo",
        str(manifest.content_version),
    ).read_bytes()
    fields = PPF_HEADER.unpack(frame[:PPF_HEADER.size])
    payload = frame[PPF_HEADER.size:]

    assert fields[:6] == (b"PPF2", 2, 2, 32, 800, 480)
    assert fields[6] == DISPLAY_PAYLOAD_SIZE
    assert fields[7] == zlib.crc32(payload) & 0xFFFFFFFF
    assert fields[8] == int(manifest.content_version.replace("-", ""))
    assert len(frame) == 96032
    assert manifest.file_size == 96032


def test_collection_renders_demo_and_calendar_with_independent_versions(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("demo", "calendar"),
        display_default_page="demo",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        if "PhotoPainter Calendar" in html and '"calendar": true' in html:
            return _solid_png((0, 0, 0, 255))
        return _solid_png((255, 255, 255, 255))

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    first = service.render_collection(
        "multi-device",
        {
            "calendar": [],
            "availability": {"calendar": False},
        },
    )
    second = service.render_collection(
        "multi-device",
        {
            "calendar": [],
            "availability": {"calendar": True},
        },
    )

    assert [page.page_id for page in first.pages] == ["demo", "calendar"]
    assert first.default_page == "demo"
    assert first.protocol_version == 3
    assert first.format == "PPF2"
    assert first.pixel_format == "GRAY2"
    assert first.payload_size == 96000
    assert first.file_size == 96032
    assert second.collection_version != first.collection_version
    assert second.pages[0].content_version == first.pages[0].content_version
    assert second.pages[1].content_version != first.pages[1].content_version
    assert capture_count == 4
    for page in second.pages:
        assert service.frame_path(
            "multi-device",
            page.page_id,
            page.content_version,
        ).is_file()
        assert page.frame_url.startswith(f"/api/v2/display/frame/{page.page_id}/")

    stored = service.get_manifest("multi-device")
    assert stored == second


def test_manifest_schedule_aligns_next_refresh_to_utc_boundary():
    manifest = DisplayCollectionManifest(
        device_id="schedule-device",
        collection_version="20260717-153000",
        default_page="demo",
        pages=[
            DisplayPageManifest(
                page_id="demo",
                content_version="20260717-153000",
                crc32="00000000",
                sha256="0" * 64,
                payload_sha256="0" * 64,
                created_at=datetime(2026, 7, 17, 7, 30, tzinfo=UTC),
                frame_url="/api/v2/display/frame/demo/20260717-153000.ppf",
                preview_url="/api/v2/display/preview/demo/20260717-153000.png",
            )
        ],
        created_at=datetime(2026, 7, 17, 7, 30, tzinfo=UTC),
    )

    scheduled = schedule_display_manifest(
        manifest,
        interval_seconds=3600,
        now=datetime(2026, 7, 17, 7, 30, tzinfo=UTC),
    )

    assert scheduled.next_refresh_at == 1784275200
    assert scheduled.protocol_version == 3
    assert "next_refresh_at" not in manifest.model_dump()
    assert "poll_after_seconds" not in scheduled.model_dump()
    with pytest.raises(ValidationError):
        DisplayScheduledManifest.model_validate(manifest.model_dump())


@pytest.mark.parametrize(
    ("now", "expected"),
    [
        (
            datetime(2026, 7, 17, 2, 30, tzinfo=UTC),
            datetime(2026, 7, 17, 3, 15, tzinfo=UTC),
        ),
        (
            datetime(2026, 7, 17, 3, 20, tzinfo=UTC),
            datetime(2026, 7, 17, 3, 50, tzinfo=UTC),
        ),
        (
            datetime(2026, 7, 17, 4, 0, tzinfo=UTC),
            datetime(2026, 7, 18, 3, 15, tzinfo=UTC),
        ),
        (
            datetime(2026, 7, 17, 3, 15, tzinfo=UTC),
            datetime(2026, 7, 17, 3, 50, tzinfo=UTC),
        ),
    ],
)
def test_manifest_schedule_uses_next_daily_time_in_configured_timezone(
    now: datetime,
    expected: datetime,
):
    manifest = DisplayCollectionManifest(
        device_id="daily-schedule-device",
        collection_version="20260717-153000",
        default_page="demo",
        pages=[
            DisplayPageManifest(
                page_id="demo",
                content_version="20260717-153000",
                crc32="00000000",
                sha256="0" * 64,
                payload_sha256="0" * 64,
                created_at=datetime(2026, 7, 17, 7, 30, tzinfo=UTC),
                frame_url="/api/v2/display/frame/demo/20260717-153000.ppf",
                preview_url="/api/v2/display/preview/demo/20260717-153000.png",
            )
        ],
        created_at=datetime(2026, 7, 17, 7, 30, tzinfo=UTC),
    )

    scheduled = schedule_display_manifest(
        manifest,
        interval_seconds=3600,
        now=now,
        daily_times=(time(11, 50), time(11, 15)),
        timezone_name="Asia/Shanghai",
    )

    assert scheduled.next_refresh_at == int(expected.timestamp())


def test_mail_change_only_updates_pages_that_depend_on_mail(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("demo", "calendar"),
        display_default_page="demo",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        color = (0, 0, 0, 255) if "新邮件主题" in html else (255, 255, 255, 255)
        return _solid_png(color)

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    base = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "calendar": [],
        "mail": {"unread_count": 1, "messages": []},
        "availability": {"calendar": True, "mail": True},
    }
    first = service.render_collection("mail-isolation", base)
    changed = {
        **base,
        "mail": {
            "unread_count": 2,
            "messages": [{"from": "测试", "subject": "新邮件主题", "date": "17:00"}],
        },
    }
    second = service.render_collection("mail-isolation", changed)

    first_versions = {page.page_id: page.content_version for page in first.pages}
    second_versions = {page.page_id: page.content_version for page in second.pages}
    assert second_versions["demo"] != first_versions["demo"]
    assert second_versions["calendar"] == first_versions["calendar"]
    assert capture_count == 3


def test_rss_change_only_updates_rss_page(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("calendar", "rss"),
        display_default_page="calendar",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        color = (0, 0, 0, 255) if "RSS 新文章" in html else (255, 255, 255, 255)
        return _solid_png(color)

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    base = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "calendar": [],
        "rss": {
            "article_count": 0,
            "source_count": 1,
            "sources": [{"name": "IT之家", "count": 0}],
            "items": [],
        },
        "availability": {"calendar": True, "rss": True},
    }
    first = service.render_collection("rss-isolation", base)
    second = service.render_collection(
        "rss-isolation",
        {
            **base,
            "rss": {
                "article_count": 1,
                "source_count": 1,
                "sources": [{"name": "IT之家", "count": 1}],
                "items": [
                    {
                        "title": "RSS 新文章",
                        "source": "IT之家",
                        "published_text": "18:00",
                    }
                ],
            },
        },
    )

    first_versions = {page.page_id: page.content_version for page in first.pages}
    second_versions = {page.page_id: page.content_version for page in second.pages}
    assert second_versions["calendar"] == first_versions["calendar"]
    assert second_versions["rss"] != first_versions["rss"]
    assert capture_count == 3


def test_moon_change_only_updates_moon_page(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("calendar", "moon"),
        display_default_page="calendar",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        color = (
            (0, 0, 0, 255)
            if "月相新状态" in html
            else (255, 255, 255, 255)
        )
        return _solid_png(color)

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    base = {
        "date": "2026年07月20日",
        "weekday": "星期一",
        "time": "下午5时",
        "calendar": [],
        "moon": {
            "current": {
                "time": "17:00",
                "name": "亏凸月",
                "illumination": 98,
                "icon": "805",
            },
            "timeline": [],
        },
        "availability": {"calendar": True, "moon": True},
    }
    first = service.render_collection("moon-isolation", base)
    second = service.render_collection(
        "moon-isolation",
        {
            **base,
            "moon": {
                **base["moon"],
                "current": {
                    **base["moon"]["current"],
                    "name": "月相新状态",
                },
            },
        },
    )

    first_versions = {page.page_id: page.content_version for page in first.pages}
    second_versions = {page.page_id: page.content_version for page in second.pages}
    assert second_versions["calendar"] == first_versions["calendar"]
    assert second_versions["moon"] != first_versions["moon"]
    assert capture_count == 3


def test_calendar_change_updates_demo_and_calendar_pages(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("demo", "calendar"),
        display_default_page="demo",
        display_dither=False,
        display_render_timeout_ms=1000,
    )

    def capture(html: str) -> bytes:
        color = (0, 0, 0, 255) if "新日程" in html else (255, 255, 255, 255)
        return _solid_png(color)

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    base = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "calendar": [],
        "availability": {"calendar": True},
    }
    first = service.render_collection("calendar-isolation", base)
    second = service.render_collection(
        "calendar-isolation",
        {
            **base,
            "calendar": [{"title": "新日程", "relative": "今天", "location": "会议室"}],
        },
    )

    first_versions = {page.page_id: page.content_version for page in first.pages}
    second_versions = {page.page_id: page.content_version for page in second.pages}
    assert second_versions["demo"] != first_versions["demo"]
    assert second_versions["calendar"] != first_versions["calendar"]


def test_month_calendar_change_only_updates_month_calendar_page(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("calendar", "month-calendar"),
        display_default_page="calendar",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        color = (0, 0, 0, 255) if "月历新日程" in html else (255, 255, 255, 255)
        return _solid_png(color)

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    base = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "calendar": [],
        "calendar_month": {
            "year": 2026,
            "month": 7,
            "source": "icloud",
            "event_count": 0,
            "events": [],
        },
        "availability": {"calendar": True, "calendar_month": True},
    }
    first = service.render_collection("month-calendar-isolation", base)
    second = service.render_collection(
        "month-calendar-isolation",
        {
            **base,
            "calendar_month": {
                **base["calendar_month"],
                "event_count": 1,
                "events": [
                    {
                        "title": "月历新日程",
                        "location": "会议室",
                        "start": "2026-07-20T02:00:00Z",
                        "end": "2026-07-20T03:00:00Z",
                        "date": "2026-07-20",
                        "time": "10:00",
                        "all_day": False,
                        "relative": "周一 10:00",
                    }
                ],
            },
        },
    )

    first_versions = {page.page_id: page.content_version for page in first.pages}
    second_versions = {page.page_id: page.content_version for page in second.pages}
    assert second_versions["calendar"] == first_versions["calendar"]
    assert second_versions["month-calendar"] != first_versions["month-calendar"]
    assert capture_count == 3


def test_weather_change_updates_demo_and_weather_only(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_pages=("demo", "calendar", "weather"),
        display_default_page="demo",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        color = (0, 0, 0, 255) if "暴雨" in html else (255, 255, 255, 255)
        return _solid_png(color)

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    base = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午5时",
        "weather": {"city": "苏州", "text": "晴", "daily": []},
        "calendar": [],
        "availability": {"weather": True, "calendar": True},
    }

    first = service.render_collection("weather-isolation", base)
    second = service.render_collection(
        "weather-isolation",
        {
            **base,
            "weather": {"city": "苏州", "text": "暴雨", "daily": []},
        },
    )

    first_versions = {page.page_id: page.content_version for page in first.pages}
    second_versions = {page.page_id: page.content_version for page in second.pages}
    assert second_versions["demo"] != first_versions["demo"]
    assert second_versions["weather"] != first_versions["weather"]
    assert second_versions["calendar"] == first_versions["calendar"]
    assert capture_count == 5


def test_same_visible_content_skips_capture_and_survives_restart(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(_html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        return _source_png()

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    first = service.render(
        "stable-device",
        "demo",
        {
            "device_id": "stable-device",
            "generated_at": "2026-07-15T20:00:00+08:00",
            "memory": ["不可见内容 A"],
            "time": "下午8时",
        },
    )
    second = service.render(
        "stable-device",
        "demo",
        {
            "device_id": "another-ignored-id",
            "generated_at": "2026-07-15T20:10:00+08:00",
            "memory": ["不可见内容 B"],
            "time": "下午8时",
        },
    )

    assert second.content_version == first.content_version
    assert capture_count == 1
    state_path = next((tmp_path / "frames").glob("*/pages/demo/render_state.json"))
    state = json.loads(state_path.read_text(encoding="utf-8"))
    assert state["manifest_version"] == first.content_version
    assert "generated_at" not in state["visible_snapshot"]
    assert "device_id" not in state["visible_snapshot"]
    assert "memory" not in state["visible_snapshot"]

    restarted = DisplayRenderService(settings)

    def unexpected_capture(_html: str) -> bytes:
        raise AssertionError("服务重启后相同内容不应重新截图")

    restarted._capture = unexpected_capture  # type: ignore[method-assign]
    restored = restarted.render(
        "stable-device",
        "demo",
        {"time": "下午8时"},
    )
    assert restored.content_version == first.content_version


def test_rapid_device_status_changes_reuse_displayed_status(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
        display_refresh_interval_seconds=3600,
    )
    capture_count = 0

    def capture(_html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        return _source_png()

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    first_status = {
        "available": True,
        "temperature_c": 27.0,
        "humidity_percent": 32,
        "battery_percent": 100,
    }
    second_status = {
        **first_status,
        "temperature_c": 27.5,
        "humidity_percent": 33,
        "battery_percent": 99,
    }

    first = service.render(
        "status-stability-device",
        "demo",
        {"time": "上午1时", "device_status": first_status},
    )
    second = service.render(
        "status-stability-device",
        "demo",
        {"time": "上午1时", "device_status": second_status},
    )

    assert second.content_version == first.content_version
    assert capture_count == 1
    state_path = next((tmp_path / "frames").glob("*/pages/demo/render_state.json"))
    state = json.loads(state_path.read_text(encoding="utf-8"))
    assert state["schema_version"] == 3
    assert state["visible_snapshot"]["device_status"] == first_status


def test_device_status_deadbands_apply_after_hourly_hold():
    service = object.__new__(DisplayRenderService)
    service._device_status_min_refresh_seconds = 3600
    previous_status = {
        "available": True,
        "temperature_c": 27.0,
        "humidity_percent": 32,
        "battery_percent": 100,
    }
    render_state = {
        "manifest_version": "20260716-010000",
        "visible_snapshot": {"device_status": previous_status},
    }
    previous = DisplayPageManifest(
        page_id="demo",
        content_version="20260716-010000",
        crc32="00000000",
        sha256="0" * 64,
        payload_sha256="0" * 64,
        created_at=datetime.now(UTC) - timedelta(hours=2),
        frame_url="/api/v2/display/frame/demo/20260716-010000.ppf",
        preview_url="/api/v2/display/preview/demo/20260716-010000.png",
    )
    snapshot = {
        "device_status": {
            "available": True,
            "temperature_c": 27.5,
            "humidity_percent": 34,
            "battery_percent": 96,
        }
    }

    stabilized = service._stabilize_device_status(snapshot, render_state, previous)

    assert stabilized["device_status"] == {
        "available": True,
        "temperature_c": 27.0,
        "humidity_percent": 32,
        "battery_percent": 100,
    }


def test_manifest_reuses_version_and_returns_304_without_recapture(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(_html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        return _source_png()

    render_service = DisplayRenderService(settings)
    render_service._capture = capture  # type: ignore[method-assign]
    app = create_app()
    context_service = SimpleNamespace(
        build=lambda device_id, required_sources=None: {
            "time": "下午8时",
            "mail": {"unread_count": 1, "messages": []},
        }
    )
    app.state.display_render_service = render_service
    app.state.display_refresh_service = DisplayRefreshService(
        SimpleNamespace(display_pages=("demo",), display_default_page="demo"),
        context_service=context_service,
        render_service=render_service,
    )
    client = TestClient(app)
    headers = {"X-Device-Id": "etag-device"}

    first = client.get("/api/v2/display/manifest", headers=headers)
    etag = first.headers["etag"]
    second = client.get(
        "/api/v2/display/manifest",
        headers={**headers, "If-None-Match": etag},
    )
    third = client.get(
        "/api/v2/display/manifest",
        headers={**headers, "If-None-Match": etag},
    )

    assert first.status_code == 200
    assert second.status_code == 304
    assert third.status_code == 304
    assert capture_count == 1
    assert len(list((tmp_path / "frames").glob("*/pages/demo/*.ppf"))) == 1
    page = first.json()["pages"][0]
    frame = client.get(page["frame_url"], headers=headers)
    preview = client.get(page["preview_url"], headers=headers)
    assert frame.status_code == 200
    assert len(frame.content) == 96032
    assert preview.status_code == 200
    assert preview.headers["content-type"] == "image/png"


def test_changed_snapshot_with_same_payload_is_recorded_once(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(_html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        return _source_png()

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    first = service.render("payload-device", "demo", {"time": "下午8时"})
    second = service.render("payload-device", "demo", {"time": "下午9时"})
    third = service.render("payload-device", "demo", {"time": "下午9时"})

    assert second.content_version == first.content_version
    assert third.content_version == first.content_version
    assert capture_count == 2
    state_path = next((tmp_path / "frames").glob("*/pages/demo/render_state.json"))
    state = json.loads(state_path.read_text(encoding="utf-8"))
    assert state["visible_snapshot"]["time"] == "下午9时"


def test_changed_four_gray_payload_publishes_new_version(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)
    service._capture = (  # type: ignore[method-assign]
        lambda html: _solid_png((0, 0, 0, 255))
        if "下午9时" in html
        else _solid_png((255, 255, 255, 255))
    )

    first = service.render("pixel-device", "demo", {"time": "下午8时"})
    second = service.render("pixel-device", "demo", {"time": "下午9时"})

    assert second.content_version != first.content_version
    assert second.payload_sha256 != first.payload_sha256
    assert len(list((tmp_path / "frames").glob("*/pages/demo/*.ppf"))) == 2


def test_template_change_is_processed_once_even_when_pixels_match(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    template_dir = tmp_path / "pages"
    shutil.copytree(project_root / "web" / "pages", template_dir)
    shared_css_path = tmp_path / "epaper.css"
    shutil.copy2(project_root / "web" / "shared" / "epaper.css", shared_css_path)
    settings = SimpleNamespace(
        display_template_dir=template_dir,
        display_output_dir=tmp_path / "frames",
        display_shared_css=shared_css_path,
        display_framework_css=project_root
        / "web"
        / "vendor"
        / "trmnl"
        / "3.1.2"
        / "plugins.min.css.gz",
        display_framework_js=project_root
        / "web"
        / "vendor"
        / "trmnl"
        / "3.1.2"
        / "plugins.min.js.gz",
        display_qweather_icon_dir=project_root
        / "web"
        / "vendor"
        / "qweather-icons"
        / "1.8.0"
        / "icons",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    capture_count = 0

    def capture(_html: str) -> bytes:
        nonlocal capture_count
        capture_count += 1
        return _source_png()

    service = DisplayRenderService(settings)
    service._capture = capture  # type: ignore[method-assign]
    first = service.render("asset-device", "demo", {"time": "下午8时"})
    style_path = template_dir / "demo" / "style.css"
    style_path.write_text(
        style_path.read_text(encoding="utf-8") + "\n/* fingerprint change */\n",
        encoding="utf-8",
    )
    second = service.render("asset-device", "demo", {"time": "下午8时"})
    third = service.render("asset-device", "demo", {"time": "下午8时"})
    shared_css_path.write_text(
        shared_css_path.read_text(encoding="utf-8") + "\n/* shared fingerprint change */\n",
        encoding="utf-8",
    )
    fourth = service.render("asset-device", "demo", {"time": "下午8时"})
    fifth = service.render("asset-device", "demo", {"time": "下午8时"})

    assert second.content_version == first.content_version
    assert third.content_version == first.content_version
    assert fourth.content_version == first.content_version
    assert fifth.content_version == first.content_version
    assert capture_count == 3


def test_visible_snapshot_diff_uses_stable_field_paths():
    previous = {
        "device_status": {"temperature_c": 27.0},
        "mail": {"unread_count": 1},
        "calendar": [],
    }
    current = {
        "device_status": {"temperature_c": 27.5},
        "mail": {"unread_count": 0},
        "calendar": [{"title": "评审"}],
    }

    assert DisplayRenderService._diff_snapshot_values(previous, current) == [
        "calendar",
        "device_status.temperature_c",
        "mail.unread_count",
    ]


def test_template_inlines_local_trmnl_framework(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)

    html = service._build_html("demo", {"device_id": "framework-test"})

    assert 'data-framework="trmnl-3.1.2"' in html
    assert 'data-shared="epaper-ui"' in html
    assert 'data-page="demo"' in html
    assert "--pp-font-weight-min: 700" in html
    assert html.index('data-shared="epaper-ui"') < html.index('data-page="demo"')
    assert "screen--color-6a" in html
    assert 'class="screen screen--og screen--2bit screen--no-bleed"' in html
    assert 'class="layout dashboard"' in html
    assert "pp-canvas" not in html
    assert "pp-panel" not in html
    assert "PHOTO_PAINTER_FRAMEWORK" not in html
    assert "https://trmnl.com" not in html
    assert 'id="device-id"' not in html
    assert "DEVICE ·" not in html
    assert "HTML · TRMNL" not in html
    assert 'id="memory"' not in html
    assert "备忘 MEMORY" not in html
    assert 'id="weather-main-icon"' in html
    assert 'id="weather-mark"' not in html
    assert 'class="device-status grid"' in html
    for framework_class in (
        "flex--between",
        "grid--cols-3",
        "title--base",
        "content--small",
        "label--small",
        "value--tnums",
        "list gap--none",
        "progress-bar--small",
    ):
        assert framework_class in html
    assert 'id="device-temperature"' in html
    assert 'class="quick-weather"' not in html


def test_template_inlines_local_alibaba_puhuiti_font(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    font_path = (
        project_root
        / "web"
        / "vendor"
        / "fonts"
        / "AlibabaPuHuiTi-3-55-Regular.ttf"
    )
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_font_file=font_path,
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)

    html = service._build_html("demo", {"device_id": "font-test"})

    assert 'data-font="alibaba-puhuiti-3"' in html
    assert 'font-family:"PhotoPainter PuHui"' in html
    assert "data:font/ttf;base64," in html
    assert "--font-cn-min:12px" in html
    assert str(font_path) not in html


def test_all_visible_chinese_uses_at_least_twelve_pixels(tmp_path: Path):
    """在真实 Chromium 计算样式中执行中文最小字号规范。"""
    from playwright.sync_api import sync_playwright

    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_font_file=project_root
        / "web"
        / "vendor"
        / "fonts"
        / "AlibabaPuHuiTi-3-55-Regular.ttf",
        display_dither=False,
        display_render_timeout_ms=5000,
    )
    service = DisplayRenderService(settings)
    common = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "time": "下午6时",
        "weather": {
            "city": "苏州",
            "adm1": "江苏省",
            "adm2": "苏州市",
            "observed_at": "2026-07-18T18:00+08:00",
            "text": "阴",
            "icon": "104",
            "temp_c": 33,
            "feels_like_c": 35,
            "humidity_percent": 71,
            "wind_dir": "西风",
            "wind_scale": "3",
            "precip_mm": 0,
            "pressure_hpa": 999,
            "vis_km": 14,
            "air": "优",
            "aqi": 41,
            "air_primary": "",
            "attribution": "QWeather",
            "daily": [
                {
                    "date": f"2026-07-{18 + index:02d}",
                    "text": text,
                    "icon": icon,
                    "min": 27 + index // 5,
                    "max": 35 - index // 2,
                    "uv_index": 6,
                    "sunrise": "05:06",
                    "sunset": "19:03",
                }
                for index, (text, icon) in enumerate(
                    [
                        ("小雨", "305"),
                        ("大雨", "307"),
                        ("中雨", "306"),
                        ("小雨", "305"),
                        ("多云", "101"),
                        ("多云", "101"),
                        ("阴", "104"),
                    ]
                )
            ],
            "minutely": {
                "summary": "70分钟后开始下小雨",
                "points": [
                    {"time": f"2026-07-18T18:{index * 5:02d}+08:00", "precip_mm": index / 100}
                    for index in range(12)
                ],
            },
            "alerts": [{"title": "苏州市气象台发布高温预警", "severity": "moderate"}],
        },
        "moon": {
            "city": "苏州",
            "adm1": "江苏省",
            "adm2": "苏州市",
            "date": "2026-07-18",
            "moonrise": "19:12",
            "moonset": "05:24",
            "updated_text": "17:00",
            "stale": False,
            "attribution": "QWeather",
            "current": {
                "time": "18:00",
                "value": 0.54,
                "name": "亏凸月",
                "illumination": 98,
                "icon": "805",
            },
            "timeline": [
                {
                    "time": f"{hour:02d}:00",
                    "value": 0.54,
                    "name": "亏凸月",
                    "illumination": 98 - index,
                    "icon": "805",
                }
                for index, hour in enumerate((0, 5, 9, 14, 18, 23))
            ],
        },
        "calendar": [
            {"title": "产品方案评审", "relative": "今天 19:30", "location": "会议室"},
            {"title": "周末摄影计划", "relative": "明天 全天", "location": "金鸡湖"},
        ],
        "calendar_month": {
            "year": 2026,
            "month": 7,
            "source": "icloud",
            "event_count": 3,
            "events": [
                {
                    "title": "产品方案评审",
                    "location": "会议室",
                    "start": "2026-07-18T11:30:00Z",
                    "end": "2026-07-18T12:30:00Z",
                    "date": "2026-07-18",
                    "time": "19:30",
                    "all_day": False,
                    "relative": "今天 19:30",
                },
                {
                    "title": "周末摄影计划",
                    "location": "金鸡湖",
                    "start": "2026-07-19",
                    "end": "2026-07-20",
                    "date": "2026-07-19",
                    "time": "",
                    "all_day": True,
                    "relative": "明天 全天",
                },
                {
                    "title": "跨月计划复盘",
                    "location": "线上会议",
                    "start": "2026-08-01T01:00:00Z",
                    "end": "2026-08-01T02:00:00Z",
                    "date": "2026-08-01",
                    "time": "09:00",
                    "all_day": False,
                    "relative": "08-01 09:00",
                },
            ],
        },
        "mail": {
            "unread_count": 2,
            "messages": [
                {"from": "设计协作组", "subject": "新版页面布局建议", "date": "18:12"},
                {"from": "系统通知", "subject": "黑白渲染已经完成", "date": "17:40"},
            ],
        },
        "quota": {
            "available": True,
            "level": "专业版",
            "items": [
                {"label": "每月额度", "remaining": 78},
                {"label": "五小时额度", "remaining": 64},
                {"label": "每周额度", "remaining": 86},
            ],
        },
        "device_status": {
            "temperature_c": 28,
            "humidity_percent": 48,
            "battery_percent": 86,
        },
        "rss": {
            "stale": False,
            "source_count": 1,
            "article_count": 4,
            "updated_text": "18:30",
            "sources": [{"name": "IT之家", "count": 4}],
            "items": [
                {
                    "title": f"RSS 测试文章 {index + 1}",
                    "source": "IT之家",
                    "published_text": "18:30",
                }
                for index in range(4)
            ],
        },
        "availability": {
            "weather": True,
            "moon": True,
            "calendar": True,
            "calendar_month": True,
            "mail": True,
            "quota": True,
            "rss": True,
        },
    }

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 800, "height": 480})
        for template in (
            "demo",
            "calendar",
            "month-calendar",
            "weather",
            "moon",
            "rss",
        ):
            page.set_content(service._build_html(template, common), wait_until="load")
            page.wait_for_function("() => window.__PHOTO_PAINTER_READY__ === true")
            font_state = page.evaluate(
                """async () => {
                    await document.fonts.ready;
                    return {
                      loaded: document.fonts.check('12px "PhotoPainter PuHui"', '体感天气'),
                      family: getComputedStyle(document.body).fontFamily,
                    };
                }"""
            )
            issues = page.evaluate(
                """() => Array.from(document.querySelectorAll('body *')).flatMap((element) => {
                    const hasChinese = Array.from(element.childNodes).some(
                      (node) => node.nodeType === Node.TEXT_NODE && /[\u3400-\u9fff]/.test(node.textContent || ''),
                    );
                    const rect = element.getBoundingClientRect();
                    const style = getComputedStyle(element);
                    if (!hasChinese || rect.width === 0 || rect.height === 0 || style.visibility === 'hidden') return [];
                    const size = Number.parseFloat(style.fontSize);
                    const weight = Number.parseInt(style.fontWeight, 10);
                    return size < 12 || (size <= 12 && weight < 700) ? [{
                      tag: element.tagName,
                      className: element.className?.baseVal || element.className || '',
                      text: (element.textContent || '').trim().slice(0, 30),
                      size,
                      weight,
                    }] : [];
                })"""
            )
            assert font_state["loaded"] is True
            assert "PhotoPainter PuHui" in font_state["family"]
            assert issues == [], f"{template} 存在不符合最小字号或字重的中文: {issues}"
        browser.close()


def test_rss_primary_panel_fills_canvas(tmp_path: Path):
    """不注入业务内容时，RSS 核心区域仍应铺满画布。"""
    from playwright.sync_api import sync_playwright

    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_font_file=project_root
        / "web"
        / "vendor"
        / "fonts"
        / "AlibabaPuHuiTi-3-55-Regular.ttf",
        display_dither=False,
        display_render_timeout_ms=5000,
    )
    service = DisplayRenderService(settings)

    rss_data = {
        "rss": {"items": []},
        "availability": {"rss": True},
    }

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 800, "height": 480})

        page.set_content(service._build_html("rss", rss_data), wait_until="load")
        page.wait_for_function("() => window.__PHOTO_PAINTER_READY__ === true")
        rss_layout = page.evaluate(
            """() => {
                const panel = document.querySelector('.feed-panel').getBoundingClientRect();
                const list = document.querySelector('.story-list').getBoundingClientRect();
                return {
                    hasFeedRail: Boolean(document.querySelector('.feed-rail')),
                    panel: [panel.x, panel.y, panel.width, panel.height],
                    list: [list.x, list.y, list.width, list.height],
                    hasEmptyState: Boolean(document.querySelector('.empty-state')),
                };
            }"""
        )
        assert rss_layout == {
            "hasFeedRail": False,
            "panel": [0, 0, 800, 480],
            "list": [18, 96, 764, 344],
            "hasEmptyState": True,
        }
        browser.close()


def test_weather_dry_rain_state_hides_chart_and_keeps_today_marker_clear(tmp_path: Path):
    """无雨时不画伪降水柱，今天标记与天气文字保持间距。"""
    from playwright.sync_api import sync_playwright

    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_font_file=project_root
        / "web"
        / "vendor"
        / "fonts"
        / "AlibabaPuHuiTi-3-55-Regular.ttf",
        display_dither=False,
        display_render_timeout_ms=5000,
    )
    service = DisplayRenderService(settings)
    context = {
        "date": "2026年07月18日",
        "weekday": "星期六",
        "availability": {"weather": True},
        "qweather_icons": {},
        "weather": {
            "city": "苏州",
            "text": "晴",
            "temp_c": 32,
            "daily": [
                {
                    "date": "2026-07-18",
                    "text": "晴",
                    "icon": "100",
                    "min": 27,
                    "max": 35,
                }
            ],
            "minutely": {
                "summary": "未来两小时无降水",
                "points": [
                    {
                        "time": f"2026-07-18T18:{index * 5:02d}+08:00",
                        "precip_mm": 0,
                    }
                    for index in range(24)
                ],
            },
        },
    }

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 800, "height": 480})
        page.set_content(service._build_html("weather", context))
        page.wait_for_function("window.__PHOTO_PAINTER_READY__ === true")

        assert page.locator("#rain-card").evaluate(
            "node => node.classList.contains('is-dry')"
        )
        assert page.locator("#rain-card").evaluate(
            "node => !node.classList.contains('is-raining')"
        )
        assert page.locator("#rain-summary").text_content() == "未来两小时无降水"
        assert page.locator("#rain-bars").evaluate(
            "node => getComputedStyle(node).display"
        ) == "none"
        assert page.locator(".rain-axis").evaluate(
            "node => getComputedStyle(node).display"
        ) == "none"

        marker_gap = page.locator(".forecast-day.is-today").evaluate(
            """node => {
                const day = node.getBoundingClientRect();
                const text = node.querySelector('.forecast-day-text').getBoundingClientRect();
                return {
                    gap: day.bottom - text.bottom,
                    markerHeight: parseFloat(getComputedStyle(node, '::after').height),
                };
            }"""
        )
        assert marker_gap["gap"] >= 5
        assert marker_gap["markerHeight"] == 3
        browser.close()


def test_template_inlines_qweather_svg_from_api_icon_code(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)

    html = service._build_html(
        "demo",
        {
            "weather": {
                "icon": "100",
                "daily": [{"icon": "305"}],
            }
        },
    )

    assert "qi-100" in html
    assert "qi-305" in html
    assert "cdn.jsdelivr.net/npm/qweather-icons" not in html
    assert service._load_qweather_icon("../../LICENSE") == ""


def test_weather_template_contains_chart_and_all_daily_icons(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)

    html = service._build_html(
        "weather",
        {
            "weather": {
                "icon": "100",
                "daily": [
                    {"icon": code, "min": 20, "max": 30}
                    for code in ("100", "101", "104", "305", "306", "300", "302")
                ],
            },
            "availability": {"weather": True},
        },
    )

    assert "PhotoPainter Weather" in html
    assert 'id="temperature-chart"' in html
    assert "未来2小时" in html
    assert "chart-high-line" in html
    for code in ("100", "101", "104", "305", "306", "300", "302"):
        assert f"qi-{code}" in html


def test_moon_template_embeds_local_phase_icons_and_static_sections(tmp_path: Path):
    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)

    html = service._build_html(
        "moon",
        {
            "date": "2026年07月20日",
            "weekday": "星期一",
            "moon": {
                "city": "昆山",
                "current": {
                    "time": "17:00",
                    "value": 0.54,
                    "name": "亏凸月",
                    "illumination": 98,
                    "icon": "805",
                },
                "timeline": [
                    {
                        "time": "00:00",
                        "name": "亏凸月",
                        "illumination": 99,
                        "icon": "805",
                    }
                ],
            },
            "availability": {"moon": True},
        },
    )

    assert "PhotoPainter Moon" in html
    assert 'id="moon-hero-icon"' in html
    assert 'id="moon-timeline"' in html
    assert "月面照明度" in html
    assert "qi-805" in html
    assert "icons.qweather.com" not in html


def test_moon_template_keeps_unavailable_state_stable(tmp_path: Path):
    from playwright.sync_api import sync_playwright

    project_root = Path(__file__).resolve().parents[1]
    settings = SimpleNamespace(
        display_template_dir=project_root / "web" / "pages",
        display_output_dir=tmp_path / "frames",
        display_dither=False,
        display_render_timeout_ms=1000,
    )
    service = DisplayRenderService(settings)
    html = service._build_html(
        "moon",
        {
            "date": "2026年07月20日",
            "weekday": "星期一",
            "moon": {"attribution": "QWeather"},
            "availability": {"moon": False},
        },
    )

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 800, "height": 480})
        page.set_content(html, wait_until="load")
        page.wait_for_function("() => window.__PHOTO_PAINTER_READY__ === true")
        state = page.evaluate(
            """() => ({
                location: document.getElementById('moon-location').textContent,
                phase: document.getElementById('moon-phase-name').textContent,
                points: document.querySelectorAll('.moon-point').length,
                hasStatus: document.getElementById('moon-status') !== null,
                hasFooter: document.querySelector('.moon-footer') !== null,
                width: document.body.scrollWidth,
                height: document.body.scrollHeight,
            })"""
        )
        browser.close()

    assert state == {
        "location": "月相数据不可用",
        "phase": "暂无月相",
        "points": 6,
        "hasStatus": False,
        "hasFooter": False,
        "width": 800,
        "height": 480,
    }


def test_only_deskmate_dashboard_business_json_api_is_exposed():
    paths = create_app().openapi()["paths"]
    assert "/api/v2/display/render" in paths
    assert "/api/v1/device/status" in paths
    assert "/api/v2/display/status" not in paths
    assert "/api/v1/display/status" not in paths
    assert "/api/v1/ota/check" in paths
    assert "/api/v1/logs/boot" in paths
    assert "/api/v1/voice/chat" in paths
    assert "/api/v1/devices/register" not in paths
    assert "/api/v1/dashboard" in paths
    assert "/api/v1/weather/current" not in paths
    assert "/api/v1/settings" not in paths
    assert "/api/v1/quota" not in paths


def test_display_manifest_auto_renders_live_data_and_supports_etag():
    app = create_app()
    device_id = "first-online-device"
    version = "20260715-195443"
    page = DisplayPageManifest(
        page_id="demo",
        content_version=version,
        crc32="00000000",
        sha256="0" * 64,
        payload_sha256="0" * 64,
        created_at=datetime.now(UTC),
        frame_url=f"/api/v2/display/frame/demo/{version}.ppf",
        preview_url=f"/api/v2/display/preview/demo/{version}.png",
    )
    manifest = DisplayCollectionManifest(
        device_id=device_id,
        collection_version=version,
        default_page="demo",
        pages=[page],
        created_at=datetime.now(UTC),
    )
    calls: list[str] = []

    def build_context(request_device_id: str, required_sources=None) -> dict:
        calls.append(request_device_id)
        return {"device_id": request_device_id}

    def render_collection(**kwargs) -> DisplayCollectionManifest:
        assert kwargs["device_id"] == device_id
        assert kwargs["page_data"] == {"device_id": device_id}
        return manifest

    app.state.display_render_service = SimpleNamespace(
        get_manifest=lambda _device_id, required=False: None,
    )
    def refresh_collection(**kwargs) -> DisplayCollectionManifest:
        page_data = build_context(kwargs["device_id"])
        return render_collection(device_id=kwargs["device_id"], page_data=page_data)

    app.state.display_refresh_service = SimpleNamespace(
        refresh_collection=refresh_collection
    )
    client = TestClient(app)
    response = client.get(
        "/api/v2/display/manifest",
        headers={"X-Device-Id": device_id},
    )
    assert response.status_code == 200
    payload = response.json()
    assert payload["collection_version"] == version
    assert payload["protocol_version"] == 3
    assert "poll_after_seconds" not in payload
    next_refresh_at = payload["next_refresh_at"]
    assert response.headers["etag"] == f'"{version}:{next_refresh_at}"'

    not_modified = client.get(
        "/api/v2/display/manifest",
        headers={"X-Device-Id": device_id, "If-None-Match": response.headers["etag"]},
    )
    assert not_modified.status_code == 304
    assert calls == [device_id, device_id]


def test_display_manifest_refresh_failure_reuses_previous_collection():
    app = create_app()
    device_id = "fallback-device"
    version = "20260715-195443"
    page = DisplayPageManifest(
        page_id="demo",
        content_version=version,
        crc32="00000000",
        sha256="0" * 64,
        payload_sha256="0" * 64,
        created_at=datetime.now(UTC),
        frame_url=f"/api/v2/display/frame/demo/{version}.ppf",
        preview_url=f"/api/v2/display/preview/demo/{version}.png",
    )
    previous = DisplayCollectionManifest(
        device_id=device_id,
        collection_version=version,
        default_page="demo",
        pages=[page],
        created_at=datetime.now(UTC),
    )
    app.state.display_render_service = SimpleNamespace(
        get_manifest=lambda _device_id, required=False: previous
    )

    def fail_refresh(**kwargs):
        raise RuntimeError("数据源超时")

    app.state.display_refresh_service = SimpleNamespace(
        refresh_collection=fail_refresh
    )

    response = TestClient(app).get(
        "/api/v2/display/manifest",
        headers={"X-Device-Id": device_id},
    )

    assert response.status_code == 200
    assert response.json()["collection_version"] == version
    next_refresh_at = response.json()["next_refresh_at"]
    assert response.headers["etag"] == f'"{version}:{next_refresh_at}"'


def test_display_time_uses_chinese_hour_without_minutes():
    assert DisplayContextService._format_display_hour(
        datetime(2026, 7, 15, 10, 37),
    ) == "上午10时"
    assert DisplayContextService._format_display_hour(
        datetime(2026, 7, 15, 20, 5),
    ) == "下午8时"


def test_device_status_rounding_ignores_small_raw_sensor_changes():
    def status(temperature: float, humidity: float):
        return SimpleNamespace(
            environment=SimpleNamespace(
                temperature_c=temperature,
                humidity_percent=humidity,
            ),
            battery=SimpleNamespace(
                percent=100.0,
            ),
        )

    first = DisplayContextService._format_device_status(status(27.03, 35.98))
    second = DisplayContextService._format_device_status(status(27.04, 35.89))

    assert first == second
    assert first["temperature_c"] == 27.0
    assert first["humidity_percent"] == 36


def test_live_context_uses_internal_real_data_services():
    weather = SimpleNamespace(
        location=SimpleNamespace(city="苏州"),
        now=SimpleNamespace(
            text="晴",
            icon="100",
            temp_c=28,
            feels_like_c=29,
            humidity_percent=60,
        ),
        air=SimpleNamespace(category="优"),
        daily=SimpleNamespace(items=[]),
    )
    moon = SimpleNamespace(
        error="",
        stale=False,
        attribution="QWeather",
        updated_at=datetime.fromisoformat("2026-07-20T05:00+08:00"),
        location=SimpleNamespace(city="苏州", adm1="江苏省", adm2="苏州市"),
        fx_date="2026-07-20",
        moonrise="2026-07-20T19:12+08:00",
        moonset="2026-07-21T05:24+08:00",
        phases=[
            SimpleNamespace(
                fx_time="2026-07-20T12:00+08:00",
                value=0.54,
                name="亏凸月",
                illumination_percent=98,
                icon="805",
            )
        ],
    )
    calendar = SimpleNamespace(source="icloud", error="", items=[])
    mail = SimpleNamespace(unread_count=2, messages=[])
    quota = SimpleNamespace(available=True, level="Pro", limits=[])
    rss = SimpleNamespace(available=True, stale=False, feeds=[], items=[])
    memory = SimpleNamespace(query_memory=lambda device_id, query: "- 真实记忆")
    device_status = SimpleNamespace(
        environment=SimpleNamespace(temperature_c=28.58, humidity_percent=35.89),
        battery=SimpleNamespace(percent=99.6),
    )
    service = DisplayContextService(
        SimpleNamespace(
            display_default_timezone="Asia/Shanghai",
            display_default_city="苏州",
        ),
        weather_service=SimpleNamespace(
            get_current_weather=lambda city: weather,
            get_moon_phase=lambda city, date: moon,
        ),
        calendar_service=SimpleNamespace(
            get_upcoming_events=lambda timezone: calendar,
            get_month_events=lambda timezone: calendar,
        ),
        mail_service=SimpleNamespace(get_mail_summary=lambda timezone: mail),
        quota_service=SimpleNamespace(check_glm=lambda: quota),
        memory_service=memory,
        device_status_service=SimpleNamespace(get=lambda device_id: device_status),
        rss_service=SimpleNamespace(get_latest_articles=lambda timezone: rss),
    )

    context = service.build("real-device")

    assert context["weather"]["city"] == "苏州"
    assert context["weather"]["icon"] == "100"
    assert context["mail"]["unread_count"] == 2
    assert context["quota"]["level"] == "Pro"
    assert context["memory"] == ["真实记忆"]
    assert context["availability"] == {
        "weather": True,
        "moon": True,
        "calendar": True,
        "calendar_month": True,
        "mail": True,
        "quota": True,
        "rss": True,
    }
    assert context["device_status"] == {
        "available": True,
        "temperature_c": 28.5,
        "humidity_percent": 36,
        "battery_percent": 100,
    }


def test_live_context_marks_failed_sources_unavailable():
    weather = SimpleNamespace(
        error="QWeather timeout",
        location=SimpleNamespace(city="苏州"),
        now=SimpleNamespace(
            text="",
            icon="",
            temp_c=None,
            feels_like_c=None,
            humidity_percent=None,
        ),
        air=None,
        daily=SimpleNamespace(items=[]),
    )
    moon = SimpleNamespace(
        error="QWeather timeout",
        stale=False,
        attribution="QWeather",
        updated_at=datetime.fromisoformat("2026-07-20T05:00+08:00"),
        location=SimpleNamespace(city="苏州", adm1="江苏省", adm2="苏州市"),
        fx_date="2026-07-20",
        moonrise="",
        moonset="",
        phases=[],
    )
    calendar = SimpleNamespace(source="mock", error="CalDAV timeout", items=[])
    mail = SimpleNamespace(error="IMAP timeout", unread_count=0, messages=[])
    quota = SimpleNamespace(available=False, level=None, limits=[])
    rss = SimpleNamespace(available=False, stale=False, feeds=[], items=[])
    service = DisplayContextService(
        SimpleNamespace(
            display_default_timezone="Asia/Shanghai",
            display_default_city="苏州",
        ),
        weather_service=SimpleNamespace(
            get_current_weather=lambda city: weather,
            get_moon_phase=lambda city, date: moon,
        ),
        calendar_service=SimpleNamespace(
            get_upcoming_events=lambda timezone: calendar,
            get_month_events=lambda timezone: calendar,
        ),
        mail_service=SimpleNamespace(get_mail_summary=lambda timezone: mail),
        quota_service=SimpleNamespace(check_glm=lambda: quota),
        memory_service=SimpleNamespace(query_memory=lambda device_id, query: ""),
        device_status_service=SimpleNamespace(get=lambda device_id: None),
        rss_service=SimpleNamespace(get_latest_articles=lambda timezone: rss),
    )

    context = service.build("failed-device")

    assert context["availability"] == {
        "weather": False,
        "moon": False,
        "calendar": False,
        "calendar_month": False,
        "mail": False,
        "quota": False,
        "rss": False,
    }
