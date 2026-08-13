"""精简后的服务配置加载测试。"""

from pathlib import Path

import pytest

from app.core.config import ServerSettings


CONFIG = """
[app]
title = "PhotoPainter Test"
version = "1.0.0"
description = "test"
[server]
host = "127.0.0.1"
port = 4321
log_level = "debug"
[providers]
weather = "mock"
calendar = "mock"
mail = "mock"
llm = "zhipu"
speech = "zhipu"
embedding = "zhipu"
[dashboard]
source_timeout_seconds = 20
[zhipu]
asr_model = "asr"
llm_model = "llm"
tts_model = "tts"
tts_voice = "voice"
quota_cache_seconds = 60
[voice.debug_audio]
enabled = false
[assistant]
principal_id = "owner"
checkpoint_path = "data/assistant/test-checkpoints.sqlite3"
max_input_chars = 4000
model_timeout_seconds = 30
recursion_limit = 12
[assistant.memory]
enabled = true
store_path = "data/assistant/test-memories.sqlite3"
embedder_model = "embedding-test"
embedder_dims = 3
search_limit = 4
search_threshold = 0.25
[mcp.web_search_prime]
enabled = false
transport = "streamable_http"
url = "https://open.bigmodel.cn/api/mcp/web_search_prime/mcp"
timeout_seconds = 20
[qweather]
host = "api.example.test"
timeout_seconds = 1
city_lookup_path = "/city"
now_path = "/now"
daily_path = "/daily/{days}"
daily_days = "3d"
minutely_path = "/rain"
alert_path = "/alert/{latitude}/{longitude}"
air_path = "/air/{latitude}/{longitude}"
moon_path = "/moon"
[weather.cache]
location_seconds = 1
now_seconds = 2
daily_seconds = 3
minutely_seconds = 4
alert_seconds = 5
air_seconds = 6
moon_seconds = 7
[caldav]
url = "https://example.test/"
timeout_seconds = 2
range_days = 7
max_events = 8
month_max_events = 80
[caldav.cache]
events_seconds = 9
[imap]
host = "imap.example.test"
port = 993
use_ssl = true
timeout_seconds = 3
max_messages = 4
[imap.cache]
inbox_seconds = 10
[rss]
enabled = true
feeds = ["https://example.test/feed.xml"]
timeout_seconds = 4
cache_seconds = 600
max_items_per_feed = 9
max_items = 18
[display]
template_dir = "web/pages"
output_dir = "frames"
shared_css = "web/shared/epaper.css"
pages = ["demo", "calendar"]
default_page = "calendar"
dither = true
render_timeout_ms = 5000
device_status_min_refresh_seconds = 1800
[display.refresh_schedule]
timezone = "Asia/Shanghai"
daily_times = ["11:50", "11:15"]
[display.defaults]
city = "上海"
timezone = "Asia/Shanghai"
device_id = "screen-1"
[storage]
runtime_log_dir = "logs"
device_status_dir = "data/device_status"
log_keep_sessions = 7
ota_manifest_dir = "bins/manifests"
ota_artifact_dir = "bins/artifacts"
"""


def test_settings_load_display_and_internal_provider_config(tmp_path: Path):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(CONFIG, encoding="utf-8")
    env_path.write_text(
        "ZHIPU_API_KEY=test-key\nDEVICE_API_TOKEN=device-secret\n",
        encoding="utf-8",
    )

    settings = ServerSettings(config_path, env_path, tmp_path)

    assert settings.server_port == 4321
    assert settings.zhipu_api_key == "test-key"
    assert settings.device_api_token == "device-secret"
    assert settings.voice_debug_audio_enabled is False
    assert settings.llm_provider == "zhipu"
    assert settings.speech_provider == "zhipu"
    assert settings.embedding_provider == "zhipu"
    assert settings.assistant_principal_id == "owner"
    assert settings.assistant_checkpoint_path == (
        tmp_path / "data" / "assistant" / "test-checkpoints.sqlite3"
    )
    assert settings.assistant_max_input_chars == 4000
    assert settings.assistant_model_timeout_seconds == 30
    assert settings.assistant_recursion_limit == 12
    assert settings.assistant_memory_enabled is True
    assert settings.assistant_memory_store_path == (
        tmp_path / "data" / "assistant" / "test-memories.sqlite3"
    )
    assert settings.assistant_memory_embedder_model == "embedding-test"
    assert settings.assistant_memory_embedder_dims == 3
    assert settings.assistant_memory_search_limit == 4
    assert settings.assistant_memory_search_threshold == 0.25
    assert settings.web_search_mcp_enabled is False
    assert settings.web_search_mcp_transport == "streamable_http"
    assert settings.web_search_mcp_url.endswith("/web_search_prime/mcp")
    assert settings.web_search_mcp_timeout_seconds == 20
    assert settings.dashboard_source_timeout_seconds == 20
    assert settings.display_default_city == "上海"
    assert settings.display_default_device_id == "screen-1"
    assert settings.display_template_dir == tmp_path / "web" / "pages"
    assert settings.display_output_dir == tmp_path / "frames"
    assert settings.display_shared_css == tmp_path / "web" / "shared" / "epaper.css"
    assert settings.display_font_file == (
        tmp_path / "web" / "vendor" / "fonts" / "AlibabaPuHuiTi-3-55-Regular.ttf"
    )
    assert settings.display_pages == ("demo", "calendar")
    assert settings.display_default_page == "calendar"
    assert settings.display_dither is True
    assert settings.display_device_status_min_refresh_seconds == 1800
    assert settings.display_refresh_schedule_timezone == "Asia/Shanghai"
    assert [item.strftime("%H:%M") for item in settings.display_refresh_daily_times] == [
        "11:15",
        "11:50",
    ]
    assert settings.caldav_month_max_events == 80
    assert settings.rss_enabled is True
    assert settings.rss_feed_urls == ("https://example.test/feed.xml",)
    assert settings.rss_timeout_seconds == 4
    assert settings.rss_cache_seconds == 600
    assert settings.rss_max_items_per_feed == 9
    assert settings.rss_max_items == 18
    assert settings.qweather_moon_path == "/moon"
    assert settings.weather_moon_cache_seconds == 7
    assert settings.runtime_log_dir == tmp_path / "logs"
    assert settings.device_status_dir == tmp_path / "data" / "device_status"
    assert settings.ota_manifest_dir == tmp_path / "bins" / "manifests"
    assert settings.ota_artifact_dir == tmp_path / "bins" / "artifacts"


