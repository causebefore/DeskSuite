"""
天气聚合数据的 Pydantic 模型，供服务端网页渲染与语音工具使用。

数据来源：和风天气 QWeather API（免费订阅）或 mock 数据。

模型层次：
- WeatherPayload     — 顶层聚合，包含所有天气模块
  ├── WeatherLocation — 城市信息（名称、ID、经纬度、行政区划）
  ├── WeatherNow      — 实时天气（温度、体感、风力、湿度、气压等）
  ├── DailyForecast   — 多日预报
  │   └── DailyForecastItem — 单日预报
  ├── MinutelyRain    — 分钟级降水预报（未来 2 小时）
  │   └── MinutelyRainItem  — 单个时间点的降水数据
  └── WeatherAlert    — 天气预警（台风、暴雨、高温等）

设计要点：
- 所有非核心字段都有默认值，保证 API 部分失败时仍能返回可用数据
- 温度、湿度等数值字段为可选 int/float（API 可能缺失个别字段）
- MinutelyRain 需要经纬度，城市搜索成功后自动获取
"""

from datetime import UTC, datetime

from pydantic import BaseModel, Field


class WeatherLocation(BaseModel):
    """
    城市位置信息。

    由和风天气城市搜索 API (/geo/v2/city/lookup) 返回。
    location_id 用于后续天气查询，经纬度用于分钟降水与预警接口。
    """
    city: str = Field(description="城市中文名称（如 '苏州'、'北京'）")
    location_id: str = Field(
        default="",
        description="和风天气 LocationID，后续天气 API 查询的主键",
    )
    adm1: str = Field(default="", description="一级行政区划（省份）")
    adm2: str = Field(default="", description="二级行政区划（城市/区）")
    latitude: float | None = Field(
        default=None,
        description="纬度（用于分钟降水与预警查询）",
    )
    longitude: float | None = Field(
        default=None,
        description="经度（用于分钟降水与预警查询）",
    )


class WeatherNow(BaseModel):
    """
    实时天气数据。

    由和风天气实时天气 API (/v7/weather/now) 返回。
    温度单位为摄氏度（unit=m），风速为风力等级。
    """
    obs_time: str = Field(default="", description="观测时间（ISO 格式）")
    temp_c: int | None = Field(default=None, description="当前温度（°C）")
    feels_like_c: int | None = Field(default=None, description="体感温度（°C）")
    text: str = Field(default="", description="天气现象文字（如 '晴'、'多云'、'小雨'）")
    icon: str = Field(default="", description="天气图标代码（如 '100'、'305'）")
    wind_dir: str = Field(default="", description="风向（如 '北风'、'东南风'）")
    wind_scale: str = Field(default="", description="风力等级（如 '3-4'）")
    humidity_percent: int | None = Field(default=None, description="相对湿度（%）")
    precip_mm: float | None = Field(default=None, description="降水量（mm）")
    pressure_hpa: int | None = Field(default=None, description="大气压强（hPa）")
    vis_km: int | None = Field(default=None, description="能见度（km）")


class MinutelyRainItem(BaseModel):
    """
    单个时间点的分钟降水预报。

    由和风天气分钟降水 API (/v7/minutely/5m) 返回，
    每 5 分钟一个数据点，共返回未来 2 小时的数据。
    """
    fx_time: str = Field(description="预报时间（ISO 格式）")
    precip_mm: float = Field(description="降水量（mm）")
    rain_type: str = Field(default="", description="降水类型（如 'rain'、'snow'）")


class MinutelyRain(BaseModel):
    """
    分钟级降水预报集合。

    包含未来 2 小时内的逐 5 分钟降水数据。
    summary 为人类可读的摘要（如 '未来 2 小时暂无降水'）。
    """
    summary: str = Field(default="", description="降水摘要文字")
    items: list[MinutelyRainItem] = Field(
        default_factory=list,
        description="逐 5 分钟降水数据列表（最多 24 条）",
    )


