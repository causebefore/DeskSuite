"""容器内 OTA 发布 helper 的真实文件系统契约测试。"""

import hashlib
import json
import stat

import pytest

import remote_ota_publish as remote


TARGET = "deskmate_esp32s3_v1"
ARTIFACT_A = "a" * 64
ARTIFACT_B = "b" * 64


def _write_release(tmp_path, *, content, ota_version, artifact_id, target=TARGET):
    firmware = tmp_path / f"{artifact_id[:8]}.bin"
    firmware.write_bytes(content)
    manifest = {
        "protocol_version": 2,
        "product_id": 2,
        "firmware_target": target,
        "artifacts": {
            "app": {
                "version": "1.0.2",
                "ota_version": ota_version,
                "artifact_id": artifact_id,
                "file_sha256": hashlib.sha256(content).hexdigest(),
                "size": len(content),
            }
        },
    }
    manifest_path = tmp_path / f"{artifact_id[:8]}.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return firmware, manifest_path, manifest


def _publish(firmware, manifest, root):
    return remote.publish_remote_files(
        firmware,
        manifest,
        root,
        runtime_uid=None,
        runtime_gid=None,
    )


def test_publish_remote_files_writes_verified_artifact_then_manifest(tmp_path):
    firmware, manifest_path, manifest = _write_release(
        tmp_path,
        content=b"new-firmware",
        ota_version=42,
        artifact_id=ARTIFACT_A,
    )
    root = tmp_path / "firmwares"

    published = _publish(firmware, manifest_path, root)

    assert published == root / "artifacts" / f"{ARTIFACT_A}.bin"
    assert published.read_bytes() == b"new-firmware"
    current = json.loads(
        (root / "manifests" / f"{TARGET}.json").read_text(encoding="utf-8")
    )
    assert current == manifest
    assert stat.S_IMODE(published.stat().st_mode) & stat.S_IRUSR


def test_publish_remote_files_deduplicates_matching_artifact(tmp_path):
    firmware, manifest_path, _ = _write_release(
        tmp_path,
        content=b"same-firmware",
        ota_version=42,
        artifact_id=ARTIFACT_A,
    )
    root = tmp_path / "firmwares"
    published = _publish(firmware, manifest_path, root)
    original_mtime = published.stat().st_mtime_ns
    _, next_manifest_path, _ = _write_release(
        tmp_path,
        content=b"same-firmware",
        ota_version=43,
        artifact_id=ARTIFACT_A,
    )

    second = _publish(firmware, next_manifest_path, root)

    assert second == published
    assert second.stat().st_mtime_ns == original_mtime


def test_publish_remote_files_rejects_conflicting_existing_artifact(tmp_path):
    firmware, manifest_path, _ = _write_release(
        tmp_path,
        content=b"expected",
        ota_version=42,
        artifact_id=ARTIFACT_A,
    )
    root = tmp_path / "firmwares"
    artifact_dir = root / "artifacts"
    artifact_dir.mkdir(parents=True)
    (artifact_dir / f"{ARTIFACT_A}.bin").write_bytes(b"conflict")

    with pytest.raises(remote.PublishError):
        _publish(firmware, manifest_path, root)

    assert not (root / "manifests" / f"{TARGET}.json").exists()


def test_publish_remote_files_archives_previous_manifest_and_keeps_artifact(tmp_path):
    first_firmware, first_manifest_path, first_manifest = _write_release(
        tmp_path,
        content=b"first",
        ota_version=42,
        artifact_id=ARTIFACT_A,
    )
    root = tmp_path / "firmwares"
    first_artifact = _publish(first_firmware, first_manifest_path, root)
    second_firmware, second_manifest_path, second_manifest = _write_release(
        tmp_path,
        content=b"second",
        ota_version=43,
        artifact_id=ARTIFACT_B,
    )

    _publish(second_firmware, second_manifest_path, root)

    history = json.loads(
        (
            root
            / "manifests"
            / "history"
            / TARGET
            / "42.json"
        ).read_text(encoding="utf-8")
    )
    current = json.loads(
        (root / "manifests" / f"{TARGET}.json").read_text(encoding="utf-8")
    )
    assert history == first_manifest
    assert current == second_manifest
    assert first_artifact.read_bytes() == b"first"


@pytest.mark.parametrize("ota_version", (41, 42))
def test_publish_remote_files_rejects_non_increasing_version(tmp_path, ota_version):
    first_firmware, first_manifest_path, first_manifest = _write_release(
        tmp_path,
        content=b"first",
        ota_version=42,
        artifact_id=ARTIFACT_A,
    )
    root = tmp_path / "firmwares"
    _publish(first_firmware, first_manifest_path, root)
    second_firmware, second_manifest_path, _ = _write_release(
        tmp_path,
        content=b"second",
        ota_version=ota_version,
        artifact_id=ARTIFACT_B,
    )

    with pytest.raises(remote.PublishError):
        _publish(second_firmware, second_manifest_path, root)

    current = json.loads(
        (root / "manifests" / f"{TARGET}.json").read_text(encoding="utf-8")
    )
    assert current == first_manifest
    assert not (root / "artifacts" / f"{ARTIFACT_B}.bin").exists()


def test_publish_remote_files_rejects_invalid_target_without_writing(tmp_path):
    firmware, manifest_path, _ = _write_release(
        tmp_path,
        content=b"new-firmware",
        ota_version=42,
        artifact_id=ARTIFACT_A,
        target="../outside",
    )
    root = tmp_path / "firmwares"

    with pytest.raises(remote.PublishError):
        _publish(firmware, manifest_path, root)

    assert not root.exists()


def test_publish_remote_files_keeps_current_manifest_when_input_hash_is_wrong(tmp_path):
    first_firmware, first_manifest_path, first_manifest = _write_release(
        tmp_path,
        content=b"first",
        ota_version=42,
        artifact_id=ARTIFACT_A,
    )
    root = tmp_path / "firmwares"
    _publish(first_firmware, first_manifest_path, root)
    second_firmware, second_manifest_path, manifest = _write_release(
        tmp_path,
        content=b"second",
        ota_version=43,
        artifact_id=ARTIFACT_B,
    )
    manifest["artifacts"]["app"]["file_sha256"] = "c" * 64
    second_manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    with pytest.raises(remote.PublishError):
        _publish(second_firmware, second_manifest_path, root)

    current = json.loads(
        (root / "manifests" / f"{TARGET}.json").read_text(encoding="utf-8")
    )
    assert current == first_manifest
    assert not (root / "artifacts" / f"{ARTIFACT_B}.bin").exists()
