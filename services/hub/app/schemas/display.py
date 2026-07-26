"""显示渲染接口模型。"""

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field, model_validator


class DisplayRenderRequest(BaseModel):
    """请求服务端生成一个新的多页面显示集合。"""

    pages: list[str] | None = Field(
        default=None,
        min_length=1,
        max_length=16,
        description="覆盖配置中的页面顺序；为空时使用 display.pages",
    )
    default_page: str | None = Field(
        default=None,
        min_length=1,
        max_length=80,
        description="覆盖默认页面；必须包含在最终页面集合中",
    )
    dither: bool | None = Field(default=None, description="覆盖全局抖动配置")


class DisplayPageManifest(BaseModel):
    """一个不可变 PPF2 四灰阶页面的下载元数据。"""

    page_id: str = Field(pattern=r"^[a-zA-Z0-9_-]+$")
    content_version: str = Field(pattern=r"^[0-9]{8}-[0-9]{6}$")
    file_size: Literal[96032] = 96032
    crc32: str = Field(pattern=r"^[0-9a-f]{8}$")
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    payload_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    created_at: datetime
    frame_url: str
    preview_url: str


class DisplayCollectionManifest(BaseModel):
    """设备一次同步并原子启用的多页面显示集合。"""

    device_id: str
    protocol_version: Literal[3] = 3
    format: Literal["PPF2"] = "PPF2"
    pixel_format: Literal["GRAY2"] = "GRAY2"
    collection_version: str = Field(pattern=r"^[0-9]{8}-[0-9]{6}$")
    default_page: str = Field(pattern=r"^[a-zA-Z0-9_-]+$")
    width: Literal[800] = 800
    height: Literal[480] = 480
    bits_per_pixel: Literal[2] = 2
    header_size: Literal[32] = 32
    payload_size: Literal[96000] = 96000
    file_size: Literal[96032] = 96032
    pages: list[DisplayPageManifest] = Field(min_length=1, max_length=16)
    created_at: datetime

    @model_validator(mode="after")
    def validate_page_set(self) -> "DisplayCollectionManifest":
        """确保页面 ID 唯一，且默认页面确实存在。"""
        page_ids = [page.page_id for page in self.pages]
        if len(set(page_ids)) != len(page_ids):
            raise ValueError("Manifest 页面 ID 不能重复")
        if self.default_page not in page_ids:
            raise ValueError("Manifest 默认页面不在页面集合中")
        return self


class DisplayScheduledManifest(DisplayCollectionManifest):
    """下发给设备、包含绝对 UTC 刷新计划的 Manifest v3。"""

    next_refresh_at: int = Field(
        ge=1704067200,
        le=4102444799,
        description="设备下一次刷新目标的 UTC Unix 时间戳（秒）",
    )
