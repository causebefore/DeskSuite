"""
天气服务 — 封装和风天气 API 调用，提供城市搜索、实时天气、七日预报、
分钟降水、预警、空气质量与月相查询。

核心设计：
- 多级缓存：按数据新鲜度需求分层缓存
  - 城市位置：7 天（极稳定，几乎不变）
  - 实时天气：15 分钟（和风免费订阅更新频率 ≈ 10-20 分钟）
  - 七日预报：3 小时（预报通常一天更新 3-8 次）
  - 分钟降水：15 分钟
  - 天气预警：15 分钟
  - 月相：6 小时；回源失败时优先保留上一份真实数据
- 容错降级：API 调用失败 → 返回 mock 数据，不影响设备 UI 展示
- 自动切换：根据 weather_provider 配置自动切换真实 API / mock 模式
- 可选 API：分钟降水、预警 API 请求失败时静默降级（optional=True）

API 调用流程：
1. get_current_weather(city)
2.   → _fetch_qweather(city)
3.     → _lookup_city(city)           # 城市搜索 → WeatherLocation（7 天缓存）
4.     → _fetch_now(location_id)       # 实时天气（15 分钟缓存）
5.     → _fetch_daily(location_id)     # 七日预报（3 小时缓存）
6.     → _fetch_minutely(location)     # 分钟降水（15 分钟缓存，需经纬度）
7.     → _fetch_alerts(location)       # 天气预警（15 分钟缓存，需经纬度）
8.   → 聚合为 WeatherPayload 返回
"""

from datetime import UTC, datetime, timedelta
from collections.abc import Callable
from typing import Any, TypeVar
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import gzip
import json

from loguru import logger

from app.core.config import ServerSettings
from app.schemas.moon import MoonPayload, MoonPhasePoint
from app.schemas.weather import (
    DailyForecast,
    DailyForecastItem,
    MinutelyRain,
    MinutelyRainItem,
    WeatherAir,
    WeatherAlert,
    WeatherLocation,
    WeatherNow,
    WeatherPayload,
)

# 泛型变量：用于 _get_cached 的类型标注
T = TypeVar("T")


