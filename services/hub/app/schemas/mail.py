"""
邮箱聚合数据的 Pydantic 模型，供服务端网页渲染与语音工具使用。

数据来源：foxmail IMAP（只读）或 mock。只下发邮件元信息，不含正文。
date 为 UTC ISO 8601；date_text 是服务端按设备时区算好的本地文本。
"""

from datetime import UTC, datetime

from pydantic import BaseModel, Field


class MailMessage(BaseModel):
    """单封邮件的元信息。"""

    uid: int | str = Field(default="", description="IMAP UID")
    from_name: str = Field(default="", description="发件人显示名（解析失败回退地址）")
    subject: str = Field(default="", description="主题")
    date: str = Field(default="", description="邮件时间（UTC ISO 8601）")
    date_text: str = Field(default="", description="本地化时间文本（如 '07-04 09:00'）")
    unread: bool = Field(default=False, description="是否未读")


class MailPayload(BaseModel):
    """邮箱页面的顶层载荷。"""

    source: str = Field(description="数据来源：'qq-imap' 或 'mock'")
    error: str = Field(default="", description="错误描述（正常为空）")
    generated_at: datetime = Field(
        default_factory=lambda: datetime.now(UTC),
        description="数据生成时间（UTC）",
    )
    unread_count: int = Field(default=0, description="未读邮件数")
    messages: list[MailMessage] = Field(
        default_factory=list,
        description="最近邮件列表（最多 max_messages 条，按时间倒序）",
    )
