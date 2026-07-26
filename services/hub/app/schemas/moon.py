"""和风天气月升月落与逐小时月相数据模型。"""

from datetime import UTC, datetime

from pydantic import BaseModel, Field

from app.schemas.weather import WeatherLocation


class MoonPhasePoint(BaseModel):
    """单个小时的月相预报。"""

    fx_time: str = Field(default="", description="月相预报时间（ISO 格式）")
    value: float | None = Field(default=None, description="月相数值（0 到 1）")
    name: str = Field(default="", description="月相名称")
    illumination_percent: int | None = Field(
        default=None,
        description="月亮照明度百分比",
    )
    icon: str = Field(default="", description="和风天气月相图标代码")


class MoonPayload(BaseModel):
    """指定城市和日期的月升、月落及逐小时月相。"""

    source: str = Field(description="数据来源标识：qweather 或 mock")
    updated_at: datetime = Field(
        default_factory=lambda: datetime.now(UTC),
        description="数据更新时间",
    )
    location: WeatherLocation = Field(description="城市位置信息")
    fx_date: str = Field(default="", description="预报日期（YYYY-MM-DD）")
    moonrise: str = Field(default="", description="当天月升时间，可能为空")
    moonset: str = Field(default="", description="当天月落时间，可能为空")
    phases: list[MoonPhasePoint] = Field(
        default_factory=list,
        description="逐小时月相数据，最多 24 条",
    )
    attribution: str = Field(default="QWeather", description="数据来源归属标识")
    stale: bool = Field(default=False, description="是否为回退使用的旧缓存")
    error: str = Field(default="", description="取数失败时的内部错误描述")
