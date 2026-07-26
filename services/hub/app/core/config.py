"""从 config.toml 与 .env 加载服务端运行配置。"""

from functools import lru_cache
from pathlib import Path
from typing import Any
import os
import re
import tomllib
from datetime import time
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG_PATH = PROJECT_ROOT / "config.toml"
DEFAULT_ENV_PATH = PROJECT_ROOT / ".env"


def _read_env_file(env_path: Path) -> dict[str, str]:
    """读取简单 KEY=VALUE 格式的密钥文件。"""
    if not env_path.exists():
        return {}
    values: dict[str, str] = {}
    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def _read_toml(config_path: Path) -> dict[str, Any]:
    if not config_path.exists():
        raise FileNotFoundError(f"配置文件不存在: {config_path}")
    with config_path.open("rb") as handle:
        return tomllib.load(handle)


def _section(data: dict[str, Any], name: str) -> dict[str, Any]:
    section = data.get(name)
    if not isinstance(section, dict):
        raise KeyError(f"config.toml 缺少 [{name}] 配置段")
    return section


def _nested_section(data: dict[str, Any], first: str, second: str) -> dict[str, Any]:
    return _section(_section(data, first), second)


def _path_from_root(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def _parse_daily_times(value: Any) -> tuple[time, ...]:
    """校验并解析每日刷新时间，内部按时间先后排序。"""
    if value is None:
        return ()
    if not isinstance(value, list) or not value or len(value) > 48:
        raise ValueError(
            "display.refresh_schedule.daily_times 必须是包含 1 到 48 个时间的数组"
        )
    if any(
        not isinstance(item, str)
        or re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", item) is None
        for item in value
    ):
        raise ValueError(
            "display.refresh_schedule.daily_times 必须使用 HH:MM 格式"
        )
    if len(set(value)) != len(value):
        raise ValueError("display.refresh_schedule.daily_times 不能包含重复时间")
    return tuple(sorted(time.fromisoformat(item) for item in value))


class ServerSettings:
    """服务端运行配置；普通配置来自 TOML，密钥来自环境变量或 .env。"""

    def __init__(
        self,
        config_path: Path | str = DEFAULT_CONFIG_PATH,
        env_path: Path | str = DEFAULT_ENV_PATH,
        project_root: Path | str = PROJECT_ROOT,
    ) -> None:
        self.project_root = Path(project_root)
        self.config_path = Path(config_path)
        self.env_path = Path(env_path)
        data = _read_toml(self.config_path)
        secrets = _read_env_file(self.env_path)

        app = _section(data, "app")
        server = _section(data, "server")
        providers = _section(data, "providers")
        dashboard = _section(data, "dashboard")
        zhipu = _section(data, "zhipu")
        qweather = _section(data, "qweather")
        weather_cache = _nested_section(data, "weather", "cache")
        caldav = _section(data, "caldav")
        caldav_cache = _nested_section(data, "caldav", "cache")
        imap = _section(data, "imap")
        imap_cache = _nested_section(data, "imap", "cache")
        rss = data.get("rss") or {}
        if not isinstance(rss, dict):
            raise ValueError("config.toml [rss] 必须是配置段")
        display = _section(data, "display")
        display_defaults = _nested_section(data, "display", "defaults")
        display_refresh_schedule = display.get("refresh_schedule", {})
        if not isinstance(display_refresh_schedule, dict):
            raise ValueError("config.toml [display.refresh_schedule] 必须是配置段")
        storage = _section(data, "storage")

        self.app_title = str(app["title"])
        self.app_version = str(app["version"])
        self.app_description = str(app["description"])
        self.server_host = str(server["host"])
        self.server_port = int(server["port"])
        self.server_log_level = str(server["log_level"])

        self.weather_provider = str(providers["weather"]).lower()
        self.calendar_provider = str(providers["calendar"]).lower()
        self.mail_provider = str(providers["mail"]).lower()
        self.dashboard_source_timeout_seconds = float(
            dashboard["source_timeout_seconds"]
        )
        if not 1 <= self.dashboard_source_timeout_seconds <= 15:
            raise ValueError(
                "dashboard.source_timeout_seconds 必须在 1 到 15 秒之间"
            )

        self.zhipu_api_key = os.getenv(
            "ZHIPU_API_KEY", secrets.get("ZHIPU_API_KEY", "")
        )
        self.device_api_token = os.getenv(
            "DEVICE_API_TOKEN", secrets.get("DEVICE_API_TOKEN", "")
        )
        self.qweather_api_key = os.getenv(
            "QWEATHER_API_KEY", secrets.get("QWEATHER_API_KEY", "")
        )
        self.caldav_username = os.getenv(
            "CALDAV_USERNAME", secrets.get("CALDAV_USERNAME", "")
        )
        self.caldav_password = os.getenv(
            "CALDAV_PASSWORD", secrets.get("CALDAV_PASSWORD", "")
        )
        self.imap_username = os.getenv(
            "IMAP_USERNAME", secrets.get("IMAP_USERNAME", "")
        )
        self.imap_password = os.getenv(
            "IMAP_PASSWORD", secrets.get("IMAP_PASSWORD", "")
        )

        self.zhipu_llm_model = str(zhipu["llm_model"])
        self.zhipu_asr_model = str(zhipu["asr_model"])
        self.zhipu_tts_model = str(zhipu["tts_model"])
        self.zhipu_tts_voice = str(zhipu["tts_voice"])
        self.quota_cache_seconds = int(zhipu.get("quota_cache_seconds", 300))

        self.qweather_host = str(qweather["host"])
        self.qweather_timeout_seconds = float(qweather["timeout_seconds"])
        self.qweather_city_lookup_path = str(qweather["city_lookup_path"])
        self.qweather_now_path = str(qweather["now_path"])
        self.qweather_daily_path = str(qweather["daily_path"])
        self.qweather_daily_days = str(qweather["daily_days"])
        self.qweather_minutely_path = str(qweather["minutely_path"])
        self.qweather_alert_path = str(qweather["alert_path"])
        self.qweather_air_path = str(qweather["air_path"])
        self.qweather_moon_path = str(qweather["moon_path"])
        self.weather_location_cache_seconds = int(weather_cache["location_seconds"])
        self.weather_now_cache_seconds = int(weather_cache["now_seconds"])
        self.weather_daily_cache_seconds = int(weather_cache["daily_seconds"])
        self.weather_minutely_cache_seconds = int(weather_cache["minutely_seconds"])
        self.weather_alert_cache_seconds = int(weather_cache["alert_seconds"])
        self.weather_air_cache_seconds = int(weather_cache["air_seconds"])
        self.weather_moon_cache_seconds = int(weather_cache["moon_seconds"])

        self.caldav_url = str(caldav["url"])
        self.caldav_timeout_seconds = float(caldav["timeout_seconds"])
        self.caldav_range_days = int(caldav["range_days"])
        self.caldav_max_events = int(caldav["max_events"])
        self.caldav_month_max_events = int(caldav.get("month_max_events", 100))
        if not 1 <= self.caldav_month_max_events <= 500:
            raise ValueError("caldav.month_max_events 必须在 1 到 500 之间")
        self.caldav_events_cache_seconds = int(caldav_cache["events_seconds"])
        self.imap_host = str(imap["host"])
        self.imap_port = int(imap["port"])
        self.imap_use_ssl = bool(imap["use_ssl"])
        self.imap_timeout_seconds = float(imap["timeout_seconds"])
        self.imap_max_messages = int(imap["max_messages"])
        self.imap_inbox_cache_seconds = int(imap_cache["inbox_seconds"])

        rss_feeds = rss.get("feeds", [])
        if (
            not isinstance(rss_feeds, list)
            or len(rss_feeds) > 8
            or any(not isinstance(url, str) or not url.strip() for url in rss_feeds)
            or len(set(rss_feeds)) != len(rss_feeds)
        ):
            raise ValueError("rss.feeds 必须是最多包含 8 个唯一 URL 的数组")
        self.rss_enabled = bool(rss.get("enabled", bool(rss_feeds)))
        self.rss_feed_urls = tuple(url.strip() for url in rss_feeds)
        self.rss_timeout_seconds = float(rss.get("timeout_seconds", 8))
        self.rss_cache_seconds = int(rss.get("cache_seconds", 900))
        self.rss_max_items_per_feed = int(rss.get("max_items_per_feed", 12))
        self.rss_max_items = int(rss.get("max_items", 24))
        if not 1 <= self.rss_timeout_seconds <= 30:
            raise ValueError("rss.timeout_seconds 必须在 1 到 30 秒之间")
        if self.rss_cache_seconds < 60:
            raise ValueError("rss.cache_seconds 不能小于 60")
        if not 1 <= self.rss_max_items_per_feed <= 50:
            raise ValueError("rss.max_items_per_feed 必须在 1 到 50 之间")
        if not 1 <= self.rss_max_items <= 100:
            raise ValueError("rss.max_items 必须在 1 到 100 之间")

        self.display_template_dir = _path_from_root(
            self.project_root, str(display["template_dir"])
        )
        self.display_output_dir = _path_from_root(
            self.project_root, str(display["output_dir"])
        )
        self.display_framework_css = _path_from_root(
            self.project_root,
            str(
                display.get(
                    "framework_css",
                    "web/vendor/trmnl/3.1.2/plugins.min.css.gz",
                )
            ),
        )
        self.display_framework_js = _path_from_root(
            self.project_root,
            str(
                display.get(
                    "framework_js",
                    "web/vendor/trmnl/3.1.2/plugins.min.js.gz",
                )
            ),
        )
        self.display_shared_css = _path_from_root(
            self.project_root,
            str(display.get("shared_css", "web/shared/epaper.css")),
        )
        self.display_font_file = _path_from_root(
            self.project_root,
            str(
                display.get(
                    "font_file",
                    "web/vendor/fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
                )
            ),
        )
        display_pages = display.get("pages", [display.get("default_template", "demo")])
        if (
            not isinstance(display_pages, list)
            or not display_pages
            or len(display_pages) > 16
            or any(not isinstance(page, str) or not page for page in display_pages)
            or len(set(display_pages)) != len(display_pages)
        ):
            raise ValueError("display.pages 必须是包含 1 到 16 个唯一页面名的数组")
        self.display_pages = tuple(display_pages)
        self.display_default_page = str(
            display.get("default_page", self.display_pages[0])
        )
        if self.display_default_page not in self.display_pages:
            raise ValueError("display.default_page 必须包含在 display.pages 中")
        # 兼容尚未迁移的内部调用；新代码统一使用 display_default_page。
        self.display_default_template = self.display_default_page
        self.display_dither = bool(display.get("dither", False))
        self.display_render_timeout_ms = int(display.get("render_timeout_ms", 10000))
        self.display_keep_versions = int(display.get("keep_versions", 4))
        self.display_refresh_interval_seconds = int(
            display.get("refresh_interval_seconds", 3600)
        )
        if self.display_refresh_interval_seconds < 60:
            raise ValueError("display.refresh_interval_seconds 不能小于 60")
        self.display_device_status_min_refresh_seconds = int(
            display.get(
                "device_status_min_refresh_seconds",
                self.display_refresh_interval_seconds,
            )
        )
        if self.display_device_status_min_refresh_seconds < 60:
            raise ValueError("display.device_status_min_refresh_seconds 不能小于 60")
        self.display_default_city = str(display_defaults["city"])
        self.display_default_timezone = str(display_defaults["timezone"])
        self.display_default_device_id = str(display_defaults.get("device_id", "default"))
        self.display_refresh_schedule_timezone = str(
            display_refresh_schedule.get("timezone", self.display_default_timezone)
        )
        try:
            ZoneInfo(self.display_refresh_schedule_timezone)
        except ZoneInfoNotFoundError as exc:
            raise ValueError(
                "display.refresh_schedule.timezone 不是有效的 IANA 时区"
            ) from exc
        self.display_refresh_daily_times = _parse_daily_times(
            display_refresh_schedule.get("daily_times")
        )

        self.runtime_log_dir = _path_from_root(
            self.project_root, str(storage["runtime_log_dir"])
        )
        self.device_status_dir = _path_from_root(
            self.project_root, str(storage["device_status_dir"])
        )
        self.log_keep_sessions = int(storage["log_keep_sessions"])
        self.ota_manifest_dir = _path_from_root(
            self.project_root, str(storage["ota_manifest_dir"])
        )
        self.ota_artifact_dir = _path_from_root(
            self.project_root, str(storage["ota_artifact_dir"])
        )

        memory = data.get("memory") or {}
        self.memory_enabled = bool(memory.get("enabled", False))
        self.memory_vector_store_path = _path_from_root(
            self.project_root,
            str(memory.get("vector_store_path", "data/mem0_chroma")),
        )
        self.memory_collection_name = str(memory.get("collection_name", "photopainter"))
        self.memory_llm_model = str(memory.get("llm_model", "glm-4-plus"))
        self.memory_embedder_model = str(memory.get("embedder_model", "embedding-3"))
        self.memory_embedder_dims = int(memory.get("embedder_dims", 1024))
        self.memory_search_top_k = int(memory.get("search_top_k", 5))
        self.memory_search_threshold = float(memory.get("search_threshold", 0.3))


@lru_cache
def get_server_settings() -> ServerSettings:
    """返回进程内缓存的服务配置。"""
    return ServerSettings()
