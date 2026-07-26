"""
日程聚合数据的 Pydantic 模型，供服务端网页渲染与语音工具使用。

数据来源：iCloud CalDAV（VEVENT）或 mock。
start/end 为 ISO 8601 字符串；date/time 是按设备时区生成的稳定本地字段，
relative 是可直接显示的友好文本，渲染层无需再次做时区换算。
"""

from datetime import UTC, datetime

from pydantic import BaseModel, Field


class CalendarEvent(BaseModel):
    """单条日程事件。"""

    title: str = Field(default="", description="事件标题")
    start: str = Field(default="", description="开始时间（定时事件为 UTC ISO 8601，全天事件为本地日期）")
    end: str = Field(default="", description="结束时间（定时事件为 UTC ISO 8601，全天事件为本地日期）")
    date: str = Field(default="", description="设备时区下的开始日期（YYYY-MM-DD）")
    time: str = Field(default="", description="设备时区下的开始时间（HH:MM，全天事件为空）")
    all_day: bool = Field(default=False, description="是否全天事件")
    location: str = Field(default="", description="地点")
    relative: str = Field(default="", description="本地化友好文本（如 '今天 10:00'、'周日 全天'）")


class CalendarPayload(BaseModel):
    """日程页面的顶层载荷。"""

    source: str = Field(description="数据来源：'icloud' 或 'mock'")
    error: str = Field(default="", description="错误描述（正常为空）")
    generated_at: datetime = Field(
        default_factory=lambda: datetime.now(UTC),
        description="数据生成时间（UTC）",
    )
    range_days: int = Field(default=7, description="拉取的未来天数")
    items: list[CalendarEvent] = Field(
        default_factory=list,
        description="日程事件列表（最多 max_events 条，按开始时间升序）",
    )
