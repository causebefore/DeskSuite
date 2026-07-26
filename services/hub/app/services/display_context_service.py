"""聚合网页渲染所需的数据，不向 ESP32 暴露业务 JSON。"""

from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from zoneinfo import ZoneInfo

from loguru import logger

from app.services.display_page_registry import ALL_DATA_SOURCE_KEYS


class DisplayContextService:
    """把外部数据源转换为稳定的模板上下文。"""

    def __init__(
        self,
        settings,
        weather_service,
        calendar_service,
        mail_service,
        quota_service,
        memory_service,
        device_status_service,
        rss_service,
    ) -> None:
        self._settings = settings
        self._weather = weather_service
        self._calendar = calendar_service
        self._mail = mail_service
        self._quota = quota_service
        self._memory = memory_service
        self._device_status = device_status_service
        self._rss = rss_service

    def build(
        self,
        device_id: str,
        required_sources: frozenset[str] | set[str] | None = None,
    ) -> dict:
        """按需聚合数据；未指定数据源时保留原有全量行为。"""
        timezone_name = self._settings.display_default_timezone
        now = datetime.now(ZoneInfo(timezone_name))
        requested = (
            ALL_DATA_SOURCE_KEYS
            if required_sources is None
            else frozenset(required_sources)
        )
        unknown = requested - ALL_DATA_SOURCE_KEYS
        if unknown:
            raise ValueError(f"未知显示数据源: {', '.join(sorted(unknown))}")

        loaders: dict[str, tuple[object, tuple]] = {}
        if "weather" in requested:
            loaders["weather"] = (
                self._weather.get_current_weather,
                (self._settings.display_default_city,),
            )
        if "moon" in requested:
            loaders["moon"] = (
                self._weather.get_moon_phase,
                (
                    self._settings.display_default_city,
                    now.strftime("%Y%m%d"),
                ),
            )
        if "calendar_month" in requested:
            # 月历时间窗覆盖当前自然月和月末未来 range_days 天；同时配置日程页时
            # 直接从这份结果裁剪近期日程，避免同一次刷新重复登录 CalDAV。
            loaders["calendar_month"] = (
                self._calendar.get_month_events,
                (timezone_name,),
            )
        elif "calendar" in requested:
            loaders["calendar"] = (
                self._calendar.get_upcoming_events,
                (timezone_name,),
            )
        if "mail" in requested:
            loaders["mail"] = (self._mail.get_mail_summary, (timezone_name,))
        if "quota" in requested:
            loaders["quota"] = (self._quota.check_glm, ())
        if "memory" in requested:
            loaders["memory"] = (
                self._memory.query_memory,
                (
                    device_id,
                    "适合显示在桌面画面中的近期重要事项、偏好和提醒",
                ),
            )
        if "rss" in requested:
            loaders["rss"] = (self._rss.get_latest_articles, (timezone_name,))
        async_sources = sorted(loaders)
        results: dict[str, object] = {}
        if async_sources:
            # 数据源彼此独立，并行回源可把整页等待时间压缩到最慢的一项。
            with ThreadPoolExecutor(
                max_workers=len(async_sources),
                thread_name_prefix="display-data",
            ) as executor:
                futures = {
                    source: executor.submit(loaders[source][0], *loaders[source][1])
                    for source in async_sources
                }
                results = {source: future.result() for source, future in futures.items()}

        context = {
            "device_id": device_id,
            "generated_at": now.isoformat(),
            "date": now.strftime("%Y年%m月%d日"),
            "weekday": "星期" + "一二三四五六日"[now.weekday()],
            "time": self._format_display_hour(now),
            "availability": {},
        }
        availability = context["availability"]

        if "device_status" in requested:
            context["device_status"] = self._format_device_status(
                self._device_status.get(device_id)
            )
        if "weather" in requested:
            weather = results["weather"]
            availability["weather"] = not bool(getattr(weather, "error", ""))
            weather_now = weather.now
            weather_air = getattr(weather, "air", None)
            minutely = getattr(weather, "minutely", None)
            context["weather"] = {
                "city": weather.location.city,
                "adm1": getattr(weather.location, "adm1", ""),
                "adm2": getattr(weather.location, "adm2", ""),
                "observed_at": getattr(weather_now, "obs_time", ""),
                "text": weather_now.text or "--",
                "icon": getattr(weather_now, "icon", ""),
                "temp_c": weather_now.temp_c,
                "feels_like_c": weather_now.feels_like_c,
                "humidity_percent": weather_now.humidity_percent,
                "wind_dir": getattr(weather_now, "wind_dir", ""),
                "wind_scale": getattr(weather_now, "wind_scale", ""),
                "precip_mm": getattr(weather_now, "precip_mm", None),
                "pressure_hpa": getattr(weather_now, "pressure_hpa", None),
                "vis_km": getattr(weather_now, "vis_km", None),
                # air 保留等级字符串供 demo 使用，天气专页额外消费 AQI 数值。
                "air": weather_air.category if weather_air else "--",
                "aqi": getattr(weather_air, "aqi", None) if weather_air else None,
                "air_primary": getattr(weather_air, "primary", "")
                if weather_air
                else "",
                "attribution": getattr(weather, "attribution", "QWeather"),
                "daily": [
                    {
                        "date": item.fx_date,
                        "text": item.text_day or item.text_night,
                        "text_day": item.text_day,
                        "text_night": item.text_night,
                        "icon": item.icon_day or item.icon_night,
                        "icon_day": item.icon_day,
                        "icon_night": item.icon_night,
                        "min": item.temp_min_c,
                        "max": item.temp_max_c,
                        "wind_dir": item.wind_dir_day,
                        "wind_scale": item.wind_scale_day,
                        "humidity_percent": item.humidity_percent,
                        "precip_mm": item.precip_mm,
                        "uv_index": item.uv_index,
                        "sunrise": item.sunrise,
                        "sunset": item.sunset,
                    }
                    for item in weather.daily.items[:7]
                ],
                "minutely": {
                    "summary": getattr(minutely, "summary", ""),
                    "points": [
                        {
                            "time": item.fx_time,
                            "precip_mm": item.precip_mm,
                            "type": item.rain_type,
                        }
                        for item in getattr(minutely, "items", [])[:24]
                    ],
                },
                "alerts": [
                    {
                        "title": alert.title,
                        "type": alert.alert_type,
                        "severity": alert.severity,
                        "text": alert.text,
                        "start_time": alert.start_time,
                        "end_time": alert.end_time,
                    }
                    for alert in getattr(weather, "alerts", [])[:3]
                ],
            }
        if "moon" in requested:
            moon = results["moon"]
            phase_items = list(getattr(moon, "phases", []))
            current_phase = self._nearest_moon_phase(phase_items, now)
            timeline = self._sample_moon_phases(phase_items, 6)
            stale = bool(getattr(moon, "stale", False))
            availability["moon"] = bool(phase_items) and (
                stale or not bool(getattr(moon, "error", ""))
            )
            context["moon"] = {
                "city": moon.location.city,
                "adm1": getattr(moon.location, "adm1", ""),
                "adm2": getattr(moon.location, "adm2", ""),
                "date": getattr(moon, "fx_date", ""),
                "moonrise": self._format_iso_clock(getattr(moon, "moonrise", "")),
                "moonset": self._format_iso_clock(getattr(moon, "moonset", "")),
                "updated_text": self._format_iso_clock(
                    getattr(moon, "updated_at", "")
                ),
                "stale": stale,
                "attribution": getattr(moon, "attribution", "QWeather"),
                "current": self._moon_phase_context(current_phase),
                "timeline": [self._moon_phase_context(item) for item in timeline],
            }
        if "calendar" in requested:
            calendar = (
                results["calendar"]
                if "calendar" in results
                else results["calendar_month"]
            )
            availability["calendar"] = not bool(getattr(calendar, "error", ""))
            calendar_items = calendar.items
            if "calendar" not in results:
                today = now.strftime("%Y-%m-%d")
                calendar_items = [
                    item
                    for item in calendar.items
                    if not getattr(item, "date", "") or item.date >= today
                ]
            context["calendar"] = [
                {
                    "title": item.title,
                    "relative": item.relative,
                    "location": item.location,
                }
                for item in calendar_items[:4]
            ]
        if "calendar_month" in requested:
            calendar_month = results["calendar_month"]
            availability["calendar_month"] = not bool(
                getattr(calendar_month, "error", "")
            )
            context["calendar_month"] = {
                "year": now.year,
                "month": now.month,
                "source": calendar_month.source,
                "event_count": len(calendar_month.items),
                "events": [
                    {
                        "title": item.title,
                        "location": item.location,
                        "start": item.start,
                        "end": item.end,
                        "date": item.date,
                        "time": item.time,
                        "all_day": item.all_day,
                        "relative": item.relative,
                    }
                    for item in calendar_month.items
                ],
            }
        if "mail" in requested:
            mail = results["mail"]
            availability["mail"] = not bool(getattr(mail, "error", ""))
            context["mail"] = {
                "unread_count": mail.unread_count,
                "messages": [
                    {
                        "from": item.from_name,
                        "subject": item.subject,
                        "date": item.date_text,
                    }
                    for item in mail.messages[:3]
                ],
            }
        if "quota" in requested:
            quota = results["quota"]
            availability["quota"] = bool(quota.available)
            context["quota"] = {
                "available": quota.available,
                "level": quota.level or "--",
                "items": [
                    {
                        "label": item.display_name or item.type,
                        "remaining": round(item.remaining_percent),
                    }
                    for item in quota.limits[:3]
                ],
            }
        if "memory" in requested:
            context["memory"] = self._memory_lines(results["memory"])
        if "rss" in requested:
            rss = results["rss"]
            availability["rss"] = bool(rss.available)
            context["rss"] = {
                "stale": bool(rss.stale),
                "source_count": len(rss.feeds),
                "article_count": len(rss.items),
                "updated_text": rss.items[0].published_text if rss.items else "--",
                "sources": [
                    {"name": feed.title, "count": feed.item_count}
                    for feed in rss.feeds[:5]
                ],
                "items": [
                    {
                        "title": item.title,
                        "source": item.source,
                        "published_text": item.published_text,
                    }
                    for item in rss.items[:4]
                ],
            }

        logger.info(
            "显示数据聚合完成: device={} sources=[{}]",
            device_id,
            ", ".join(sorted(requested)),
        )
        return context

    @staticmethod
    def _format_device_status(status) -> dict:
        """把设备原始平均值压缩成稳定的页面显示值。"""
        if status is None:
            return {
                "available": False,
                "temperature_c": None,
                "humidity_percent": None,
                "battery_percent": None,
            }
        environment = status.environment
        return {
            "available": True,
            "temperature_c": round(environment.temperature_c * 2.0) / 2.0
            if environment is not None
            else None,
            "humidity_percent": round(environment.humidity_percent)
            if environment is not None
            else None,
            "battery_percent": round(status.battery.percent),
        }

    @staticmethod
    def _format_display_hour(value: datetime) -> str:
        """把时间格式化为不含分钟的中文 12 小时制。"""
        period = "上午" if value.hour < 12 else "下午"
        hour = value.hour % 12 or 12
        return f"{period}{hour}时"

    @staticmethod
    def _format_iso_clock(value) -> str:
        """把 ISO 时间压缩为稳定的 HH:MM；空值保持占位。"""
        if isinstance(value, datetime):
            return value.strftime("%H:%M")
        if not value:
            return "--"
        try:
            return datetime.fromisoformat(str(value)).strftime("%H:%M")
        except ValueError:
            text = str(value)
            return text[11:16] if len(text) >= 16 else "--"

    @staticmethod
    def _nearest_moon_phase(items: list, now: datetime):
        """选择最接近当前设备时区时刻的逐小时月相。"""
        candidates = []
        for item in items:
            try:
                fx_time = datetime.fromisoformat(str(item.fx_time))
                candidates.append((abs((fx_time - now).total_seconds()), item))
            except (TypeError, ValueError):
                continue
        return min(candidates, key=lambda entry: entry[0])[1] if candidates else (
            items[0] if items else None
        )

    @staticmethod
    def _sample_moon_phases(items: list, count: int) -> list:
        """等距抽取全天月相点，避免 24 个点挤压 800px 横轴。"""
        if len(items) <= count:
            return items
        indexes = [
            round(index * (len(items) - 1) / (count - 1))
            for index in range(count)
        ]
        return [items[index] for index in indexes]

    @classmethod
    def _moon_phase_context(cls, item) -> dict:
        """裁剪单个月相点，只保留页面实际可见字段。"""
        if item is None:
            return {
                "time": "--",
                "value": None,
                "name": "--",
                "illumination": None,
                "icon": "",
            }
        value = getattr(item, "value", None)
        illumination = getattr(item, "illumination_percent", None)
        return {
            "time": cls._format_iso_clock(getattr(item, "fx_time", "")),
            "value": round(value, 2) if value is not None else None,
            "name": getattr(item, "name", "") or "--",
            "illumination": round(illumination)
            if illumination is not None
            else None,
            "icon": getattr(item, "icon", ""),
        }

    @staticmethod
    def _memory_lines(memory_text: str) -> list[str]:
        """把 mem0 多行文本整理为页面列表。"""
        return [
            line.removeprefix("- ").strip()
            for line in memory_text.splitlines()
            if line.strip()
        ][:3]
