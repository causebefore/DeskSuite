"""dm.py 纯逻辑单测。"""
import hashlib
import json
import subprocess
import time

import pytest

import dm


def test_run_powershell_echo_utf8():
    """run_powershell 能跑通且 UTF-8 输出不被破坏。"""
    result = dm.run_powershell(
        "$env:PYTHONIOENCODING='utf-8'; Write-Output '中文测试'",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0
    assert "中文测试" in result.stdout


def test_ota_version_first_time(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "_now_ms", lambda: 1_700_000_000_000)
    state = tmp_path / "state"
    header = tmp_path / "fw.h"
    v = dm.new_build_ota_version(state, header, minimum=1)
    assert v == 1_700_000_000_000
    assert state.read_text(encoding="utf-8").strip() == str(v)
    header_text = header.read_text(encoding="utf-8")
    assert "FIRMWARE_OTA_BUILD_VERSION" in header_text
    assert f"UINT64_C({v})" in header_text


def test_ota_version_monotonic_when_now_le_last(tmp_path, monkeypatch):
    """候选时间戳 ≤ 已记录版本时，版本 = last + 1。"""
    monkeypatch.setattr(dm, "_now_ms", lambda: 500)
    state = tmp_path / "state"
    state.write_text("1000\n")
    header = tmp_path / "fw.h"
    v = dm.new_build_ota_version(state, header, minimum=1)
    assert v == 1001
    assert state.read_text().strip() == "1001"


def test_ota_version_respects_minimum(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "_now_ms", lambda: 10)
    state = tmp_path / "state"
    header = tmp_path / "fw.h"
    v = dm.new_build_ota_version(state, header, minimum=999)
    assert v == 999


def test_ota_version_maxvalue_rejected(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "_now_ms", lambda: 0)
    state = tmp_path / "state"
    state.write_text(f"{(1 << 64) - 1}\n")
    header = tmp_path / "fw.h"
    with pytest.raises(SystemExit):
        dm.new_build_ota_version(state, header, minimum=1)


def test_ota_version_invalid_state_rejected(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "_now_ms", lambda: 1000)
    state = tmp_path / "state"
    state.write_text("not-a-number\n")
    header = tmp_path / "fw.h"
    with pytest.raises(SystemExit):
        dm.new_build_ota_version(state, header, minimum=1)


def test_init_build_cache_creates_generated_include_before_reconfigure(tmp_path, monkeypatch):
    """首次配置 CMake 前必须先创建 firmware_ota 注册的 generated include 目录。"""
    build = tmp_path / "build"
    header = build / "generated" / "firmware_ota_build.h"
    reconfigure_called = False

    def fake_run_idf_command(args, **kwargs):
        nonlocal reconfigure_called
        reconfigure_called = True
        assert header.parent.is_dir()
        assert args[-1] == "reconfigure"
        return 0

    monkeypatch.setattr(dm, "BUILD_PATH", build)
    monkeypatch.setattr(dm, "OTA_VERSION_HEADER_PATH", header)
    monkeypatch.setattr(dm, "run_idf_command", fake_run_idf_command)

    dm.init_project_build_cache()

    assert reconfigure_called
    assert header.parent.is_dir()


def test_runlog_creates_timestamped_file_and_latest(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "BUILD_LOG_PATH", tmp_path)
    rl = dm.RunLog("flash")
    assert rl.path.parent == tmp_path
    assert rl.path.name.startswith("dm-flash-")
    assert rl.path.suffix == ".log"
    rl.write("line1\n")
    rl.write("line2\n")
    rl.close_and_finalize()
    assert rl.path.read_text(encoding="utf-8") == "line1\nline2\n"
    latest = tmp_path / "dm-flash-latest.log"
    assert latest.exists()
    assert latest.read_text(encoding="utf-8") == "line1\nline2\n"


def test_runlog_tail(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "BUILD_LOG_PATH", tmp_path)
    rl = dm.RunLog("monitor")
    for i in range(100):
        rl.write(f"l{i}\n")
    rl.close_and_finalize()
    assert rl.tail(3) == ["l97", "l98", "l99"]


def test_find_latest_log(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "BUILD_LOG_PATH", tmp_path)
    # 写三个，按修改时间最新的应被选中
    for name in ["dm-build-old.log", "dm-build-newer.log", "dm-build-newest.log"]:
        p = tmp_path / name
        p.write_text("x", encoding="utf-8")
        time.sleep(0.01)
    found = dm.find_latest_log("build")
    assert found is not None
    assert found.name == "dm-build-newest.log"


def test_find_latest_log_none(tmp_path, monkeypatch):
    monkeypatch.setattr(dm, "BUILD_LOG_PATH", tmp_path)
    assert dm.find_latest_log("build") is None


SAMPLE_BUILD_LOG = """
Bootloader binary size 0x5240 bytes. 0x2dc0 bytes (36%) free.
PhotoPainter_Device.bin binary size 0x174222 bytes. Smallest app partition is 0x400000 bytes. 0x28bde0 bytes (64%) free.
FAILED: components/foo.c.o
fatal error: foo.h: No such file
error: some error
ninja: build stopped
"""


def test_extract_build_summary():
    s = dm.extract_build_summary(SAMPLE_BUILD_LOG)
    assert s["bootloader"] == ("0x5240", "0x2dc0", "36")
    assert s["app"] == ("0x174222", "0x400000", "0x28bde0", "64")


def test_extract_build_summary_empty():
    assert dm.extract_build_summary("nothing here") == {"bootloader": None, "app": None}


def test_extract_failure_summary():
    fails = dm.extract_failure_summary(SAMPLE_BUILD_LOG)
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
    assert dm.parse_font_offset(PARTITIONS_CSV) == "0x1A0000"


def test_parse_font_offset_missing():
    assert dm.parse_font_offset("nvs,data,nvs,0x9000,0x6000,\n") is None


# ── Task 7: OTA image-info 解析 / manifest 生成 / 原子发布 ───────────────
IMAGE_INFO_SAMPLE = """
esptool.py v5.0.0
Image header: ...
App version: 1.2.3
Validation hash: a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2 (valid)
"""


def test_parse_image_info():
    artifact_id, version = dm.parse_image_info(IMAGE_INFO_SAMPLE)
    assert artifact_id == "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"
    assert version == "1.2.3"


def test_parse_image_info_missing():
    with pytest.raises(SystemExit):
        dm.parse_image_info("no hash here\nno version either")


def test_build_manifest():
    m = dm.build_manifest("1.2.3", 42, "abc", "dead", 1024, "firmware-abc.bin")
    assert m["protocol_version"] == 1
    assert m["artifacts"]["app"]["version"] == "1.2.3"
    assert m["artifacts"]["app"]["ota_version"] == 42
    assert m["artifacts"]["app"]["artifact_id"] == "abc"
    assert m["artifacts"]["app"]["file_sha256"] == "dead"
    assert m["artifacts"]["app"]["size"] == 1024
    assert m["artifacts"]["app"]["filename"] == "firmware-abc.bin"


def _write_bin(path, content=b"fw-bytes"):
    path.write_bytes(content)
    return hashlib.sha256(content).hexdigest()


def test_publish_firmware_new(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    sha = _write_bin(firmware)
    published = dm.publish_firmware(firmware, service, "1.2.3", 42, "abc", sha, len(firmware.read_bytes()), "firmware-abc.bin")
    assert published.name == "firmware-abc.bin"
    assert published.exists()
    manifest = json.loads((service / "firmwares" / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["artifacts"]["app"]["artifact_id"] == "abc"


def test_publish_firmware_dedup_same_sha(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    sha = _write_bin(firmware)
    # 先发布一次
    dm.publish_firmware(firmware, service, "1.2.3", 42, "abc", sha, len(firmware.read_bytes()), "firmware-abc.bin")
    # 同 sha 再次发布：不报错，不覆盖内容
    published = dm.publish_firmware(firmware, service, "1.2.3", 43, "abc", sha, len(firmware.read_bytes()), "firmware-abc.bin")
    assert published.exists()


def test_publish_firmware_conflict_different_sha(tmp_path):
    service = tmp_path / "hub"
    (service / "app").mkdir(parents=True)
    (service / "app" / "main.py").write_text("ok")
    firmware = tmp_path / "fw.bin"
    _write_bin(firmware, b"original")
    sha_orig = hashlib.sha256(b"original").hexdigest()
    dm.publish_firmware(firmware, service, "1.2.3", 42, "abc", sha_orig, 8, "firmware-abc.bin")
    # 改固件内容，同 artifact_id 不同 sha
    _write_bin(firmware, b"different")
    sha_diff = hashlib.sha256(b"different").hexdigest()
    with pytest.raises(SystemExit):
        dm.publish_firmware(firmware, service, "1.2.3", 43, "abc", sha_diff, 9, "firmware-abc.bin")


def test_publish_firmware_out_of_bounds(tmp_path):
    # service_root 不含 app/main.py，应在校验阶段 fail
    service = tmp_path / "fake_service"
    firmware = tmp_path / "fw.bin"
    _write_bin(firmware)
    with pytest.raises(SystemExit):
        dm.publish_firmware(firmware, service, "1.2.3", 42, "abc", "x", 1, "firmware-abc.bin")
