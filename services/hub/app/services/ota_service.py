"""OTA 运行时清单读取、制品比对与受控下载定位。"""

import hashlib
import json
from pathlib import Path

from loguru import logger

from app.schemas.ota import (
    OtaCheckRequest,
    OtaCheckResponse,
    OtaManifest,
    OtaManifestEntry,
    OtaTarget,
)


class OtaService:
    """按请求重新加载当前清单，并仅暴露清单指向的应用固件。"""

    def __init__(self, manifest_path: Path, firmware_dir: Path) -> None:
        self._manifest_path = manifest_path
        self._firmware_dir = firmware_dir.resolve()

    def _load(self) -> OtaManifest:
        """读取并校验运行时清单。"""
        raw = json.loads(self._manifest_path.read_text(encoding="utf-8"))
        return OtaManifest.model_validate(raw)

    def _resolve_entry(self, entry: OtaManifestEntry) -> Path:
        """解析制品路径并验证文件元数据，阻止目录穿越或半发布文件。"""
        path = (self._firmware_dir / entry.filename).resolve()
        if path.parent != self._firmware_dir:
            raise ValueError("OTA 制品路径越界")
        stat = path.stat()
        if stat.st_size != entry.size:
            raise ValueError("OTA 制品大小与清单不一致")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != entry.file_sha256:
            raise ValueError("OTA 制品摘要与清单不一致")
        return path

    def check(self, request: OtaCheckRequest) -> OtaCheckResponse:
        """依据不可变制品标识返回应用固件更新目标。"""
        manifest = self._load()
        current = request.artifacts.get("app")
        target = manifest.artifacts.get("app")
        if current is None or target is None:
            return OtaCheckResponse()

        if target.artifact_id in {
            current.current_artifact_id,
            current.last_invalid_artifact_id,
        }:
            return OtaCheckResponse()

        if target.ota_version <= current.ota_version:
            return OtaCheckResponse()

        self._resolve_entry(target)
        logger.info(
            "OTA 返回应用固件 device_id={} version={} ota_version={} artifact_id={}",
            request.device_id,
            target.version,
            target.ota_version,
            target.artifact_id,
        )
        return OtaCheckResponse(
            updates={
                "app": OtaTarget(
                    version=target.version,
                    ota_version=target.ota_version,
                    artifact_id=target.artifact_id,
                    file_sha256=target.file_sha256,
                    size=target.size,
                    url=f"/api/v1/ota/artifacts/{target.artifact_id}",
                )
            }
        )

    def resolve_artifact(self, artifact_id: str) -> Path:
        """仅返回当前清单中与请求标识完全匹配的应用固件。"""
        manifest = self._load()
        target = manifest.artifacts.get("app")
        if target is None or target.artifact_id != artifact_id:
            raise FileNotFoundError(artifact_id)
        return self._resolve_entry(target)
