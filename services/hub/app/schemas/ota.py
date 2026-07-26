"""OTA 制品检查协议的 Pydantic 模型。"""

from typing import Literal

from pydantic import BaseModel, Field


SHA256_PATTERN = r"^[0-9a-f]{64}$"
MAX_SAFE_JSON_INTEGER = 9_007_199_254_740_991


class OtaArtifactState(BaseModel):
    """设备上报的单个制品状态。"""

    current_version: str = Field(min_length=1, max_length=64)
    ota_version: int = Field(default=0, ge=0, le=MAX_SAFE_JSON_INTEGER)
    current_artifact_id: str = Field(pattern=SHA256_PATTERN)
    last_invalid_artifact_id: str | None = Field(
        default=None,
        pattern=SHA256_PATTERN,
    )


class OtaCheckRequest(BaseModel):
    """设备提交的 OTA 制品状态。"""

    protocol_version: Literal[1]
    device_id: str = Field(min_length=1, max_length=80)
    artifacts: dict[str, OtaArtifactState]


class OtaTarget(BaseModel):
    """服务端返回的单个更新目标。"""

    version: str = Field(min_length=1, max_length=64)
    ota_version: int = Field(ge=1, le=MAX_SAFE_JSON_INTEGER)
    artifact_id: str = Field(pattern=SHA256_PATTERN)
    file_sha256: str = Field(pattern=SHA256_PATTERN)
    size: int = Field(gt=0)
    url: str = Field(min_length=1, max_length=256)


class OtaCheckResponse(BaseModel):
    """OTA 检查响应；无更新时 ``updates`` 为空对象。"""

    protocol_version: Literal[1] = 1
    updates: dict[str, OtaTarget] = Field(default_factory=dict)


class OtaManifestEntry(BaseModel):
    """服务端运行时清单中的单个制品。"""

    version: str = Field(min_length=1, max_length=64)
    ota_version: int = Field(default=0, ge=0, le=MAX_SAFE_JSON_INTEGER)
    artifact_id: str = Field(pattern=SHA256_PATTERN)
    file_sha256: str = Field(pattern=SHA256_PATTERN)
    size: int = Field(gt=0)
    filename: str = Field(pattern=r"^[A-Za-z0-9._-]+$", min_length=1, max_length=160)


class OtaManifest(BaseModel):
    """由设备仓库发布工具原子生成的运行时 OTA 清单。"""

    protocol_version: Literal[1]
    artifacts: dict[str, OtaManifestEntry]
