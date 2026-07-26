"""RSS 聚合数据模型，供显示上下文服务使用。"""

from datetime import UTC, datetime

from pydantic import BaseModel, Field


class RssArticle(BaseModel):
    """一篇经过归一化的 RSS/Atom 文章。"""

    title: str = Field(default="", description="文章标题")
    source: str = Field(default="", description="订阅源名称")
    link: str = Field(default="", description="文章链接，仅供服务端去重")
    published_at: str = Field(default="", description="发布时间（UTC ISO 8601）")
    published_text: str = Field(default="--", description="设备时区下的稳定显示文本")


class RssFeedSummary(BaseModel):
    """一个订阅源的解析结果摘要。"""

    title: str = Field(default="", description="订阅源名称")
    url: str = Field(default="", description="订阅地址")
    item_count: int = Field(default=0, ge=0, description="本次保留的文章数量")


class RssPayload(BaseModel):
    """RSS 页面使用的聚合载荷。"""

    source: str = Field(default="rss", description="数据来源")
    available: bool = Field(default=False, description="是否至少成功读取一个订阅源")
    stale: bool = Field(default=False, description="是否为回退的过期缓存")
    error: str = Field(default="", description="错误摘要（正常为空）")
    generated_at: datetime = Field(
        default_factory=lambda: datetime.now(UTC),
        description="数据生成时间（UTC）",
    )
    feeds: list[RssFeedSummary] = Field(default_factory=list)
    items: list[RssArticle] = Field(default_factory=list)