def test_settings_loads_optional_build_id_from_process_environment(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(CONFIG, encoding="utf-8")
    env_path.write_text("", encoding="utf-8")
    monkeypatch.setenv("DESKSUITE_BUILD_ID", "git-test-sha")

    settings = ServerSettings(config_path, env_path, tmp_path)

    assert settings.build_id == "git-test-sha"


def test_debug_audio_requires_device_token_when_enabled(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace("enabled = false", "enabled = true", 1),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")
    monkeypatch.delenv("DEVICE_API_TOKEN", raising=False)

    with pytest.raises(ValueError, match="必须配置 DEVICE_API_TOKEN"):
        ServerSettings(config_path, env_path, tmp_path)


def test_debug_audio_can_be_enabled_with_device_token(tmp_path: Path):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace("enabled = false", "enabled = true", 1),
        encoding="utf-8",
    )
    env_path.write_text("DEVICE_API_TOKEN=test-token\n", encoding="utf-8")

    settings = ServerSettings(config_path, env_path, tmp_path)

    assert settings.voice_debug_audio_enabled is True


def test_project_text_pages_disable_dither(tmp_path: Path):
    """项目默认页面都是信息型页面，不应启用照片抖动。"""
    project_root = Path(__file__).resolve().parents[1]

    settings = ServerSettings(
        project_root / "config.toml",
        tmp_path / ".env",
        project_root,
    )

    assert settings.display_dither is False


def test_settings_accepts_16_display_pages_and_rejects_17(tmp_path: Path):
    """配置层与设备协议保持一致，最多允许十六个页面。"""
    env_path = tmp_path / ".env"
    env_path.write_text("", encoding="utf-8")

    pages_16 = [f"page{i}" for i in range(16)]
    config_16 = CONFIG.replace(
        'pages = ["demo", "calendar"]',
        f"pages = {pages_16!r}".replace("'", '"'),
    ).replace('default_page = "calendar"', 'default_page = "page0"')
    config_path = tmp_path / "config.toml"
    config_path.write_text(config_16, encoding="utf-8")
    assert len(ServerSettings(config_path, env_path, tmp_path).display_pages) == 16

    pages_17 = [*pages_16, "page16"]
    config_17 = config_16.replace(
        f"pages = {pages_16!r}".replace("'", '"'),
        f"pages = {pages_17!r}".replace("'", '"'),
    )
    config_path.write_text(config_17, encoding="utf-8")
    with pytest.raises(ValueError, match="1 到 16"):
        ServerSettings(config_path, env_path, tmp_path)


def test_settings_rejects_more_than_eight_rss_feeds(tmp_path: Path):
    env_path = tmp_path / ".env"
    env_path.write_text("", encoding="utf-8")
    feeds = [f"https://example.test/{index}.xml" for index in range(9)]
    config = CONFIG.replace(
        'feeds = ["https://example.test/feed.xml"]',
        f"feeds = {feeds!r}".replace("'", '"'),
    )
    config_path = tmp_path / "config.toml"
    config_path.write_text(config, encoding="utf-8")

    with pytest.raises(ValueError, match="最多包含 8 个"):
        ServerSettings(config_path, env_path, tmp_path)


@pytest.mark.parametrize("timeout", [0, 21])
def test_settings_rejects_dashboard_timeout_outside_http_budget(
    tmp_path: Path,
    timeout: int,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace("source_timeout_seconds = 20", f"source_timeout_seconds = {timeout}"),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match="dashboard.source_timeout_seconds"):
        ServerSettings(config_path, env_path, tmp_path)


def test_settings_rejects_excessive_month_calendar_events(tmp_path: Path):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(CONFIG.replace("month_max_events = 80", "month_max_events = 501"), encoding="utf-8")
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match="month_max_events"):
        ServerSettings(config_path, env_path, tmp_path)