class WeatherService:
    """
    和风天气 API 封装服务。

    对外暴露 get_current_weather(city) 与 get_moon_phase(city, date)，
    内部自动完成城市搜索、分层缓存查询与模型映射。
    当 weather_provider 不是 "qweather" 或 API Key 为空时，
    自动退回 mock 模式。

    缓存实现：
    每个缓存类型维护独立的 dict[key, (datetime, value)]，
    _get_cached 方法统一管理 TTL 过期逻辑。
    """

    def __init__(self, settings: ServerSettings) -> None:
        """
        初始化天气服务。

        Args:
            settings: 服务器配置（含和风 API Key、缓存 TTL 等）
        """
        self._settings = settings
        # ── 各类型独立缓存 ─────────────────────────────
        # 格式：{key: (缓存时间戳, 缓存值)}
        self._location_cache: dict[str, tuple[datetime, WeatherLocation]] = {}
        self._now_cache: dict[str, tuple[datetime, WeatherNow]] = {}
        self._daily_cache: dict[str, tuple[datetime, DailyForecast]] = {}
        self._minutely_cache: dict[str, tuple[datetime, MinutelyRain]] = {}
        self._alert_cache: dict[str, tuple[datetime, list[WeatherAlert]]] = {}
        self._air_cache: dict[str, tuple[datetime, WeatherAir | None]] = {}
        self._moon_cache: dict[str, tuple[datetime, MoonPayload]] = {}

    # ── 公开 API ──────────────────────────────────────

    def get_current_weather(self, city: str) -> WeatherPayload:
        """
        获取指定城市的天气聚合数据（唯一对外接口）。

        根据 weather_provider 配置决定使用真实 API 还是 mock：
        - "qweather" + 有效 API Key → 调用和风天气 API
        - 其他情况 → 返回 mock 数据

        异常处理：
        - 和风 API 调用中任何异常都被捕获，转为返回含 error 字段的 mock 数据
        - 避免单个 API 失败导致整个请求 500

        Args:
            city: 城市中文名称（如 '苏州'、'北京'）

        Returns:
            WeatherPayload: 完整的天气聚合数据
        """
        # 未配置真实 API → mock 模式
        if (
            self._settings.weather_provider != "qweather"
            or not self._settings.qweather_api_key
        ):
            logger.info("天气 mock 模式：未配置和风天气 API，city={} provider={}", city, self._settings.weather_provider)
            return self._mock_weather(city, "未配置和风天气 API，返回 mock 数据")

        try:
            logger.info("开始拉取和风天气：city={}", city)
            payload = self._fetch_qweather(city)
            logger.info("和风天气拉取成功：city={} now={}", city, payload.now.text if payload.now else "")
            return payload
        except Exception as exc:
            # 降级：任何 API 错误都返回 mock 数据 + 错误描述
            logger.warning("和风天气请求失败，降级 mock：city={} exc={}", city, exc)
            return self._mock_weather(city, f"和风天气请求失败: {exc}")

    def get_moon_phase(self, city: str, forecast_date: str) -> MoonPayload:
        """获取指定城市和日期的月升、月落及逐小时月相。"""
        try:
            parsed_date = datetime.strptime(forecast_date, "%Y%m%d")
        except ValueError as exc:
            raise ValueError("月相日期必须使用 yyyyMMdd 格式") from exc

        date_key = parsed_date.strftime("%Y%m%d")
        cache_key = f"{city.strip().lower()}:{date_key}"
        if (
            self._settings.weather_provider != "qweather"
            or not self._settings.qweather_api_key
        ):
            logger.info(
                "月相 mock 模式：未配置和风天气 API，city={} date={}",
                city,
                date_key,
            )
            return self._mock_moon(
                city,
                date_key,
                "未配置和风天气 API，返回 mock 数据",
            )

        stale_entry = self._moon_cache.get(cache_key)
        try:
            payload = self._get_cached(
                self._moon_cache,
                cache_key,
                self._settings.weather_moon_cache_seconds,
                lambda: self._fetch_moon(city, date_key),
            )
            logger.info(
                "和风月相拉取成功：city={} date={} points={}",
                city,
                date_key,
                len(payload.phases),
            )
            return payload
        except Exception as exc:
            if stale_entry is not None:
                logger.warning(
                    "和风月相请求失败，回退旧缓存：city={} date={} exc={}",
                    city,
                    date_key,
                    exc,
                )
                return stale_entry[1].model_copy(
                    update={
                        "stale": True,
                        "error": f"和风月相请求失败，使用旧缓存: {exc}",
                    }
                )
            logger.warning(
                "和风月相请求失败，降级 mock：city={} date={} exc={}",
                city,
                date_key,
                exc,
            )
            return self._mock_moon(
                city,
                date_key,
                f"和风月相请求失败: {exc}",
            )

    # ── 通用缓存方法 ──────────────────────────────────

    def _get_cached(
        self,
        cache: dict[str, tuple[datetime, T]],
        key: str,
        ttl_seconds: int,
        fetcher: Callable[[], T],
    ) -> T:
        """
        通用缓存查询 / 填充方法。

        逻辑：
        1. 查缓存 → 命中且未过期 → 直接返回
        2. 未命中或已过期 → 调用 fetcher() → 存入缓存 → 返回

        Args:
            cache: 缓存 dict
            key: 缓存键
            ttl_seconds: 缓存有效期（秒）
            fetcher: 未命中时的数据获取函数

        Returns:
            缓存或新获取的数据
        """
        cached = cache.get(key)
        now = datetime.now(UTC)
        # 命中且未过期 → 直接返回
        if cached and now - cached[0] < timedelta(seconds=ttl_seconds):
            return cached[1]

        # 未命中或过期 → 调用 fetcher 获取新数据并缓存
        logger.debug("缓存未命中，回源拉取 key={}", key)
        value = fetcher()
        cache[key] = (now, value)
        return value

    # ── 和风天气聚合流程 ──────────────────────────────

    def _fetch_qweather(self, city: str) -> WeatherPayload:
        """
        调用和风天气 API 聚合所有天气数据。

        流程（每个步骤独立缓存，互不阻塞）：
        1. 城市搜索 → location_id
        2. 用 location_id 查实时天气
        3. 用 location_id 查七日预报
        4. 用经纬度查分钟降水
        5. 用经纬度查天气预警

        Args:
            city: 城市中文名称

        Returns:
            WeatherPayload: 聚合后的天气数据
        """
        city_key = city.strip().lower()

        # 步骤 1：城市搜索（缓存 TTL 最长，7 天）
        location = self._get_cached(
            self._location_cache,
            city_key,
            self._settings.weather_location_cache_seconds,
            lambda: self._lookup_city(city),
        )

        # 构建缓存键
        location_key = location.location_id or city_key
        daily_key = f"{location_key}:{self._settings.qweather_daily_days}"

        # 步骤 2-5：并行概念上独立，实际顺序执行（每个独立缓存）
        now = self._get_cached(
            self._now_cache,
            location_key,
            self._settings.weather_now_cache_seconds,
            lambda: self._fetch_now(location.location_id),
        )
        daily = self._get_cached(
            self._daily_cache,
            daily_key,
            self._settings.weather_daily_cache_seconds,
            lambda: self._fetch_daily(location.location_id),
        )
        minutely = self._get_cached(
            self._minutely_cache,
            location_key,
            self._settings.weather_minutely_cache_seconds,
            lambda: self._fetch_minutely(location),
        )
        alerts = self._get_cached(
            self._alert_cache,
            location_key,
            self._settings.weather_alert_cache_seconds,
            lambda: self._fetch_alerts(location),
        )
        air = self._get_cached(
            self._air_cache,
            location_key,
            self._settings.weather_air_cache_seconds,
            lambda: self._fetch_air(location),
        )

        return WeatherPayload(
            source="qweather",
            location=location,
            now=now,
            daily=daily,
            minutely=minutely,
            alerts=alerts,
            air=air,
        )

    # ── 和风天气各接口调用 ────────────────────────────

    def _lookup_city(self, city: str) -> WeatherLocation:
        """
        城市搜索 — 将中文城市名转换为和风 LocationID。

        调用和风城市搜索 API: /geo/v2/city/lookup?location=<city>&lang=zh

        Args:
            city: 城市中文名称

        Returns:
            WeatherLocation: 城市信息（含 location_id、经纬度、行政区划）

        Raises:
            ValueError: 未找到城市（API 返回空 location 列表）
        """
        data = self._get_json(
            self._settings.qweather_city_lookup_path,
            {"location": city, "lang": "zh"},
        )
        locations = data.get("location") or []
        if not locations:
            raise ValueError(f"未找到城市: {city}")

        # 取 API 返回的第一个结果（通常是最匹配的）
        first = locations[0]
        logger.info("城市搜索 {} → location_id={}", city, first.get("id", ""))
        return WeatherLocation(
            city=first.get("name") or city,
            location_id=first.get("id", ""),
            adm1=first.get("adm1", ""),  # 省份
            adm2=first.get("adm2", ""),  # 城市/区
            latitude=_to_float(first.get("lat")),
            longitude=_to_float(first.get("lon")),
        )

    def _fetch_moon(self, city: str, forecast_date: str) -> MoonPayload:
        """调用和风天文接口并映射指定日期的逐小时月相。"""
        city_key = city.strip().lower()
        location = self._get_cached(
            self._location_cache,
            city_key,
            self._settings.weather_location_cache_seconds,
            lambda: self._lookup_city(city),
        )
        data = self._get_json(
            self._settings.qweather_moon_path,
            {
                "location": location.location_id,
                "date": forecast_date,
                "lang": "zh",
            },
        )
        phases = [
            MoonPhasePoint(
                fx_time=item.get("fxTime", ""),
                value=_to_float(item.get("value")),
                name=item.get("name", ""),
                illumination_percent=_to_int(item.get("illumination")),
                icon=item.get("icon", ""),
            )
            for item in (data.get("moonPhase") or [])[:24]
        ]
        if not phases:
            raise ValueError("和风月相响应缺少 moonPhase")

        return MoonPayload(
            source="qweather",
            updated_at=_parse_datetime(data.get("updateTime")),
            location=location,
            fx_date=datetime.strptime(forecast_date, "%Y%m%d").date().isoformat(),
            moonrise=data.get("moonrise", ""),
            moonset=data.get("moonset", ""),
            phases=phases,
        )

    def _fetch_now(self, location_id: str) -> WeatherNow:
        """
        实时天气查询。

        调用和风实时天气 API: /v7/weather/now?location=<id>&lang=zh&unit=m
        unit=m 表示公制单位（温度 °C，风速 km/h）。

        Args:
            location_id: 和风天气 LocationID

        Returns:
            WeatherNow: 实时天气数据
        """
        data = self._get_json(
            self._settings.qweather_now_path,
            {"location": location_id, "lang": "zh", "unit": "m"},
        )
        now = data.get("now") or {}
        return WeatherNow(
            obs_time=now.get("obsTime", ""),
            temp_c=_to_int(now.get("temp")),          # 当前温度
            feels_like_c=_to_int(now.get("feelsLike")),  # 体感温度
            text=now.get("text", ""),                  # 天气现象（如 '晴'）
            icon=now.get("icon", ""),                   # 图标代码（如 '100'）
            wind_dir=now.get("windDir", ""),            # 风向
            wind_scale=now.get("windScale", ""),        # 风力等级
            humidity_percent=_to_int(now.get("humidity")),  # 湿度
            precip_mm=_to_float(now.get("precip")),          # 降水量
            pressure_hpa=_to_int(now.get("pressure")),       # 气压
            vis_km=_to_int(now.get("vis")),                   # 能见度
        )

    def _fetch_daily(self, location_id: str) -> DailyForecast:
        """
        多日天气预报查询。

        调用和风多日预报 API: /v7/weather/{days}?location=<id>&lang=zh&unit=m
        days 由配置决定（默认 7d）。显示上下文只在服务端网页中使用，
        因此保留前 7 条预报用于温度趋势图，不会增加 ESP32 的 JSON 负担。

        Args:
            location_id: 和风天气 LocationID

        Returns:
            DailyForecast: 多日预报数据
        """
        days = self._settings.qweather_daily_days
        path = self._settings.qweather_daily_path.format(days=days)
        data = self._get_json(
            path,
            {"location": location_id, "lang": "zh", "unit": "m"},
            optional=True,
        )
        if not data:
            return DailyForecast(days=days)

        items = []
        # 固定最多 7 天，保证 800×480 趋势图的横轴密度稳定。
        for item in data.get("daily", [])[:7]:
            items.append(
                DailyForecastItem(
                    fx_date=item.get("fxDate", ""),
                    text_day=item.get("textDay", ""),
                    text_night=item.get("textNight", ""),
                    icon_day=item.get("iconDay", ""),
                    icon_night=item.get("iconNight", ""),
                    temp_min_c=_to_int(item.get("tempMin")),
                    temp_max_c=_to_int(item.get("tempMax")),
                    wind_dir_day=item.get("windDirDay", ""),
                    wind_scale_day=item.get("windScaleDay", ""),
                    humidity_percent=_to_int(item.get("humidity")),
                    precip_mm=_to_float(item.get("precip")),
                    uv_index=_to_int(item.get("uvIndex")),
                    sunrise=item.get("sunrise", ""),
                    sunset=item.get("sunset", ""),
                )
            )
        return DailyForecast(days=days, items=items)

    def _fetch_minutely(self, location: WeatherLocation) -> MinutelyRain:
        """
        分钟级降水预报查询。

        调用和风分钟降水 API: /v7/minutely/5m?location=<经度,纬度>&lang=zh
        返回未来 2 小时以 5 分钟为粒度的降水预报。

        注意：和风天气此接口要求 location=经度,纬度（与城市搜索相反）。

        Args:
            location: 城市位置信息（需含经纬度）

        Returns:
            MinutelyRain: 分钟降水预报数据
        """
        # 无经纬度时无法查询分钟降水
        if location.latitude is None or location.longitude is None:
            return MinutelyRain(summary="缺少经纬度，无法获取分钟降水")

        # 和风分钟降水 API 要求 location=经度,纬度（最多 2 位小数）
        loc = f"{location.longitude:.2f},{location.latitude:.2f}"
        data = self._get_json(
            self._settings.qweather_minutely_path,
            {"location": loc, "lang": "zh"},
            optional=True,
        )
        if not data:
            return MinutelyRain(summary="分钟降水暂不可用")

        items = []
        # 仅取前 24 条（2 小时 × 每 5 分钟一条）
        for item in data.get("minutely", [])[:24]:
            items.append(
                MinutelyRainItem(
                    fx_time=item.get("fxTime", ""),
                    precip_mm=_to_float(item.get("precip")) or 0.0,
                    rain_type=item.get("type", ""),
                )
            )
        return MinutelyRain(summary=data.get("summary", ""), items=items)

    def _fetch_alerts(self, location: WeatherLocation) -> list[WeatherAlert]:
        """
        天气预警查询。

        调用和风天气预警 API:
        /weatheralert/v1/current/{latitude}/{longitude}?lang=zh&localTime=true

        需要经纬度（由城市搜索获取）。若城市搜索结果不含经纬度则跳过预警查询。
        最多返回 5 条预警信息。

        Args:
            location: 城市位置信息（需含经纬度）

        Returns:
            list[WeatherAlert]: 天气预警列表（可能为空）
        """
        if location.latitude is None or location.longitude is None:
            return []

        path = self._settings.qweather_alert_path.format(
            latitude=f"{location.latitude:.2f}",
            longitude=f"{location.longitude:.2f}",
        )
        data = self._get_json(path, {"lang": "zh", "localTime": "true"}, optional=True)
        if not data:
            return []

        # 兼容不同返回字段名：warning（旧版）/ alerts（新版）
        raw_alerts = data.get("warning") or data.get("alerts") or []
        alerts = []
        for item in raw_alerts[:5]:  # 最多取 5 条预警
            event_type = item.get("eventType") or {}
            alerts.append(
                WeatherAlert(
                    title=item.get("title") or item.get("headline", ""),
                    alert_type=item.get("typeName")
                    or item.get("type")
                    or event_type.get("name", ""),
                    severity=item.get("severity") or item.get("level", ""),
                    text=item.get("text")
                    or item.get("description")
                    or item.get("instruction", ""),
                    start_time=item.get("startTime") or item.get("effectiveTime", ""),
                    end_time=item.get("endTime") or item.get("expireTime", ""),
                )
            )
        return alerts

    def _fetch_air(self, location: WeatherLocation) -> WeatherAir | None:
        """
        空气质量实况查询。

        调用和风新版空气质量 API: /airquality/v1/current/{lat}/{lon}?lang=zh
        （开发版域名 .re.qweatherapi.com 下的 v1 接口，旧版 /v7/air/now 不可用）。
        响应的 indexes 数组含多套 AQI 标准，优先取中国国标 cn-mep
        （category 为中文"优/良/轻度污染"…），其次和风通用 qaqi，否则第一个。

        设为可选接口：无经纬度、订阅无权限或请求失败，返回 None（静默降级），
        设备端回退显示 '--'，不影响其他天气数据。

        Args:
            location: 城市位置信息（需含经纬度）

        Returns:
            WeatherAir | None: 空气质量数据；不可用时为 None
        """
        if location.latitude is None or location.longitude is None:
            return None

        path = self._settings.qweather_air_path.format(
            latitude=f"{location.latitude:.2f}",
            longitude=f"{location.longitude:.2f}",
        )
        data = self._get_json(path, {"lang": "zh"}, optional=True)
        if not data:
            return None

        indexes = data.get("indexes") or []
        chosen = next((i for i in indexes if i.get("code") == "cn-mep"), None)
        if chosen is None:
            chosen = next((i for i in indexes if i.get("code") == "qaqi"), None)
        if chosen is None and indexes:
            chosen = indexes[0]
        if not chosen:
            return None

        primary = chosen.get("primaryPollutant") or {}
        return WeatherAir(
            aqi=_to_int(chosen.get("aqi")),
            category=chosen.get("category", ""),
            primary=primary.get("code", ""),
        )

    # ── 底层 HTTP 请求 ─────────────────────────────────

    def _get_json(
        self, path: str, params: dict[str, Any], optional: bool = False
    ) -> dict[str, Any]:
        """
        发送和风天气 API 请求并返回 JSON 数据。

        使用标准库 urllib（无第三方依赖），支持 gzip 压缩。
        请求自动附上 API Key 和 Accept 头。

        错误处理策略：
        - optional=True: API 返回非 200 或网络异常 → 返回空 dict（静默降级）
        - optional=False: API 返回非 200 或网络异常 → 抛出异常（向上传播）

        Args:
            path: API 路径（如 "/v7/weather/now"）
            params: 查询参数 dict（不含 key，自动追加）
            optional: 是否为可选接口（失败时是否静默降级）

        Returns:
            API 响应的 JSON dict

        Raises:
            ValueError: API 返回非 200 状态码（optional=False 时）
            URLError / socket.timeout: 网络错误（optional=False 时）
        """
        query = dict(params)
        # 自动追加 API Key
        query["key"] = self._settings.qweather_api_key

        # 构建完整 URL
        url = f"https://{self._settings.qweather_host}{path}?{urlencode(query)}"

        request = Request(
            url,
            headers={
                "Accept": "application/json",
                "Accept-Encoding": "gzip, identity",  # 支持 gzip 压缩，减少流量
                "User-Agent": "hyper-rlcd-server",
            },
        )

        try:
            with urlopen(request, timeout=self._settings.qweather_timeout_seconds) as response:
                body = response.read()
                # 自动解压 gzip 响应
                if (
                    response.headers.get("Content-Encoding") == "gzip"
                    or body.startswith(b"\x1f\x8b")  # gzip 魔数
                ):
                    body = gzip.decompress(body)
                data = json.loads(body.decode("utf-8"))
        except Exception as exc:
            if optional:
                logger.warning("和风天气网络异常（可选接口静默降级）path={}：{}", path, exc)
                return {}  # 可选接口失败 → 静默降级
            logger.warning("和风天气网络异常 path={}：{}", path, exc)
            raise  # 必要接口失败 → 向上传播

        # 检查 API 返回状态码
        code = str(data.get("code", "200"))
        if code not in {"200", "204"}:
            if optional:
                logger.warning("和风天气返回非200（可选接口静默降级）code={} path={}", code, path)
                return {}  # 可选接口状态码异常 → 静默降级
            logger.warning("和风天气返回非200 code={} path={}", code, path)
            raise ValueError(f"和风天气返回 code={code}")
        return data

    # ── Mock 数据 ──────────────────────────────────────

    def _mock_moon(
        self,
        city: str,
        forecast_date: str,
        error: str = "",
    ) -> MoonPayload:
        """生成结构稳定的月相 mock，供未配置和失败状态保持布局。"""
        target_date = datetime.strptime(forecast_date, "%Y%m%d").date()
        return MoonPayload(
            source="mock",
            location=WeatherLocation(city=city),
            fx_date=target_date.isoformat(),
            moonrise=f"{target_date.isoformat()}T18:12+08:00",
            moonset=(target_date + timedelta(days=1)).isoformat()
            + "T06:24+08:00",
            phases=[
                MoonPhasePoint(
                    fx_time=(
                        f"{target_date.isoformat()}T{hour:02d}:00+08:00"
                    ),
                    value=round(0.52 + hour * 0.001, 3),
                    name="亏凸月",
                    illumination_percent=max(96, 99 - hour // 8),
                    icon="805",
                )
                for hour in range(24)
            ],
            error=error,
        )

    def _mock_weather(self, city: str, error: str = "") -> WeatherPayload:
        """
        生成 mock 天气数据。

        在以下场景使用：
        - 未配置和风天气 API Key
        - API 调用失败（网络异常、返回错误码等）

        返回的数据结构与真实 API 完全一致，确保设备端无需区分 mock/真实模式。

        Args:
            city: 城市名称
            error: 错误描述（正常时为空字符串）

        Returns:
            WeatherPayload: 含固定 mock 数据的天气聚合
        """
        mock_date = datetime.now(UTC).date()
        mock_dates = [
            (mock_date + timedelta(days=offset)).isoformat()
            for offset in range(7)
        ]
        mock_hour = datetime.now(UTC).replace(minute=0, second=0, microsecond=0)
        return WeatherPayload(
            source="mock",
            location=WeatherLocation(city=city),
            now=WeatherNow(
                obs_time=datetime.now(UTC).isoformat(),
                temp_c=26,
                feels_like_c=27,
                text="晴",
                icon="100",
                wind_dir="东南风",
                wind_scale="3",
                humidity_percent=55,
                precip_mm=0.0,
                pressure_hpa=1008,
                vis_km=18,
            ),
            daily=DailyForecast(
                days="7d",
                items=[
                    DailyForecastItem(
                        fx_date=mock_dates[0],
                        text_day="晴",
                        text_night="多云",
                        icon_day="100",
                        icon_night="101",
                        temp_min_c=22,
                        temp_max_c=29,
                        wind_dir_day="东南风",
                        wind_scale_day="3",
                        humidity_percent=55,
                        precip_mm=0.0,
                        uv_index=7,
                        sunrise="06:00",
                        sunset="18:00",
                    ),
                    DailyForecastItem(
                        fx_date=mock_dates[1],
                        text_day="多云",
                        text_night="阴",
                        icon_day="101",
                        icon_night="104",
                        temp_min_c=23,
                        temp_max_c=30,
                        wind_dir_day="东风",
                        wind_scale_day="2",
                        humidity_percent=61,
                        precip_mm=0.0,
                        uv_index=5,
                        sunrise="06:00",
                        sunset="18:00",
                    ),
                    DailyForecastItem(
                        fx_date=mock_dates[2],
                        text_day="小雨",
                        text_night="小雨",
                        icon_day="305",
                        icon_night="305",
                        temp_min_c=21,
                        temp_max_c=27,
                        wind_dir_day="东北风",
                        wind_scale_day="3",
                        humidity_percent=78,
                        precip_mm=3.2,
                        uv_index=2,
                        sunrise="06:00",
                        sunset="18:00",
                    ),
                    DailyForecastItem(
                        fx_date=mock_dates[3],
                        text_day="中雨",
                        text_night="小雨",
                        icon_day="306",
                        icon_night="305",
                        temp_min_c=20,
                        temp_max_c=25,
                        wind_dir_day="北风",
                        wind_scale_day="3-4",
                        humidity_percent=84,
                        precip_mm=8.6,
                        uv_index=1,
                        sunrise="06:01",
                        sunset="17:59",
                    ),
                    DailyForecastItem(
                        fx_date=mock_dates[4],
                        text_day="阴",
                        text_night="多云",
                        icon_day="104",
                        icon_night="151",
                        temp_min_c=21,
                        temp_max_c=27,
                        wind_dir_day="西北风",
                        wind_scale_day="2",
                        humidity_percent=72,
                        precip_mm=0.8,
                        uv_index=3,
                        sunrise="06:01",
                        sunset="17:59",
                    ),
                    DailyForecastItem(
                        fx_date=mock_dates[5],
                        text_day="多云",
                        text_night="晴",
                        icon_day="101",
                        icon_night="150",
                        temp_min_c=22,
                        temp_max_c=30,
                        wind_dir_day="西风",
                        wind_scale_day="2",
                        humidity_percent=63,
                        precip_mm=0.0,
                        uv_index=6,
                        sunrise="06:02",
                        sunset="17:58",
                    ),
                    DailyForecastItem(
                        fx_date=mock_dates[6],
                        text_day="晴",
                        text_night="晴",
                        icon_day="100",
                        icon_night="150",
                        temp_min_c=23,
                        temp_max_c=32,
                        wind_dir_day="西南风",
                        wind_scale_day="2",
                        humidity_percent=57,
                        precip_mm=0.0,
                        uv_index=8,
                        sunrise="06:02",
                        sunset="17:58",
                    ),
                ],
            ),
            minutely=MinutelyRain(
                summary="40 分钟后有小雨，持续约 30 分钟",
                items=[
                    MinutelyRainItem(
                        fx_time=(mock_hour + timedelta(minutes=index * 5)).isoformat(),
                        precip_mm=max(0.0, round(0.35 - abs(index - 12) * 0.035, 2)),
                        rain_type="rain",
                    )
                    for index in range(24)
                ],
            ),
            air=WeatherAir(aqi=58, category="良", primary="pm2p5"),
            error=error,
        )


# ── 工具函数 ──────────────────────────────────────────────

def _to_int(value: Any) -> int | None:
    """
    安全类型转换：任意值 → int | None。

    和风 API 的数值字段可能返回 int、float、字符串或 null。
    此函数统一处理各种类型，转换失败时返回 None（而非抛异常）。
    """
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _to_float(value: Any) -> float | None:
    """
    安全类型转换：任意值 → float | None。

    与 _to_int 同理，处理和风 API 返回的各种可能类型。
    """
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _parse_datetime(value: Any) -> datetime:
    """解析和风 ISO 时间，缺失或异常时回退当前 UTC 时间。"""
    if isinstance(value, str) and value:
        try:
            parsed = datetime.fromisoformat(value)
            return parsed if parsed.tzinfo is not None else parsed.replace(tzinfo=UTC)
        except ValueError:
            pass
    return datetime.now(UTC)
