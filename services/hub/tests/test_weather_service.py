"""和风天气 API 字段映射和天气显示上下文测试。"""

from datetime import UTC, datetime, timedelta
from types import SimpleNamespace

from app.workflows.display.context import DisplayContextService
from app.services.weather_service import WeatherService
from app.schemas.moon import MoonPayload, MoonPhasePoint
from app.schemas.weather import WeatherLocation


def _service_settings() -> SimpleNamespace:
    return SimpleNamespace(
        qweather_daily_days="7d",
        qweather_daily_path="/v7/weather/{days}",
        qweather_minutely_path="/v7/minutely/5m",
        qweather_alert_path="/weatheralert/v1/current/{latitude}/{longitude}",
        qweather_air_path="/airquality/v1/current/{latitude}/{longitude}",
        qweather_moon_path="/v7/astronomy/moon",
        weather_location_cache_seconds=604800,
    )


def test_daily_forecast_maps_seven_days_from_qweather():
    service = WeatherService(_service_settings())
    observed: dict = {}

    def get_json(path, params, optional=False):
        observed.update(path=path, params=params, optional=optional)
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
        "optional": True,
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

    def get_json(path, params, optional=False):
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

    def get_json(path, params, optional=False):
        observed.update(path=path, params=params, optional=optional)
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
        "optional": False,
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