@pytest.mark.parametrize(
    ("replacement", "message"),
    [
        ('daily_times = []', "1 到 48"),
        ('daily_times = ["11:15", "11:15"]', "不能包含重复时间"),
        ('daily_times = ["11:60"]', "HH:MM"),
    ],
)
def test_settings_rejects_invalid_daily_refresh_times(
    tmp_path: Path,
    replacement: str,
    message: str,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace('daily_times = ["11:50", "11:15"]', replacement),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match=message):
        ServerSettings(config_path, env_path, tmp_path)


def test_settings_rejects_invalid_refresh_schedule_timezone(tmp_path: Path):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace('timezone = "Asia/Shanghai"\ndaily_times', 'timezone = "Mars/Base"\ndaily_times'),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match="IANA 时区"):
        ServerSettings(config_path, env_path, tmp_path)


def test_settings_without_daily_schedule_uses_legacy_interval(tmp_path: Path):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace(
            '[display.refresh_schedule]\ntimezone = "Asia/Shanghai"\ndaily_times = ["11:50", "11:15"]\n',
            "",
        ),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    settings = ServerSettings(config_path, env_path, tmp_path)

    assert settings.display_refresh_daily_times == ()
    assert settings.display_refresh_interval_seconds == 3600


@pytest.mark.parametrize(
    ("old", "new", "message"),
    [
        (
            'transport = "streamable_http"',
            'transport = "sse"',
            "streamable_http",
        ),
        (
            'url = "https://open.bigmodel.cn/api/mcp/web_search_prime/mcp"',
            'url = "http://example.test/mcp"',
            "HTTPS URL",
        ),
        (
            'url = "https://open.bigmodel.cn/api/mcp/web_search_prime/mcp"\n'
            "timeout_seconds = 20",
            'url = "https://open.bigmodel.cn/api/mcp/web_search_prime/mcp"\n'
            "timeout_seconds = 61",
            "5 到 60",
        ),
    ],
)
def test_settings_rejects_unsafe_web_search_mcp_config(
    tmp_path: Path,
    old: str,
    new: str,
    message: str,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(CONFIG.replace(old, new, 1), encoding="utf-8")
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match=message):
        ServerSettings(config_path, env_path, tmp_path)


@pytest.mark.parametrize(
    ("field", "message"),
    [
        ("llm", "LLM provider"),
        ("speech", "speech provider"),
        ("embedding", "embedding provider"),
    ],
)
def test_settings_rejects_unknown_ai_provider(
    tmp_path: Path,
    field: str,
    message: str,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace(f'{field} = "zhipu"', f'{field} = "unknown"'),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match=message):
        ServerSettings(config_path, env_path, tmp_path)


def test_settings_rejects_assistant_input_limit_above_schema_cap(tmp_path: Path):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace("max_input_chars = 4000", "max_input_chars = 4001"),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match="1 到 4000"):
        ServerSettings(config_path, env_path, tmp_path)


@pytest.mark.parametrize(
    ("old", "new", "message"),
    [
        ("embedder_dims = 3", "embedder_dims = 0", "1 到 4096"),
        ("search_limit = 4", "search_limit = 21", "1 到 20"),
        ("search_threshold = 0.25", "search_threshold = 1.1", "-1 到 1"),
        ('embedder_model = "embedding-test"', 'embedder_model = ""', "不能为空"),
    ],
)
def test_settings_rejects_invalid_assistant_memory_config(
    tmp_path: Path,
    old: str,
    new: str,
    message: str,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(CONFIG.replace(old, new), encoding="utf-8")
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match=message):
        ServerSettings(config_path, env_path, tmp_path)


def test_settings_keeps_checkpoint_and_long_term_store_in_separate_files(
    tmp_path: Path,
):
    config_path = tmp_path / "config.toml"
    env_path = tmp_path / ".env"
    config_path.write_text(
        CONFIG.replace(
            'store_path = "data/assistant/test-memories.sqlite3"',
            'store_path = "data/assistant/test-checkpoints.sqlite3"',
        ),
        encoding="utf-8",
    )
    env_path.write_text("", encoding="utf-8")

    with pytest.raises(ValueError, match="不能与 assistant.checkpoint_path 相同"):
        ServerSettings(config_path, env_path, tmp_path)
