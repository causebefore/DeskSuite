"""多产品 OTA 清单选择与全局哈希制品库测试。"""

import hashlib
import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from app.schemas.ota import OtaArtifactState, OtaCheckRequest
from app.services.ota_service import OtaService


PHOTO_TARGET = "photopainter_esp32s3_v1"
DESK_TARGET = "deskmate_esp32s3_v1"
CURRENT_ARTIFACT_ID = "0" * 64


def _publish(
    manifest_dir: Path,
    artifact_dir: Path,
    *,
    product_id: int,
    firmware_target: str,
    artifact_id: str,
    ota_version: int,
    content: bytes,
    download_url: str | None = None,
) -> None:
    """写入一份符合发布工具契约的测试清单和制品。"""
    manifest_dir.mkdir(parents=True, exist_ok=True)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    file_sha256 = hashlib.sha256(content).hexdigest()
    if download_url is None:
        (artifact_dir / f"{artifact_id}.bin").write_bytes(content)
    manifest = {
        "protocol_version": 2,
        "product_id": product_id,
        "firmware_target": firmware_target,
        "artifacts": {
            "app": {
                "version": "1.2.3",
                "ota_version": ota_version,
                "artifact_id": artifact_id,
                "file_sha256": file_sha256,
                "size": len(content),
                **(
                    {"download_url": download_url}
                    if download_url is not None
                    else {}
                ),
            }
        },
    }
    (manifest_dir / f"{firmware_target}.json").write_text(
        json.dumps(manifest),
        encoding="utf-8",
    )


def _request(
    *,
    product_id: int,
    firmware_target: str,
    ota_version: int = 1,
    current_artifact_id: str = CURRENT_ARTIFACT_ID,
    last_invalid_artifact_id: str | None = None,
) -> OtaCheckRequest:
    return OtaCheckRequest(
        protocol_version=2,
        product_id=product_id,
        firmware_target=firmware_target,
        device_id="device-1",
        artifacts={
            "app": OtaArtifactState(
                current_version="1.0.0",
                ota_version=ota_version,
                current_artifact_id=current_artifact_id,
                last_invalid_artifact_id=last_invalid_artifact_id,
            )
        },
    )


def test_check_selects_manifest_by_firmware_target(tmp_path: Path):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    photo_id = "a" * 64
    desk_id = "b" * 64
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id=photo_id,
        ota_version=10,
        content=b"photo",
    )
    _publish(
        manifests,
        artifacts,
        product_id=2,
        firmware_target=DESK_TARGET,
        artifact_id=desk_id,
        ota_version=20,
        content=b"desk",
    )

    response = OtaService(manifests, artifacts).check(
        _request(product_id=2, firmware_target=DESK_TARGET)
    )

    assert response.protocol_version == 2
    assert response.updates["app"].artifact_id == desk_id


def test_check_returns_external_https_url_without_local_artifact(tmp_path: Path):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    artifact_id = "a" * 64
    download_url = (
        "https://github.com/causebefore/desksuite-firmware/releases/download/"
        f"{PHOTO_TARGET}-v10/{artifact_id}.bin"
    )
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id=artifact_id,
        ota_version=10,
        content=b"photo",
        download_url=download_url,
    )

    response = OtaService(manifests, artifacts).check(
        _request(product_id=1, firmware_target=PHOTO_TARGET)
    )

    assert response.updates["app"].url == download_url
    assert not (artifacts / f"{artifact_id}.bin").exists()


@pytest.mark.parametrize(
    "download_url",
    [
        "http://github.com/causebefore/desksuite-firmware/file.bin",
        "https://token@github.com/causebefore/desksuite-firmware/file.bin",
        "//github.com/causebefore/desksuite-firmware/file.bin",
    ],
)
def test_manifest_rejects_unsafe_external_download_url(
    tmp_path: Path,
    download_url: str,
):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id="a" * 64,
        ota_version=10,
        content=b"photo",
        download_url=download_url,
    )

    with pytest.raises(ValidationError):
        OtaService(manifests, artifacts).check(
            _request(product_id=1, firmware_target=PHOTO_TARGET)
        )


def test_protocol_v1_request_is_not_accepted():
    with pytest.raises(ValidationError):
        OtaCheckRequest.model_validate(
            {
                "protocol_version": 1,
                "device_id": "legacy-device",
                "artifacts": {},
            }
        )


def test_check_rejects_product_and_target_identity_mismatch(tmp_path: Path):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id="a" * 64,
        ota_version=10,
        content=b"photo",
    )

    with pytest.raises(ValueError, match="产品与请求不一致"):
        OtaService(manifests, artifacts).check(
            _request(product_id=2, firmware_target=PHOTO_TARGET)
        )


def test_manifest_rejects_legacy_filename_field(tmp_path: Path):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id="a" * 64,
        ota_version=10,
        content=b"photo",
    )
    manifest_path = manifests / f"{PHOTO_TARGET}.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["artifacts"]["app"]["filename"] = "legacy.bin"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    with pytest.raises(ValidationError):
        OtaService(manifests, artifacts).check(
            _request(product_id=1, firmware_target=PHOTO_TARGET)
        )


@pytest.mark.parametrize(
    ("ota_version", "current_id", "invalid_id"),
    [
        (10, CURRENT_ARTIFACT_ID, None),
        (1, "a" * 64, None),
        (1, CURRENT_ARTIFACT_ID, "a" * 64),
    ],
)
def test_check_suppresses_non_update_targets(
    tmp_path: Path,
    ota_version: int,
    current_id: str,
    invalid_id: str | None,
):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id="a" * 64,
        ota_version=10,
        content=b"photo",
    )

    response = OtaService(manifests, artifacts).check(
        _request(
            product_id=1,
            firmware_target=PHOTO_TARGET,
            ota_version=ota_version,
            current_artifact_id=current_id,
            last_invalid_artifact_id=invalid_id,
        )
    )

    assert response.updates == {}


def test_download_only_exposes_artifacts_referenced_by_active_manifest(
    tmp_path: Path,
):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    active_id = "a" * 64
    unreferenced_id = "b" * 64
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id=active_id,
        ota_version=10,
        content=b"active",
    )
    (artifacts / f"{unreferenced_id}.bin").write_bytes(b"unreferenced")
    service = OtaService(manifests, artifacts)

    assert service.resolve_artifact(active_id).name == f"{active_id}.bin"
    with pytest.raises(FileNotFoundError):
        service.resolve_artifact(unreferenced_id)


def test_download_does_not_proxy_external_release_artifact(tmp_path: Path):
    manifests = tmp_path / "manifests"
    artifacts = tmp_path / "artifacts"
    artifact_id = "a" * 64
    _publish(
        manifests,
        artifacts,
        product_id=1,
        firmware_target=PHOTO_TARGET,
        artifact_id=artifact_id,
        ota_version=10,
        content=b"external",
        download_url=(
            "https://github.com/causebefore/desksuite-firmware/releases/download/"
            f"{PHOTO_TARGET}-v10/{artifact_id}.bin"
        ),
    )

    with pytest.raises(FileNotFoundError):
        OtaService(manifests, artifacts).resolve_artifact(artifact_id)
