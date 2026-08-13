"""按设备串行协调显示数据聚合、多页面集合刷新与绝对刷新时间发布。"""

from collections.abc import Sequence
from datetime import UTC, datetime, time, timedelta
from threading import Lock, RLock
from zoneinfo import ZoneInfo

from app.schemas.display import DisplayCollectionManifest, DisplayScheduledManifest
from app.workflows.display.pages import (
    required_sources_for_pages,
    validate_page_set,
)


class DisplayRefreshService:
    """由设备请求驱动刷新，不创建后台调度任务。"""

    def __init__(self, settings, context_service, render_service) -> None:
        self._context = context_service
        self._render = render_service
        self._default_pages = tuple(settings.display_pages)
        self._default_page = settings.display_default_page
        self._locks_guard = Lock()
        self._device_locks: dict[str, RLock] = {}

    def refresh_collection(
        self,
        device_id: str,
        pages: list[str] | tuple[str, ...] | None = None,
        default_page: str | None = None,
        dither: bool | None = None,
    ) -> DisplayCollectionManifest:
        """统一取数并原子刷新指定设备的多页面集合。"""
        page_ids = tuple(self._default_pages if pages is None else pages)
        selected_default = self._default_page if default_page is None else default_page
        validate_page_set(page_ids, selected_default)
        required_sources = required_sources_for_pages(page_ids)

        with self._lock_for(device_id):
            page_data = self._context.build(
                device_id,
                required_sources=required_sources,
            )
            return self._render.render_collection(
                device_id=device_id,
                page_data=page_data,
                pages=page_ids,
                default_page=selected_default,
                dither=dither,
            )

    def _lock_for(self, device_id: str) -> RLock:
        """惰性创建并返回设备独享的进程内重入锁。"""
        with self._locks_guard:
            lock = self._device_locks.get(device_id)
            if lock is None:
                lock = RLock()
                self._device_locks[device_id] = lock
            return lock


def schedule_display_manifest(
    manifest: DisplayCollectionManifest,
    interval_seconds: int,
    now: datetime | None = None,
    *,
    daily_times: Sequence[time] = (),
    timezone_name: str = "UTC",
) -> DisplayScheduledManifest:
    """按每日时间表或兼容周期返回下一次绝对刷新时间。"""
    next_refresh_at = next_refresh_at_utc(
        interval_seconds,
        now,
        daily_times=daily_times,
        timezone_name=timezone_name,
    )
    return DisplayScheduledManifest.model_validate(
        {**manifest.model_dump(), "next_refresh_at": next_refresh_at}
    )


def next_refresh_at_utc(
    interval_seconds: int,
    now: datetime | None = None,
    *,
    daily_times: Sequence[time] = (),
    timezone_name: str = "UTC",
) -> int:
    """按共享显示时间表计算严格晚于当前时刻的 UTC Unix 时间戳。"""
    current = datetime.now(UTC) if now is None else now.astimezone(UTC)
    if daily_times:
        return _next_daily_refresh_at(
            current,
            daily_times,
            ZoneInfo(timezone_name),
        )
    current_timestamp = int(current.timestamp())
    interval_seconds = max(60, int(interval_seconds))
    return (current_timestamp // interval_seconds + 1) * interval_seconds


def _next_daily_refresh_at(
    current: datetime,
    daily_times: Sequence[time],
    timezone: ZoneInfo,
) -> int:
    """查找当前时刻之后最近的有效本地计划时间。"""
    local_now = current.astimezone(timezone)
    scheduled_times = sorted(set(daily_times))
    for day_offset in range(8):
        local_date = local_now.date() + timedelta(days=day_offset)
        for scheduled_time in scheduled_times:
            candidate_local = datetime.combine(
                local_date,
                scheduled_time,
                tzinfo=timezone,
            )
            candidate_utc = candidate_local.astimezone(UTC)
            normalized = candidate_utc.astimezone(timezone)
            if (
                normalized.date() != local_date
                or normalized.time().replace(tzinfo=None) != scheduled_time
            ):
                continue
            if candidate_utc > current:
                return int(candidate_utc.timestamp())
    raise ValueError("未来七天内没有有效的每日刷新时间")