class DailyForecastItem(BaseModel):
    """
    单日天气预报。

    由和风天气多日预报 API (/v7/weather/3d 等) 返回。
    包含白天与夜间两个时段的预报数据。
    """
    fx_date: str = Field(description="预报日期（YYYY-MM-DD 格式）")
    text_day: str = Field(default="", description="白天天气现象")
    text_night: str = Field(default="", description="夜间天气现象")
    icon_day: str = Field(default="", description="白天天气图标代码")
    icon_night: str = Field(default="", description="夜间天气图标代码")
    temp_min_c: int | None = Field(default=None, description="最低温度（°C）")
    temp_max_c: int | None = Field(default=None, description="最高温度（°C）")
    wind_dir_day: str = Field(default="", description="白天风向")
    wind_scale_day: str = Field(default="", description="白天风力等级")
    humidity_percent: int | None = Field(default=None, description="相对湿度（%）")
    precip_mm: float | None = Field(default=None, description="降水量（mm）")
    uv_index: int | None = Field(default=None, description="紫外线指数")
    sunrise: str = Field(default="", description="日出时间（HH:MM，和风原样透传）")
    sunset: str = Field(default="", description="日落时间（HH:MM，和风原样透传）")


class DailyForecast(BaseModel):
    """
    多日天气预报集合。

    days 标识预报天数（如 "7d"），显示服务最多保留 7 天用于趋势图。
    这些数据只在服务端模板中使用，不会作为业务 JSON 下发给 ESP32。
    """
    days: str = Field(default="7d", description="预报天数标识")
    items: list[DailyForecastItem] = Field(
        default_factory=list,
        description="多日预报数据列表（当前最多 7 条）",
    )


class WeatherAlert(BaseModel):
    """
    天气预警信息。

    由和风天气预警 API (/weatheralert/v1/current) 返回。
    包含台风、暴雨、高温、寒潮等各类气象灾害预警。
    """
    title: str = Field(default="", description="预警标题")
    alert_type: str = Field(default="", description="预警类型（如 '台风'、'暴雨'）")
    severity: str = Field(default="", description="预警严重等级（如 '黄色'、'红色'）")
    text: str = Field(default="", description="预警详细文字")
    start_time: str = Field(default="", description="预警生效时间")
    end_time: str = Field(default="", description="预警失效时间")


class WeatherAir(BaseModel):
    """
    空气质量实况。

    由和风新版空气质量 API (/airquality/v1/current/{lat}/{lon}) 返回，取 indexes 中中国国标 cn-mep 的 aqi 与 category。
    aqi 为空气质量指数，category 为等级文字（如 '优'、'良'、'轻度污染'），
    primary 为首要污染物代码（如 'pm2p5'，无污染物时为空）。
    接口不可用（订阅权限不足或请求失败）时整个 air 块为 None，设备端回退显示 '--'。
    """

    aqi: int | None = Field(default=None, description="空气质量指数（AQI）")
    category: str = Field(default="", description="空气质量等级文字（如 '优'、'良'）")
    primary: str = Field(default="", description="首要污染物代码（如 'pm2p5'）")


class WeatherPayload(BaseModel):
    """
    天气聚合数据的顶层结构。

    WeatherService.get_current_weather() 返回此结构，
    包含从多个和风天气 API 聚合而来的完整天气数据。

    error 字段在数据正常时为空字符串，仅在发生异常时携带错误描述。
    """
    source: str = Field(description="数据来源标识：'qweather' 或 'mock'")
    updated_at: datetime = Field(
        default_factory=lambda: datetime.now(UTC),
        description="数据生成时间（UTC）",
    )
    location: WeatherLocation = Field(description="城市位置信息")
    now: WeatherNow = Field(description="实时天气")
    daily: DailyForecast = Field(
        default_factory=DailyForecast,
        description="多日天气预报",
    )
    minutely: MinutelyRain = Field(
        default_factory=MinutelyRain,
        description="分钟级降水预报",
    )
    alerts: list[WeatherAlert] = Field(
        default_factory=list,
        description="天气预警列表",
    )
    air: WeatherAir | None = Field(
        default=None,
        description="空气质量实况（订阅权限不足或接口失败时为 null）",
    )
    attribution: str = Field(
        default="QWeather",
        description="数据来源归属标识",
    )
    error: str = Field(
        default="",
        description="错误信息（正常时为空，出错时携带可读错误描述）",
    )
