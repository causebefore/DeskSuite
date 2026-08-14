"""和风天气 API 字段映射和天气显示上下文测试。"""

import json
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace

import pytest

from app.workflows.display.context import DisplayContextService
from app.services.weather_service import WeatherService
from app.schemas.moon import MoonPayload, MoonPhasePoint
from app.schemas.weather import (
    DailyForecast,
    DailyForecastItem,
    MinutelyRain,
    WeatherLocation,
    WeatherNow,
)


def _service_settings() -> SimpleNamespace:
    return SimpleNamespace(
        qweather_daily_days="7d",
        qweather_daily_path="/v7/weather/{days}",
        qweather_minutely_path="/v7/minutely/5m",
        qweather_alert_path="/weatheralert/v1/current/{latitude}/{longitude}",
        qweather_air_path="/airquality/v1/current/{latitude}/{longitude}",
        qweather_moon_path="/v7/astronomy/moon",
        qweather_host="test.qweather.example",
        qweather_api_key="test-key",
        qweather_timeout_seconds=6,
        weather_location_cache_seconds=604800,
        weather_now_cache_seconds=900,
        weather_daily_cache_seconds=10800,
        weather_minutely_cache_seconds=900,
        weather_alert_cache_seconds=900,
        weather_air_cache_seconds=900,
    )


class _JsonResponse:
    def __init__(self, payload: dict) -> None:
        self._body = json.dumps(payload).encode("utf-8")
        self.headers: dict[str, str] = {}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        return None

    def read(self) -> bytes:
        return self._body


def test_daily_forecast_maps_seven_days_from_qweather():
    service = WeatherService(_service_settings())
    observed: dict = {}

    def get_json(path, params):
        observed.update(path=path, params=params)
        return {
            "daily": [
                {
                    "fxDate": f"2026-07-{18 + index:02d}",
                    "textDay": "多云",
                    "textNight": "晴",
                    "iconDay": "101",
                    "iconNight": "150",
                    "tempMin": str(20 + index),
                    "tempMax": str(29 + index),
                    "windDirDay": "东南风",
                    "windScaleDay": "3",
                    "humidity": "68",
                    "precip": "1.2",
                    "uvIndex": "5",
                    "sunrise": "05:03",
                    "sunset": "19:05",
                }
                for index in range(8)
            ]
        }

    service._get_json = get_json  # type: ignore[method-assign]
    forecast = service._fetch_daily("101190401")

    assert observed == {
        "path": "/v7/weather/7d",
        "params": {"location": "101190401", "lang": "zh", "unit": "m"},
    }
    assert forecast.days == "7d"
    assert len(forecast.items) == 7
    assert forecast.items[0].temp_min_c == 20
    assert forecast.items[0].temp_max_c == 29
    assert forecast.items[0].uv_index == 5
    assert forecast.items[0].sunrise == "05:03"


def test_optional_qweather_modules_map_minutely_alert_and_air():
    service = WeatherService(_service_settings())
    location = WeatherLocation(
        city="苏州",
        location_id="101190401",
        latitude=31.30,
        longitude=120.58,
    )

    def get_json(path, params):
        if path == "/v7/minutely/5m":
            return {
                "summary": "40分钟后有小雨",
                "minutely": [
                    {"fxTime": "2026-07-18T18:00+08:00", "precip": "0.3", "type": "rain"}
                ],
            }
        if path.startswith("/weatheralert/"):
            return {
                "alerts": [
                    {
                        "headline": "暴雨黄色预警",
                        "eventType": {"name": "暴雨"},
                        "severity": "minor",
                        "description": "预计有强降水",
                        "effectiveTime": "2026-07-18T18:00+08:00",
                        "expireTime": "2026-07-18T22:00+08:00",
                    }
                ]
            }
        return {
            "indexes": [
                {
                    "code": "cn-mep",
                    "aqi": 42,
                    "category": "优",
                    "primaryPollutant": {"code": "pm2p5"},
                }
            ]
        }

    service._get_json = get_json  # type: ignore[method-assign]

    minutely = service._fetch_minutely(location)
    alerts = service._fetch_alerts(location)
    air = service._fetch_air(location)

    assert minutely.summary == "40分钟后有小雨"
    assert minutely.items[0].precip_mm == 0.3
    assert alerts[0].title == "暴雨黄色预警"
    assert alerts[0].alert_type == "暴雨"
    assert alerts[0].severity == "minor"
    assert air is not None
    assert air.aqi == 42
    assert air.category == "优"
    assert air.primary == "pm2p5"


