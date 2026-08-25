"""DeskSuite 统一构建工具纯逻辑单测。"""
import hashlib
import json
import subprocess
import time
from dataclasses import replace
from types import SimpleNamespace

import pytest

import ds


ARTIFACT_ID = "a" * 64


def test_run_powershell_echo_utf8():
    """run_powershell 能跑通且 UTF-8 输出不被破坏。"""
    result = ds.run_powershell(
        "$env:PYTHONIOENCODING='utf-8'; Write-Output '中文测试'",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0
    assert "中文测试" in result.stdout


def test_ota_version_first_time(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "_now_ms", lambda: 1_700_000_000_000)
    state = tmp_path / "state"
    header = tmp_path / "fw.h"
    v = ds.new_build_ota_version(
        state,
        header,
        ds.PRODUCTS["photopainter"],
        minimum=1,
    )
    assert v == 1_700_000_000_000
    assert state.read_text(encoding="utf-8").strip() == str(v)
    header_text = header.read_text(encoding="utf-8")
    assert "FIRMWARE_OTA_BUILD_VERSION" in header_text
    assert "DESKSUITE_PRODUCT_ID UINT32_C(1)" in header_text
    assert (
        'DESKSUITE_FIRMWARE_TARGET "photopainter_esp32s3_v1"' in header_text
    )
    assert f"UINT64_C({v})" in header_text


def test_ota_version_monotonic_when_now_le_last(tmp_path, monkeypatch):
    """候选时间戳 ≤ 已记录版本时，版本 = last + 1。"""
    monkeypatch.setattr(ds, "_now_ms", lambda: 500)
    state = tmp_path / "state"
    state.write_text("1000\n")
    header = tmp_path / "fw.h"
    v = ds.new_build_ota_version(
        state,
        header,
        ds.PRODUCTS["photopainter"],
        minimum=1,
    )
    assert v == 1001
    assert state.read_text().strip() == "1001"


def test_ota_version_respects_minimum(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "_now_ms", lambda: 10)
    state = tmp_path / "state"
    header = tmp_path / "fw.h"
    v = ds.new_build_ota_version(
        state,
        header,
        ds.PRODUCTS["photopainter"],
        minimum=999,
    )
    assert v == 999


def test_ota_version_maxvalue_rejected(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "_now_ms", lambda: 0)
    state = tmp_path / "state"
    state.write_text(f"{ds.MAX_SAFE_JSON_INTEGER}\n")
    header = tmp_path / "fw.h"
    with pytest.raises(SystemExit):
        ds.new_build_ota_version(
            state,
            header,
            ds.PRODUCTS["photopainter"],
            minimum=1,
        )


def test_ota_version_invalid_state_rejected(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "_now_ms", lambda: 1000)
    state = tmp_path / "state"
    state.write_text("not-a-number\n")
    header = tmp_path / "fw.h"
    with pytest.raises(SystemExit):
        ds.new_build_ota_version(
            state,
            header,
            ds.PRODUCTS["photopainter"],
            minimum=1,
        )


def test_init_build_cache_creates_generated_include_before_reconfigure(tmp_path, monkeypatch):
    """首次配置 CMake 前必须先创建 firmware_ota 注册的 generated include 目录。"""
    build = tmp_path / "build"
    header = build / "generated" / "firmware_ota_build_project.h"
    reconfigure_called = False

    def fake_run_idf_command(args, **kwargs):
        nonlocal reconfigure_called
        reconfigure_called = True
        assert header.parent.is_dir()
        assert args[-1] == "reconfigure"
        return 0

    monkeypatch.setattr(ds, "BUILD_PATH", build)
    monkeypatch.setattr(ds, "OTA_VERSION_HEADER_PATH", header)
    monkeypatch.setattr(ds, "run_idf_command", fake_run_idf_command)

    ds.init_project_build_cache()

    assert reconfigure_called
    assert header.parent.is_dir()


def test_fixed_environment_accepts_idf_selected_ninja_path(tmp_path, monkeypatch):
    """固定环境校验应接受 ESP-IDF 实际写入缓存的用户级 Ninja 路径。"""
    idf_path = tmp_path / "esp-idf"
    python_path = tmp_path / "python.exe"
    ninja_path = tmp_path / ".espressif" / "tools" / "ninja" / "1.12.1" / "ninja.exe"
    build_path = tmp_path / "build"
    idf_path.mkdir()
    python_path.write_bytes(b"python")
    ninja_path.parent.mkdir(parents=True)
    ninja_path.write_bytes(b"ninja")
    build_path.mkdir()
    (build_path / "CMakeCache.txt").write_text(
        f"CMAKE_MAKE_PROGRAM:FILEPATH={str(ninja_path).replace(chr(92), '/')}\n",
        encoding="utf-8",
    )
    (build_path / "config.env").write_text(
        json.dumps({"IDF_PATH": str(idf_path), "IDF_TARGET": "esp32s3"}),
        encoding="utf-8",
    )
    (build_path / "build.ninja").write_text("", encoding="utf-8")

    monkeypatch.setattr(ds, "EXPECTED_IDF_PATH", idf_path)
    monkeypatch.setattr(ds, "EXPECTED_PYTHON_PATH", python_path)
    monkeypatch.setattr(ds, "EXPECTED_NINJA_PATH", ninja_path)
    monkeypatch.setattr(ds, "BUILD_PATH", build_path)
    monkeypatch.setattr(
        ds,
        "run_powershell",
        lambda *args, **kwargs: SimpleNamespace(stdout="1.12.1"),
    )

    ds.check_fixed_environment()


def test_cmd_build_initializes_cache_before_validation_and_identity(tmp_path, monkeypatch):
    """干净构建必须先创建缓存，再校验固定环境并生成产品身份头。"""
    events = []

    class FakeRunLog:
        path = tmp_path / "build.log"
        file_handle = None

        @staticmethod
        def close_and_finalize():
            events.append("close_log")

        @staticmethod
        def read_text():
            return ""

    monkeypatch.setattr(ds, "init_project_build_cache", lambda: events.append("init"))

    def fake_check():
        assert events == ["init"]
        events.append("check")

    def fake_new_version(*args, **kwargs):
        assert events == ["init", "check"]
        events.append("identity")
        return 123

    monkeypatch.setattr(ds, "check_fixed_environment", fake_check)
    monkeypatch.setattr(ds, "new_build_ota_version", fake_new_version)
    monkeypatch.setattr(ds, "RunLog", lambda kind: FakeRunLog())
    monkeypatch.setattr(ds, "run_ninja", lambda **kwargs: events.append("build") or 0)

    assert ds.cmd_build(SimpleNamespace(full_log=False)) == 0
    assert events == ["init", "check", "identity", "build", "close_log"]


def test_flash_monitor_refreshes_build_identity(tmp_path, monkeypatch):
    """烧录并监控也必须刷新产品身份与单调 OTA 版本头。"""
    events = []

    class FakeRunLog:
        path = tmp_path / "flash.log"
        file_handle = None

        @staticmethod
        def close_and_finalize():
            events.append("close_log")

    monkeypatch.setattr(ds, "init_project_build_cache", lambda: events.append("init"))
    monkeypatch.setattr(ds, "check_fixed_environment", lambda: events.append("check"))
    monkeypatch.setattr(
        ds,
        "new_build_ota_version",
        lambda *args, **kwargs: events.append("identity") or 123,
    )
    monkeypatch.setattr(ds, "stop_com_port", lambda port: events.append("stop_port"))
    monkeypatch.setattr(ds, "RunLog", lambda kind: FakeRunLog())
    monkeypatch.setattr(
        ds,
        "run_idf_command",
        lambda *args, **kwargs: events.append("flash") or 0,
    )
    monkeypatch.setattr(ds, "_monitor_tee", lambda port: events.append("monitor") or 0)

    assert ds.cmd_flash_monitor(SimpleNamespace(port="COM9")) == 0
    assert events == [
        "init",
        "check",
        "identity",
        "stop_port",
        "flash",
        "close_log",
        "monitor",
    ]


def test_runlog_creates_timestamped_file_and_latest(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "BUILD_LOG_PATH", tmp_path)
    rl = ds.RunLog("flash")
    assert rl.path.parent == tmp_path
    assert rl.path.name.startswith("ds-flash-")
    assert rl.path.suffix == ".log"
    rl.write("line1\n")
    rl.write("line2\n")
    rl.close_and_finalize()
    assert rl.path.read_text(encoding="utf-8") == "line1\nline2\n"
    latest = tmp_path / "ds-flash-latest.log"
    assert latest.exists()
    assert latest.read_text(encoding="utf-8") == "line1\nline2\n"


def test_runlog_tail(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "BUILD_LOG_PATH", tmp_path)
    rl = ds.RunLog("monitor")
    for i in range(100):
        rl.write(f"l{i}\n")
    rl.close_and_finalize()
    assert rl.tail(3) == ["l97", "l98", "l99"]


def test_find_latest_log(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "BUILD_LOG_PATH", tmp_path)
    # 写三个，按修改时间最新的应被选中
    for name in ["ds-build-old.log", "ds-build-newer.log", "ds-build-newest.log"]:
        p = tmp_path / name
        p.write_text("x", encoding="utf-8")
        time.sleep(0.01)
    found = ds.find_latest_log("build")
    assert found is not None
    assert found.name == "ds-build-newest.log"


def test_find_latest_log_none(tmp_path, monkeypatch):
    monkeypatch.setattr(ds, "BUILD_LOG_PATH", tmp_path)
    assert ds.find_latest_log("build") is None


SAMPLE_BUILD_LOG = """
Bootloader binary size 0x5240 bytes. 0x2dc0 bytes (36%) free.
PhotoPainter_Device.bin binary size 0x174222 bytes. Smallest app partition is 0x400000 bytes. 0x28bde0 bytes (64%) free.
FAILED: components/foo.c.o
fatal error: foo.h: No such file
error: some error
ninja: build stopped
"""


def test_extract_build_summary():
    s = ds.extract_build_summary(SAMPLE_BUILD_LOG)
    assert s["bootloader"] == ("0x5240", "0x2dc0", "36")
    assert s["app"] == ("0x174222", "0x400000", "0x28bde0", "64")


def test_extract_build_summary_empty():
    assert ds.extract_build_summary("nothing here") == {"bootloader": None, "app": None}


def test_extract_failure_summary():
    fails = ds.extract_failure_summary(SAMPLE_BUILD_LOG)
    assert any("FAILED:" in f for f in fails)
    assert any("fatal error:" in f for f in fails)
    assert any("ninja: build stopped" in f for f in fails)
    assert len(fails) <= 20


PARTITIONS_CSV = """
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
font,     data, unknown, 0x1A0000,0x100000,
"""


def test_parse_font_offset():
    assert ds.parse_font_offset(PARTITIONS_CSV) == "0x1A0000"


def test_parse_font_offset_missing():
    assert ds.parse_font_offset("nvs,data,nvs,0x9000,0x6000,\n") is None


# ── Task 7: OTA image-info 解析 / manifest 生成 / 原子发布 ───────────────
IMAGE_INFO_SAMPLE = """
esptool.py v5.0.0
Image header: ...
App version: 1.2.3
Validation hash: a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2 (valid)
"""


def test_parse_image_info():
    artifact_id, version = ds.parse_image_info(IMAGE_INFO_SAMPLE)
    assert artifact_id == "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"
    assert version == "1.2.3"


def test_parse_image_info_missing():
    with pytest.raises(SystemExit):
        ds.parse_image_info("no hash here\nno version either")


def test_build_manifest():
    product = ds.PRODUCTS["photopainter"]
    m = ds.build_manifest(product, "1.2.3", 42, ARTIFACT_ID, "d" * 64, 1024)
    assert m["protocol_version"] == 2
    assert m["product_id"] == 1
    assert m["firmware_target"] == "photopainter_esp32s3_v1"
    assert m["artifacts"]["app"]["version"] == "1.2.3"
    assert m["artifacts"]["app"]["ota_version"] == 42
    assert m["artifacts"]["app"]["artifact_id"] == ARTIFACT_ID
    assert m["artifacts"]["app"]["file_sha256"] == "d" * 64
    assert m["artifacts"]["app"]["size"] == 1024
    assert "filename" not in m["artifacts"]["app"]


def _write_bin(path, content=b"fw-bytes"):
    path.write_bytes(content)
    return hashlib.sha256(content).hexdigest()


def test_publish_firmware_new(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    sha = _write_bin(firmware)
    product = ds.PRODUCTS["photopainter"]
    published = ds.publish_firmware(
        firmware, service, product, "1.2.3", 42, ARTIFACT_ID, sha, len(firmware.read_bytes())
    )
    assert published.name == f"{ARTIFACT_ID}.bin"
    assert published.exists()
    manifest = json.loads(
        (service / "firmwares" / "manifests" / "photopainter_esp32s3_v1.json").read_text(encoding="utf-8")
    )
    assert manifest["artifacts"]["app"]["artifact_id"] == ARTIFACT_ID


def test_publish_firmware_dedup_same_sha(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    sha = _write_bin(firmware)
    product = ds.PRODUCTS["photopainter"]
    ds.publish_firmware(firmware, service, product, "1.2.3", 42, ARTIFACT_ID, sha, len(firmware.read_bytes()))
    # 同 sha 再次发布：不报错，不覆盖内容
    published = ds.publish_firmware(
        firmware, service, product, "1.2.3", 43, ARTIFACT_ID, sha, len(firmware.read_bytes())
    )
    assert published.exists()


def test_publish_firmware_conflict_different_sha(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    _write_bin(firmware, b"original")
    sha_orig = hashlib.sha256(b"original").hexdigest()
    product = ds.PRODUCTS["photopainter"]
    ds.publish_firmware(firmware, service, product, "1.2.3", 42, ARTIFACT_ID, sha_orig, 8)
    # 改固件内容，同 artifact_id 不同 sha
    _write_bin(firmware, b"different")
    sha_diff = hashlib.sha256(b"different").hexdigest()
    with pytest.raises(SystemExit):
        ds.publish_firmware(firmware, service, product, "1.2.3", 43, ARTIFACT_ID, sha_diff, 9)


def test_publish_firmware_out_of_bounds(tmp_path):
    # service_root 不含 app/main.py，应在校验阶段 fail
    service = tmp_path / "fake_service"
    firmware = tmp_path / "fw.bin"
    _write_bin(firmware)
    with pytest.raises(SystemExit):
        ds.publish_firmware(
            firmware,
            service,
            ds.PRODUCTS["photopainter"],
            "1.2.3",
            42,
            ARTIFACT_ID,
            "d" * 64,
            1,
        )


def test_publish_firmware_rejects_invalid_target(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    sha = _write_bin(firmware)
    invalid_product = replace(
        ds.PRODUCTS["photopainter"],
        firmware_target="../outside",
    )

    with pytest.raises(SystemExit):
        ds.publish_firmware(
            firmware,
            service,
            invalid_product,
            "1.2.3",
            42,
            ARTIFACT_ID,
            sha,
            len(firmware.read_bytes()),
        )


def test_publish_firmware_isolates_manifests_and_deduplicates_artifact(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "shared.bin"
    sha = _write_bin(firmware, b"same-content")

    photo_path = ds.publish_firmware(
        firmware,
        service,
        ds.PRODUCTS["photopainter"],
        "1.2.3",
        42,
        ARTIFACT_ID,
        sha,
        len(b"same-content"),
    )
    desk_path = ds.publish_firmware(
        firmware,
        service,
        ds.PRODUCTS["deskmate"],
        "1.2.3",
        84,
        ARTIFACT_ID,
        sha,
        len(b"same-content"),
    )

    assert photo_path == desk_path
    assert len(list((service / "firmwares" / "artifacts").glob("*.bin"))) == 1
    photo_manifest = json.loads(
        (
            service
            / "firmwares"
            / "manifests"
            / "photopainter_esp32s3_v1.json"
        ).read_text(encoding="utf-8")
    )
    desk_manifest = json.loads(
        (
            service / "firmwares" / "manifests" / "deskmate_esp32s3_v1.json"
        ).read_text(encoding="utf-8")
    )
    assert (photo_manifest["product_id"], photo_manifest["firmware_target"]) == (
        1,
        "photopainter_esp32s3_v1",
    )
    assert (desk_manifest["product_id"], desk_manifest["firmware_target"]) == (
        2,
        "deskmate_esp32s3_v1",
    )


def test_publish_firmware_rejects_stale_target_version(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    sha = _write_bin(firmware)
    product = ds.PRODUCTS["photopainter"]
    ds.publish_firmware(
        firmware,
        service,
        product,
        "1.2.3",
        42,
        ARTIFACT_ID,
        sha,
        len(b"fw-bytes"),
    )

    with pytest.raises(SystemExit):
        ds.publish_firmware(
            firmware,
            service,
            product,
            "1.2.3",
            41,
            ARTIFACT_ID,
            sha,
            len(b"fw-bytes"),
        )


def test_load_ota_publish_config_accepts_ubuntu_ssh_docker_profile(tmp_path):
    config_path = tmp_path / "products.toml"
    config_path.write_text(
        """
[ota_publish]
mode = "ssh_docker"
ssh_host = "ubuntu"
remote_service_root = "/opt/appdata/desksuite-hub"
container_name = "desksuite-hub"
container_firmware_root = "/app/firmwares"
runtime_uid = 10001
runtime_gid = 10001
""".strip(),
        encoding="utf-8",
    )

    config = ds.load_ota_publish_config(config_path)

    assert config == ds.OtaPublishConfig(
        mode="ssh_docker",
        ssh_host="ubuntu",
        remote_service_root="/opt/appdata/desksuite-hub",
        container_name="desksuite-hub",
        container_firmware_root="/app/firmwares",
        runtime_uid=10001,
        runtime_gid=10001,
    )


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("ssh_host", "ubuntu;echo-bad"),
        ("container_name", "../hub"),
        ("remote_service_root", "relative/hub"),
        ("container_firmware_root", "/app/../etc"),
        ("runtime_uid", -1),
        ("runtime_gid", True),
    ),
)
def test_load_ota_publish_config_rejects_unsafe_fields(tmp_path, field, value):
    values = {
        "mode": "ssh_docker",
        "ssh_host": "ubuntu",
        "remote_service_root": "/opt/appdata/desksuite-hub",
        "container_name": "desksuite-hub",
        "container_firmware_root": "/app/firmwares",
        "runtime_uid": 10001,
        "runtime_gid": 10001,
    }
    values[field] = value
    config_path = tmp_path / "products.toml"
    config_path.write_text(
        "\n".join(
            (
                "[ota_publish]",
                f'mode = "{values["mode"]}"',
                f'ssh_host = "{values["ssh_host"]}"',
                f'remote_service_root = "{values["remote_service_root"]}"',
                f'container_name = "{values["container_name"]}"',
                f'container_firmware_root = "{values["container_firmware_root"]}"',
                f'runtime_uid = {str(values["runtime_uid"]).lower()}',
                f'runtime_gid = {str(values["runtime_gid"]).lower()}',
            )
        ),
        encoding="utf-8",
    )

    with pytest.raises(SystemExit):
        ds.load_ota_publish_config(config_path)


def test_ota_minimum_from_manifest_text_treats_missing_as_first_publish():
    assert ds.ota_minimum_from_manifest_text(None, ds.PRODUCTS["deskmate"]) == 1


def test_ota_minimum_from_manifest_text_returns_next_remote_version():
    manifest = ds.build_manifest(
        ds.PRODUCTS["deskmate"], "1.0.2", 42, ARTIFACT_ID, "b" * 64, 1024
    )

    assert (
        ds.ota_minimum_from_manifest_text(
            json.dumps(manifest), ds.PRODUCTS["deskmate"]
        )
        == 43
    )


def test_ota_minimum_from_manifest_text_rejects_wrong_identity():
    manifest = ds.build_manifest(
        ds.PRODUCTS["photopainter"], "1.0.2", 42, ARTIFACT_ID, "b" * 64, 1024
    )

    with pytest.raises(SystemExit):
        ds.ota_minimum_from_manifest_text(
            json.dumps(manifest), ds.PRODUCTS["deskmate"]
        )


@pytest.mark.parametrize("ota_version", (True, ds.MAX_SAFE_JSON_INTEGER))
def test_ota_minimum_from_manifest_text_rejects_invalid_version(ota_version):
    manifest = ds.build_manifest(
        ds.PRODUCTS["deskmate"], "1.0.2", 42, ARTIFACT_ID, "b" * 64, 1024
    )
    manifest["artifacts"]["app"]["ota_version"] = ota_version

    with pytest.raises(SystemExit):
        ds.ota_minimum_from_manifest_text(
            json.dumps(manifest), ds.PRODUCTS["deskmate"]
        )


def test_resolve_ota_publish_target_defaults_to_remote_profile():
    mode, target = ds.resolve_ota_publish_target(None)

    assert mode == "remote"
    assert target == ds.load_ota_publish_config()


def test_resolve_ota_publish_target_uses_explicit_local_service_root(tmp_path):
    service_root = tmp_path / "hub"
    (service_root / "app").mkdir(parents=True)
    (service_root / "app" / "main.py").write_text("ok", encoding="utf-8")

    mode, target = ds.resolve_ota_publish_target(str(service_root))

    assert mode == "local"
    assert target == service_root.resolve()


class _RemoteRunner:
    def __init__(self, manifest_text, *, fail_scp=False, mount_source=None):
        self.manifest_text = manifest_text
        self.fail_scp = fail_scp
        self.mount_source = mount_source or "/opt/appdata/desksuite-hub/firmwares"
        self.calls = []

    def __call__(self, argv, *, capture_output=False, input_text=None):
        call = tuple(str(value) for value in argv)
        self.calls.append(call)
        stdout = ""
        returncode = 0
        if call[:4] == ("ssh", "ubuntu", "docker", "inspect"):
            stdout = json.dumps(
                [
                    {
                        "State": {
                            "Status": "running",
                            "Health": {"Status": "healthy"},
                        },
                        "Mounts": [
                            {
                                "Source": self.mount_source,
                                "Destination": "/app/firmwares",
                                "RW": True,
                            }
                        ],
                    }
                ]
            )
        elif call[0] == "scp" and self.fail_scp:
            returncode = 1
        elif "test" in call and "-f" in call:
            returncode = 0
        elif "cat" in call and call[-1].endswith(".json"):
            stdout = self.manifest_text
        elif "sha256sum" in call:
            manifest = json.loads(self.manifest_text)
            stdout = manifest["artifacts"]["app"]["file_sha256"] + "  artifact.bin\n"
        elif "stat" in call and "-c" in call:
            manifest = json.loads(self.manifest_text)
            stdout = str(manifest["artifacts"]["app"]["size"]) + "\n"
        return subprocess.CompletedProcess(call, returncode, stdout=stdout, stderr="")


def _remote_publish_fixture(tmp_path, *, ota_version=43):
    firmware = tmp_path / "firmware.bin"
    firmware.write_bytes(b"remote-firmware")
    file_sha = hashlib.sha256(firmware.read_bytes()).hexdigest()
    manifest = ds.build_manifest(
        ds.PRODUCTS["deskmate"],
        "1.0.2",
        ota_version,
        ARTIFACT_ID,
        file_sha,
        firmware.stat().st_size,
    )
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return firmware, manifest_path, manifest


def test_remote_ota_publisher_reads_minimum_from_running_mounted_container():
    manifest = ds.build_manifest(
        ds.PRODUCTS["deskmate"], "1.0.2", 42, ARTIFACT_ID, "b" * 64, 1024
    )
    runner = _RemoteRunner(json.dumps(manifest))
    publisher = ds.RemoteOtaPublisher(ds.load_ota_publish_config(), runner=runner)

    minimum = publisher.minimum_ota_version(ds.PRODUCTS["deskmate"])

    assert minimum == 43
    assert runner.calls[0] == ("ssh", "ubuntu", "docker", "inspect", "desksuite-hub")
    assert any("cat" in call for call in runner.calls)


def test_remote_ota_publisher_rejects_unexpected_firmware_mount():
    runner = _RemoteRunner("", mount_source="/wrong/firmwares")
    publisher = ds.RemoteOtaPublisher(ds.load_ota_publish_config(), runner=runner)

    with pytest.raises(ds.OtaPublishError):
        publisher.minimum_ota_version(ds.PRODUCTS["deskmate"])


def test_remote_ota_publisher_uploads_runs_helper_verifies_and_cleans(tmp_path):
    firmware, manifest_path, manifest = _remote_publish_fixture(tmp_path)
    runner = _RemoteRunner(json.dumps(manifest))
    publisher = ds.RemoteOtaPublisher(ds.load_ota_publish_config(), runner=runner)

    published = publisher.publish(
        firmware,
        manifest_path,
        ds.PRODUCTS["deskmate"],
    )

    assert published.endswith(f"/artifacts/{ARTIFACT_ID}.bin")
    scp_indexes = [index for index, call in enumerate(runner.calls) if call[0] == "scp"]
    helper_index = next(
        index
        for index, call in enumerate(runner.calls)
        if "remote_ota_publish.py" in " ".join(call) and "--firmware-root" in call
    )
    verify_index = next(
        index for index, call in enumerate(runner.calls) if "sha256sum" in call
    )
    assert len(scp_indexes) == 3
    assert max(scp_indexes) < helper_index < verify_index
    assert any(call[:3] == ("ssh", "ubuntu", "rm") and "-f" in call for call in runner.calls)
    assert any(
        call[:6] == ("ssh", "ubuntu", "docker", "exec", "--user", "0")
        and "rm" in call
        for call in runner.calls
    )


def test_remote_ota_publisher_cleans_staging_when_scp_fails(tmp_path):
    firmware, manifest_path, manifest = _remote_publish_fixture(tmp_path)
    runner = _RemoteRunner(json.dumps(manifest), fail_scp=True)
    publisher = ds.RemoteOtaPublisher(ds.load_ota_publish_config(), runner=runner)

    with pytest.raises(ds.OtaPublishError):
        publisher.publish(
            firmware,
            manifest_path,
            ds.PRODUCTS["deskmate"],
        )

    assert any(call[:3] == ("ssh", "ubuntu", "rm") and "-f" in call for call in runner.calls)
