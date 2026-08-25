"""官方 ESP-IDF CI 构建前后 OTA 处理测试。"""

import hashlib
import json

import pytest

import ci_ota


def _fixture_product(tmp_path):
    project_dir = tmp_path / "devices" / "sample"
    project_dir.mkdir(parents=True)
    (project_dir / "CMakeLists.txt").write_text("project(sample)\n", encoding="utf-8")
    products_path = tmp_path / "products.toml"
    products_path.write_text(
        """
[products.sample]
product_id = 7
firmware_target = "sample_esp32s3_v1"
project_dir = "devices/sample"
firmware_file = "sample.bin"
default_port = "COM1"
""".strip()
        + "\n",
        encoding="utf-8",
    )
    product = ci_ota.load_product(
        "sample",
        suite_root=tmp_path,
        products_path=products_path,
    )
    return product, project_dir


def test_prepare_build_writes_identity_header_and_paths(tmp_path):
    product, project_dir = _fixture_product(tmp_path)

    outputs = ci_ota.prepare_build(
        product,
        suite_root=tmp_path,
        ota_version=1_780_000_000_000,
    )

    header = project_dir / "build" / "generated" / "firmware_ota_build_project.h"
    text = header.read_text(encoding="utf-8")
    assert "DESKSUITE_PRODUCT_ID UINT32_C(7)" in text
    assert 'DESKSUITE_FIRMWARE_TARGET "sample_esp32s3_v1"' in text
    assert "FIRMWARE_OTA_BUILD_VERSION UINT64_C(1780000000000)" in text
    assert outputs["project_dir"] == "devices/sample"
    assert outputs["firmware_file"] == "sample.bin"


def test_finalize_release_builds_verified_manifest(tmp_path):
    product, project_dir = _fixture_product(tmp_path)
    build_dir = project_dir / "build"
    build_dir.mkdir()
    firmware = build_dir / "sample.bin"
    firmware.write_bytes(b"official-esp-idf-build")
    artifact_id = "a" * 64
    (build_dir / "image-info.txt").write_text(
        f"App version: 1.2.3\nValidation hash: {artifact_id} (valid)\n",
        encoding="utf-8",
    )

    outputs = ci_ota.finalize_release(
        product,
        1_780_000_000_001,
        suite_root=tmp_path,
    )

    manifest = json.loads((tmp_path / ".release" / "sample_esp32s3_v1.json").read_text())
    app = manifest["artifacts"]["app"]
    assert app["artifact_id"] == artifact_id
    assert app["version"] == "1.2.3"
    assert app["file_sha256"] == hashlib.sha256(firmware.read_bytes()).hexdigest()
    assert outputs["firmware"] == str(firmware.resolve())


def test_finalize_release_rejects_incomplete_image_info(tmp_path):
    product, project_dir = _fixture_product(tmp_path)
    build_dir = project_dir / "build"
    build_dir.mkdir()
    (build_dir / "sample.bin").write_bytes(b"firmware")
    (build_dir / "image-info.txt").write_text("App version: 1.2.3\n", encoding="utf-8")

    with pytest.raises(ValueError, match="Validation hash"):
        ci_ota.finalize_release(product, 1, suite_root=tmp_path)