def test_moon_phase_maps_qweather_hourly_data():
    service = WeatherService(_service_settings())
    service._lookup_city = lambda city: WeatherLocation(  # type: ignore[method-assign]
        city="苏州",
        location_id="101190401",
        adm1="江苏省",
        adm2="苏州市",
    )
    observed: dict = {}

    def get_json(path, params):
        observed.update(path=path, params=params)
        return {
            "updateTime": "2026-07-20T05:00+08:00",
            "moonrise": "2026-07-20T19:12+08:00",
            "moonset": "2026-07-21T05:24+08:00",
            "moonPhase": [
                {
                    "fxTime": f"2026-07-20T{hour:02d}:00+08:00",
                    "value": "0.54",
                    "name": "亏凸月",
                    "illumination": str(98 - hour // 12),
                    "icon": "805",
                }
                for hour in range(24)
            ],
        }

    service._get_json = get_json  # type: ignore[method-assign]
    payload = service._fetch_moon("苏州", "20260720")

    assert observed == {
        "path": "/v7/astronomy/moon",
        "params": {"location": "101190401", "date": "20260720", "lang": "zh"},
    }
    assert payload.fx_date == "2026-07-20"
    assert payload.moonrise == "2026-07-20T19:12+08:00"
    assert len(payload.phases) == 24
    assert payload.phases[0].value == 0.54
    assert payload.phases[0].illumination_percent == 98
    assert payload.phases[0].icon == "805"


def test_moon_phase_uses_stale_cache_when_refresh_fails():
    settings = SimpleNamespace(
        weather_provider="qweather",
        qweather_api_key="test-key",
        weather_moon_cache_seconds=1,
    )
    service = WeatherService(settings)
    cached = MoonPayload(
        source="qweather",
        location=WeatherLocation(city="苏州", location_id="101190401"),
        fx_date="2026-07-20",
        phases=[
            MoonPhasePoint(
                fx_time="2026-07-20T12:00+08:00",
                value=0.54,
                name="亏凸月",
                illumination_percent=98,
                icon="805",
            )
        ],
    )
    service._moon_cache["苏州:20260720"] = (
        datetime.now(UTC) - timedelta(hours=1),
        cached,
    )
    service._fetch_moon = (  # type: ignore[method-assign]
        lambda city, forecast_date: (_ for _ in ()).throw(TimeoutError("timeout"))
    )

    payload = service.get_moon_phase("苏州", "20260720")

    assert payload.stale is True
    assert payload.source == "qweather"
    assert payload.phases[0].name == "亏凸月"
    assert "使用旧缓存" in payload.error


def test_expired_weather_cache_is_kept_when_refresh_fails():
    service = WeatherService(_service_settings())
    cached = DailyForecast(
        days="7d",
        items=[
            DailyForecastItem(
                fx_date="2026-08-14",
                text_day="晴",
                temp_min_c=24,
                temp_max_c=33,
            )
        ],
    )
    service._daily_cache["101190401:7d"] = (
        datetime.now(UTC) - timedelta(hours=4),
        cached,
    )

    result = service._get_cached(
        service._daily_cache,
        "101190401:7d",
        10800,
        lambda: (_ for _ in ()).throw(TimeoutError("timeout")),
        use_stale_on_error=True,
        fallback_on_error=lambda: DailyForecast(days="7d"),
        source_name="天气预报",
    )

    assert result is cached
    assert result.items[0].text_day == "晴"


def test_weather_aggregation_uses_stale_daily_and_does_not_cache_failure_fallback():
    service = WeatherService(_service_settings())
    location = WeatherLocation(
        city="苏州",
        location_id="101190401",
        latitude=31.30,
        longitude=120.58,
    )
    cached_daily = DailyForecast(
        days="7d",
        items=[DailyForecastItem(fx_date="2026-08-14", text_day="晴")],
    )
    fresh_at = datetime.now(UTC)
    location_key = location.location_id
    daily_key = f"{location_key}:7d"
    service._location_cache["苏州"] = (fresh_at, location)
    service._now_cache[location_key] = (fresh_at, WeatherNow(text="晴"))
    service._daily_cache[daily_key] = (
        fresh_at - timedelta(hours=4),
        cached_daily,
    )
    service._minutely_cache[location_key] = (fresh_at, MinutelyRain())
    service._alert_cache[location_key] = (fresh_at, [])
    service._air_cache[location_key] = (fresh_at, None)
    service._fetch_daily = (  # type: ignore[method-assign]
        lambda location_id: (_ for _ in ()).throw(TimeoutError("timeout"))
    )

    stale_payload = service._fetch_qweather("苏州")

    assert stale_payload.daily is cached_daily

    service._daily_cache.clear()
    fallback_payload = service._fetch_qweather("苏州")

    assert fallback_payload.daily.items == []
    assert daily_key not in service._daily_cache


def test_failure_fallback_is_not_cached_but_successful_empty_result_is_cached():
    service = WeatherService(_service_settings())
    cache: dict[str, tuple[datetime, DailyForecast]] = {}

    fallback = service._get_cached(
        cache,
        "101190401:7d",
        10800,
        lambda: (_ for _ in ()).throw(TimeoutError("timeout")),
        fallback_on_error=lambda: DailyForecast(days="7d"),
        source_name="天气预报",
    )

    assert fallback.items == []
    assert cache == {}

    legitimate_empty = service._get_cached(
        cache,
        "101190401:7d",
        10800,
        lambda: DailyForecast(days="7d"),
        fallback_on_error=lambda: DailyForecast(days="7d"),
        source_name="天气预报",
    )

    assert legitimate_empty.items == []
    assert cache["101190401:7d"][1] is legitimate_empty


def test_qweather_request_retries_once_within_original_timeout_budget(monkeypatch):
    service = WeatherService(_service_settings())
    timeouts: list[float] = []

    def flaky_urlopen(request, timeout):
        timeouts.append(timeout)
        if len(timeouts) == 1:
            raise TimeoutError("first timeout")
        return _JsonResponse({"code": "200", "now": {"text": "晴"}})

    monkeypatch.setattr("app.services.weather_service.urlopen", flaky_urlopen)

    data = service._get_json("/v7/weather/now", {"location": "101190401"})

    assert data["now"]["text"] == "晴"
    assert timeouts == [3.0, 3.0]


def test_qweather_request_raises_after_exactly_two_failed_attempts(monkeypatch):
    service = WeatherService(_service_settings())
    attempts = 0

    def failing_urlopen(request, timeout):
        nonlocal attempts
        attempts += 1
        raise TimeoutError("timeout")

    monkeypatch.setattr("app.services.weather_service.urlopen", failing_urlopen)

    with pytest.raises(TimeoutError, match="timeout"):
        service._get_json("/v7/weather/now", {"location": "101190401"})

    assert attempts == 2


def test_qweather_204_is_a_legitimate_empty_response_without_retry(monkeypatch):
    service = WeatherService(_service_settings())
    attempts = 0

    def empty_urlopen(request, timeout):
        nonlocal attempts
        attempts += 1
        return _JsonResponse({"code": "204"})

    monkeypatch.setattr("app.services.weather_service.urlopen", empty_urlopen)

    assert service._get_json("/v7/weather/7d", {"location": "101190401"}) == {
        "code": "204"
    }
    assert attempts == 1


def test_weather_context_exposes_seven_day_chart_and_detail_data():
    weather_service = WeatherService(_service_settings())
    weather = weather_service._mock_weather("苏州")
    context_service = DisplayContextService(
        SimpleNamespace(
            display_default_timezone="Asia/Shanghai",
            display_default_city="苏州",
        ),
        weather_service=SimpleNamespace(get_current_weather=lambda city: weather),
        calendar_service=SimpleNamespace(),
        mail_service=SimpleNamespace(),
        quota_service=SimpleNamespace(),
        device_status_service=SimpleNamespace(),
        rss_service=SimpleNamespace(),
    )

    context = context_service.build("weather-screen", required_sources={"weather"})
    weather_context = context["weather"]

    assert context["availability"] == {"weather": True}
    assert len(weather_context["daily"]) == 7
    assert len(weather_context["minutely"]["points"]) == 24
    assert weather_context["wind_dir"] == "东南风"
    assert weather_context["aqi"] == 58
    assert weather_context["daily"][0]["uv_index"] == 7
    assert weather_context["daily"][0]["sunrise"] == "06:00"


def test_moon_context_exposes_current_phase_and_timeline():
    moon = MoonPayload(
        source="qweather",
        updated_at=datetime.fromisoformat("2026-07-20T05:00+08:00"),
        location=WeatherLocation(
            city="昆山",
            location_id="101190404",
            adm1="江苏省",
            adm2="苏州市",
        ),
        fx_date="2026-07-20",
        moonrise="2026-07-20T19:12+08:00",
        moonset="2026-07-21T05:24+08:00",
        phases=[
            MoonPhasePoint(
                fx_time="2026-07-20T12:00+08:00",
                value=0.54,
                name="亏凸月",
                illumination_percent=98,
                icon="805",
            )
        ],
    )
    context_service = DisplayContextService(
        SimpleNamespace(
            display_default_timezone="Asia/Shanghai",
            display_default_city="昆山",
        ),
        weather_service=SimpleNamespace(get_moon_phase=lambda city, date: moon),
        calendar_service=SimpleNamespace(),
        mail_service=SimpleNamespace(),
        quota_service=SimpleNamespace(),
        device_status_service=SimpleNamespace(),
        rss_service=SimpleNamespace(),
    )

    context = context_service.build("moon-screen", required_sources={"moon"})

    assert context["availability"] == {"moon": True}
    assert context["moon"]["city"] == "昆山"
    assert context["moon"]["moonrise"] == "19:12"
    assert context["moon"]["moonset"] == "05:24"
    assert context["moon"]["current"] == {
        "time": "12:00",
        "value": 0.54,
        "name": "亏凸月",
        "illumination": 98,
        "icon": "805",
    }
    assert context["moon"]["timeline"] == [context["moon"]["current"]]
