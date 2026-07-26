"""DeskMate Dashboard schema 3 的稳定设备协议模型。"""

from pydantic import BaseModel, ConfigDict, Field


class DashboardDailyItem(BaseModel):
    """单日天气投影。"""

    fx_date: str = ""
    text_day: str = ""
    text_night: str = ""
    icon_day: str = ""
    temp_min_c: int | None = None
    temp_max_c: int | None = None
    sunrise: str = ""
    sunset: str = ""


class DashboardWeather(BaseModel):
    """天气数据块；单源失败时仍保持完整字段形状。"""

    source: str = ""
    updated_at: str = ""
    error: str = ""
    city: str = ""
    text: str = ""
    icon: str = ""
    temp_c: int | None = None
    feels_like_c: int | None = None
    humidity_percent: int | None = None
    wind_dir: str = ""
    wind_scale: str = ""
    pressure_hpa: int | None = None
    precip_mm: float | None = None
    vis_km: int | None = None
    daily: list[DashboardDailyItem] = Field(default_factory=list)
    aqi: int | None = None
    aqi_category: str = ""
    minutely_summary: str = ""
    alert_title: str = ""
    alert_severity: str = ""


class DashboardCalendarItem(BaseModel):
    """单条日程投影。"""

    title: str = ""
    relative: str = ""
    all_day: bool = False
    location: str = ""


class DashboardCalendar(BaseModel):
    """日历数据块。"""

    source: str = ""
    error: str = ""
    items: list[DashboardCalendarItem] = Field(default_factory=list)


class DashboardMailMessage(BaseModel):
    """单封邮件摘要投影。"""

    from_name: str = ""
    subject: str = ""
    date_text: str = ""
    unread: bool = False


class DashboardMail(BaseModel):
    """邮件数据块。"""

    source: str = ""
    error: str = ""
    unread_count: int | None = None
    messages: list[DashboardMailMessage] = Field(default_factory=list)


class DashboardQuotaLimit(BaseModel):
    """单项额度投影。"""

    type: str = ""
    used_percent: float | None = None
    remaining_percent: float | None = None
    next_reset: str = ""


class DashboardQuota(BaseModel):
    """额度数据块。"""

    available: bool = False
    source: str = ""
    level: str = ""
    error: str = ""
    updated_at: str = ""
    limits: list[DashboardQuotaLimit] = Field(default_factory=list)


class DashboardResponse(BaseModel):
    """DeskMate Dashboard schema 3 顶层响应。"""

    model_config = ConfigDict(populate_by_name=True)

    schema_version: int = Field(default=3, alias="schema")
    device_id: str
    generated_at: str
    next_refresh_at_utc: int = Field(gt=0)
    weather: DashboardWeather
    calendar: DashboardCalendar
    mail: DashboardMail
    quota: DashboardQuota
