"""OTA 运行时清单读取、制品比对与受控下载定位。"""

import hashlib
import json
import re
from pathlib import Path

from loguru import logger

from app.schemas.ota import (
    FIRMWARE_TARGET_PATTERN,
    OtaCheckRequest,
    OtaCheckResponse,
    OtaManifest,
    OtaManifestEntry,
    OtaTarget,
)


class OtaService:
    """按固件目标加载当前清单，并仅暴露有效清单引用的制品。"""

    def __init__(self, manifest_dir: Path, artifact_dir: Path) -> None:
        self._manifest_dir = manifest_dir.resolve()
        self._artifact_dir = artifact_dir.resolve()

    def _manifest_path(self, firmware_target: str) -> Path:
        """把固件目标映射到清单路径，并拒绝任何目录穿越。"""
        if re.fullmatch(FIRMWARE_TARGET_PATTERN, firmware_target) is None:
            raise ValueError("firmware_target 非法")
        path = (self._manifest_dir / f"{firmware_target}.json").resolve()
        if path.parent != self._manifest_dir:
            raise ValueError("OTA 清单路径越界")
        return path

    def _load(
        self,
        firmware_target: str,
        expected_product_id: int | None = None,
    ) -> OtaManifest:
        """读取目标清单，并验证文件名、清单身份和请求产品一致。"""
        raw = json.loads(
            self._manifest_path(firmware_target).read_text(encoding="utf-8")
        )
        manifest = OtaManifest.model_validate(raw)
        if manifest.firmware_target != firmware_target:
            raise ValueError("OTA 清单固件目标与文件名不一致")
        if (
            expected_product_id is not None
            and manifest.product_id != expected_product_id
        ):
            raise ValueError("OTA 清单产品与请求不一致")
        return manifest

    def _resolve_entry(self, entry: OtaManifestEntry) -> Path:
        """解析制品路径并验证文件元数据，阻止目录穿越或半发布文件。"""
        path = (self._artifact_dir / f"{entry.artifact_id}.bin").resolve()
        if path.parent != self._artifact_dir:
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
        manifest = self._load(
            request.firmware_target,
            expected_product_id=request.product_id,
        )
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

        if target.download_url is None:
            self._resolve_entry(target)
        logger.info(
            "OTA 返回应用固件 product_id={} firmware_target={} device_id={} "
            "version={} ota_version={} artifact_id={}",
            request.product_id,
            request.firmware_target,
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
                    url=(
                        target.download_url
                        or f"/api/v1/ota/artifacts/{target.artifact_id}"
                    ),
                )
            }
        )

    def resolve_artifact(self, artifact_id: str) -> Path:
        """仅返回至少一个当前目标清单引用且摘要匹配的应用固件。"""
        for manifest_path in self._manifest_dir.glob("*.json"):
            firmware_target = manifest_path.stem
            try:
                manifest = self._load(firmware_target)
            except (
                FileNotFoundError,
                OSError,
                json.JSONDecodeError,
                ValueError,
            ):
                continue
            target = manifest.artifacts.get("app")
            if (
                target is not None
                and target.download_url is None
                and target.artifact_id == artifact_id
            ):
                return self._resolve_entry(target)
        raise FileNotFoundError(artifact_id)
