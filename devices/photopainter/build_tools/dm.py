#!/usr/bin/env python3
"""PhotoPainter_Device 构建工具（dm.py）。

整合原 dm.ps1 的全部命令：build / build-log / flash / flash-font /
monitor / flash-monitor / kill-port / clean / menuconfig / ota，以及内部
子命令 _monitor_run。dm.ps1 作为薄 wrapper 转发到本文件。

设计依据：docs/superpowers/specs/2026-07-20-build-tools-py-migration-design.md
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

# ── 固定编译环境（与原 dm.ps1 一致，CLAUDE.md 硬约束）──────────────────────
EXPECTED_IDF_PATH = Path(r"C:\esp\v6.0.1\esp-idf")
EXPECTED_PYTHON_PATH = Path(
    r"C:\Users\lbq08\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe"
)
EXPECTED_NINJA_PATH = Path(r"C:\Espressif\tools\ninja\1.12.1\ninja.exe")
EXPECTED_NINJA_VERSION = "1.12.1"

# ── 仓库与构建路径 ──────────────────────────────────────────────────────
REPO_ROOT = Path(__file__).resolve().parent.parent
PROJECT_ROOT = REPO_ROOT
BUILD_PATH = PROJECT_ROOT / "build"
BUILD_LOG_PATH = BUILD_PATH / "logs"

# ── OTA 版本 ────────────────────────────────────────────────────────────
OTA_VERSION_STATE_PATH = Path(__file__).resolve().parent / ".ota-version-state"
OTA_VERSION_HEADER_PATH = BUILD_PATH / "generated" / "firmware_ota_build.h"

DEFAULT_PORT = "COM5"

# 子命令集合（不含内部 _monitor_run）
PUBLIC_COMMANDS = (
    "build",
    "build-log",
    "flash",
    "flash-font",
    "monitor",
    "flash-monitor",
    "kill-port",
    "clean",
    "menuconfig",
    "ota",
    "flash-log",
    "monitor-log",
)

UINT64_MAX = (1 << 64) - 1


def _now_ms() -> int:
    """当前 UTC 毫秒时间戳。测试可 monkeypatch。"""
    return int(time.time() * 1000)


def new_build_ota_version(state_path: Path, header_path: Path, minimum: int = 1) -> int:
    """生成单调递增的 OTA 版本，写状态文件并生成版本头文件。

    候选 = 当前 UTC 毫秒；若 ≤ 已记录版本则取 记录值+1；最终不低于 minimum。
    状态文件或头文件写入失败、版本达上限、状态文件损坏时 fail()。
    """
    candidate = _now_ms()
    if state_path.exists():
        text = state_path.read_text(encoding="utf-8").strip()
        try:
            last = int(text)
        except ValueError:
            fail(f"本地 OTA 版本状态无效：{state_path}")
        if candidate <= last:
            if last == UINT64_MAX:
                fail("本地 OTA 版本已达到上限")
            candidate = last + 1
    if candidate < minimum:
        candidate = minimum
    if candidate > UINT64_MAX:
        fail("本地 OTA 版本已达到上限")

    header_path.parent.mkdir(parents=True, exist_ok=True)
    state_path.write_text(f"{candidate}\n", encoding="utf-8")
    header = (
        "/** @file firmware_ota_build.h @brief 由 dm.py 为当前固件构建生成的 OTA 版本。 */\n"
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"#define FIRMWARE_OTA_BUILD_VERSION UINT64_C({candidate})\n"
    )
    header_path.write_text(header, encoding="utf-8")
    print(f"OTA 版本：{candidate}")
    return candidate


def check_fixed_environment() -> None:
    """校验现有 CMake 构建目录及其固定工具绑定，不一致时 fail()。

    等价原 dm.ps1 的 Test-FixedBuildEnvironment。
    """
    cmake_cache = BUILD_PATH / "CMakeCache.txt"
    config_env = BUILD_PATH / "config.env"
    ninja_build = BUILD_PATH / "build.ninja"
    for required in (
        EXPECTED_IDF_PATH,
        EXPECTED_PYTHON_PATH,
        EXPECTED_NINJA_PATH,
        cmake_cache,
        config_env,
        ninja_build,
    ):
        if not required.exists():
            fail(f"固定编译环境或构建缓存缺失，已停止：{required}")

    cache_text = cmake_cache.read_text(encoding="utf-8")
    expected_ninja_cmake = str(EXPECTED_NINJA_PATH).replace("\\", "/")
    if f"CMAKE_MAKE_PROGRAM:FILEPATH={expected_ninja_cmake}" not in cache_text:
        fail("CMake 缓存未绑定固定 Ninja，已停止")

    env = json.loads(config_env.read_text(encoding="utf-8"))
    cached_idf = Path(env["IDF_PATH"]).resolve()
    if cached_idf != EXPECTED_IDF_PATH.resolve() or env.get("IDF_TARGET") != "esp32s3":
        fail("CMake 缓存的 ESP-IDF 或目标芯片不符合编译历史，已停止")

    ninja_version = run_powershell(
        f"& '{EXPECTED_NINJA_PATH}' --version", stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    ).stdout.strip()
    if ninja_version != EXPECTED_NINJA_VERSION:
        fail(f"Ninja 版本不一致，期望 {EXPECTED_NINJA_VERSION}，实际 {ninja_version}")

    print("✅ 编译环境校验通过：固定 CMake 缓存 / ESP-IDF v6.0.1 / ESP32-S3 / Ninja 1.12.1")


def init_project_build_cache() -> None:
    """首次构建时生成固定环境的 CMake 缓存。等价原 dm.ps1 Initialize-ProjectBuildCache。"""
    # firmware_ota 在 CMake 配置阶段把该目录注册为私有 include 目录；首次构建或 clean 后，
    # 版本头尚未生成，也必须先保证目录存在，否则 IDF 会在组件注册阶段拒绝继续配置。
    OTA_VERSION_HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    cmake_cache = BUILD_PATH / "CMakeCache.txt"
    config_env = BUILD_PATH / "config.env"
    ninja_build = BUILD_PATH / "build.ninja"
    if cmake_cache.exists() and config_env.exists() and ninja_build.exists():
        return
    write_step("配置目标工程")
    rc = run_idf_command(["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "reconfigure"])
    if rc != 0:
        fail("目标工程 CMake 配置失败")


def _ps_quote(value: str) -> str:
    """PowerShell 单引号转义。"""
    return "'" + value.replace("'", "''") + "'"


def _idf_prefix() -> str:
    """构造 dot-source export.ps1 的 PowerShell 前缀（设置 IDF 环境 + UTF-8）。"""
    export = EXPECTED_IDF_PATH / "export.ps1"
    return (
        f"if (!(Test-Path {_ps_quote(str(export))})) {{ Write-Error 'ESP-IDF export.ps1 不存在：{export}'; exit 1 }}; "
        f". {_ps_quote(str(export))} *> $null; "
        "$env:PYTHONIOENCODING='utf-8'; "
    )


def run_idf_command(idf_args: list[str], *, stdout=None, stderr=None) -> int:
    """在加载了 IDF 环境的 PowerShell 子进程里执行 idf.py <args>。返回退出码。"""
    script = _idf_prefix() + "& idf.py " + " ".join(_ps_quote(a) for a in idf_args)
    return run_powershell(script, stdout=stdout, stderr=stderr).returncode


def run_ninja(stdout=None, stderr=None) -> int:
    """在加载了 IDF 环境的 PowerShell 子进程里执行 ninja -C <build>。"""
    script = _idf_prefix() + f"& {_ps_quote(str(EXPECTED_NINJA_PATH))} -C {_ps_quote(str(BUILD_PATH))}"
    return run_powershell(script, stdout=stdout, stderr=stderr).returncode


def run_esptool(esptool_args: list[str], *, stdout=None, stderr=None) -> tuple[int, str]:
    """在加载了 IDF 环境的 PowerShell 子进程里执行 esptool <args>。

    返回 (退出码, 合并的 stdout+stderr 文本)。
    """
    script = _idf_prefix() + "& esptool.py " + " ".join(_ps_quote(a) for a in esptool_args) + " 2>&1"
    result = run_powershell(script, stdout=stdout if stdout is not None else subprocess.PIPE,
                            stderr=stderr)
    out = result.stdout if isinstance(result.stdout, str) else ""
    return result.returncode, out


def _timestamp() -> str:
    """日志文件名用时间戳：yyyyMMdd-HHmmss-fff。

    dm.py 运行时可用 datetime.now()，不受 workflow 脚本限制。
    """
    now = datetime.now()
    return now.strftime("%Y%m%d-%H%M%S-") + f"{now.microsecond // 1000:03d}"


def latest_log_path(kind: str) -> Path:
    """给定日志类型，返回对应的 dm-{kind}-latest.log 路径。"""
    return BUILD_LOG_PATH / f"dm-{kind}-latest.log"


def find_latest_log(kind: str) -> Path | None:
    """返回 dm-{kind}-*.log 中修改时间最新的一个；无则 None。

    排除 dm-{kind}-latest.log 镜像自身。
    """
    matches = sorted(
        BUILD_LOG_PATH.glob(f"dm-{kind}-*.log"),
        key=lambda p: p.stat().st_mtime,
    )
    matches = [p for p in matches if not p.name.endswith("-latest.log")]
    return matches[-1] if matches else None


class RunLog:
    """日志落盘器：带时间戳新文件 + -latest 镜像。

    kind ∈ "build" | "flash" | "monitor"。__init__ 即在 BUILD_LOG_PATH 下创建
    dm-{kind}-<timestamp>.log；close_and_finalize() 关句柄并 shutil.copy2 到
    dm-{kind}-latest.log 供 build-log / flash-log / monitor-log 查看。
    """

    def __init__(self, kind: str):
        if kind not in ("build", "flash", "monitor"):
            fail(f"未知日志类型：{kind}")
        self.kind = kind
        BUILD_LOG_PATH.mkdir(parents=True, exist_ok=True)
        self.path = BUILD_LOG_PATH / f"dm-{kind}-{_timestamp()}.log"
        self._fh = open(self.path, "w", encoding="utf-8", newline="\n")

    def write(self, line: str) -> None:
        """写入一行并 flush，保证子进程重定向场景下立刻可见。"""
        self._fh.write(line)
        self._fh.flush()

    def read_text(self) -> str:
        """读取当前已落盘的全部内容。"""
        if not self._fh.closed:
            self._fh.flush()
        return self.path.read_text(encoding="utf-8")

    def tail(self, n: int) -> list[str]:
        """返回最后 n 行；n ≤ 0 时返回全部行。"""
        lines = self.read_text().splitlines()
        return lines[-n:] if n > 0 else lines

    @property
    def file_handle(self):
        """重定向模式下供 subprocess 当 stdout 用。"""
        return self._fh

    def close_and_finalize(self) -> None:
        """关闭句柄并把当前文件复制为 dm-{kind}-latest.log 镜像。"""
        try:
            self._fh.flush()
            self._fh.close()
        finally:
            shutil.copy2(self.path, latest_log_path(self.kind))


# ── 编译日志摘要解析 ──────────────────────────────────────────────────────
_BOOTLOADER_RE = re.compile(
    r"Bootloader binary size (0x[0-9a-f]+) bytes\. (0x[0-9a-f]+) bytes \(([0-9]+)%\) free"
)
_APP_RE = re.compile(
    r"PhotoPainter_Device\.bin binary size (0x[0-9a-f]+) bytes\. "
    r"Smallest app partition is (0x[0-9a-f]+) bytes\. "
    r"(0x[0-9a-f]+) bytes \(([0-9]+)%\) free"
)


def extract_build_summary(log_text: str) -> dict:
    """从编译日志解析 bootloader / app 分区占用。

    匹配多次时取最后一次（等价原 dm.ps1 行为）。返回
    {"bootloader": (size, free, pct) | None,
     "app": (size, smallest, free, pct) | None}
    """
    boot = None
    app = None
    m = list(_BOOTLOADER_RE.finditer(log_text))
    if m:
        g = m[-1].groups()
        boot = (g[0], g[1], g[2])
    m = list(_APP_RE.finditer(log_text))
    if m:
        g = m[-1].groups()
        app = (g[0], g[1], g[2], g[3])
    return {"bootloader": boot, "app": app}


_FAILURE_RE = re.compile(
    r"FAILED:|fatal error:|error:|ninja: build stopped|CMake Error", re.IGNORECASE
)


def extract_failure_summary(log_text: str) -> list[str]:
    """匹配编译失败关键字行，最多 20 行。

    关键字：FAILED: / fatal error: / error: / ninja: build stopped / CMake Error。
    """
    hits = [line for line in log_text.splitlines() if _FAILURE_RE.search(line)]
    return hits[:20]


def _write_log_excerpt(line: str, max_len: int = 240) -> None:
    """输出限制长度后的单行日志，避免超长 Ninja 命令刷屏。"""
    if len(line) <= max_len:
        print(line)
    else:
        print(line[:max_len] + "……（该行已截断，全文见日志文件）")


def _show_build_failure(runlog) -> None:
    """编译失败时输出错误摘要与日志尾部，便于定位根因。"""
    print("\n错误摘要：")
    hits = extract_failure_summary(runlog.read_text())
    if hits:
        for line in hits:
            _write_log_excerpt(line)
    else:
        print("未匹配到标准错误关键字，请查看日志尾部。")
    print("\n日志尾部（最多 40 行）：")
    for line in runlog.tail(40):
        _write_log_excerpt(line)


# ── OTA：image-info 解析 / manifest 生成 / 原子发布 ────────────────────────
_VALIDATION_HASH_RE = re.compile(
    r"^\s*Validation hash:\s*([0-9a-f]{64})\s*\(valid\)\s*$", re.IGNORECASE | re.MULTILINE
)
_APP_VERSION_RE = re.compile(r"^\s*App version:\s*(.+?)\s*$", re.IGNORECASE | re.MULTILINE)


def parse_image_info(text: str) -> tuple[str, str]:
    """从 esptool image-info 输出解析 (artifact_id, version)。失败 fail()。

    artifact_id 取 Validation hash 的 64 位十六进制（小写）；version 取 App version 字段。
    任一字段缺失即视为致命错误，等价 ps1 的 throw。
    """
    h = _VALIDATION_HASH_RE.search(text)
    v = _APP_VERSION_RE.search(text)
    if not h or not v:
        fail("无法从 esptool image-info 输出解析 Validation hash 或 App version")
    return h.group(1).lower(), v.group(1).strip()


def build_manifest(version: str, ota_version: int, artifact_id: str,
                   file_sha256: str, size: int, filename: str) -> dict:
    """构造运行时清单字典。

    @param[in] version        App version（来自 image-info）
    @param[in] ota_version    单调递增的 OTA 版本（来自本地状态文件）
    @param[in] artifact_id    Validation hash（64 位十六进制），作为固件唯一标识
    @param[in] file_sha256    完整固件文件的 SHA-256 摘要
    @param[in] size           固件字节数
    @param[in] filename       发布到服务端的固件文件名
    @return 满足 OTA 协议的 manifest 字典
    """
    return {
        "protocol_version": 1,
        "artifacts": {
            "app": {
                "version": version,
                "ota_version": ota_version,
                "artifact_id": artifact_id,
                "file_sha256": file_sha256,
                "size": size,
                "filename": filename,
            }
        },
    }


def _file_sha256(path: Path) -> str:
    """分块读取文件计算 SHA-256，避免一次性读入大固件占用内存。"""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def publish_firmware(firmware_path: Path, service_root: Path, version: str,
                     ota_version: int, artifact_id: str, file_sha256: str,
                     size: int, filename: str) -> Path:
    """原子发布固件与 manifest.json 到 service_root/firmwares/。

    @param[in] firmware_path  本地刚编译出的固件路径
    @param[in] service_root   DeskSuite Hub 根目录（须含 app/main.py）
    @param[in] version        App version
    @param[in] ota_version    OTA 单调递增版本
    @param[in] artifact_id    Validation hash，作为固件唯一标识
    @param[in] file_sha256    完整固件文件 SHA-256
    @param[in] size           固件字节数
    @param[in] filename       发布文件名（firmware-<artifact_id>.bin）
    @return 发布后的固件路径

    契约：
    - 校验 service_root/app/main.py 存在，缺失即 fail；
    - 路径越界检查：firmwares 须在 service_root 下；
    - 同 artifact_id 已存在：sha256 相同则跳过复制（去重），不同则 fail；
    - manifest.json 用临时文件 + os.replace 原子替换，崩溃不会留下半截文件。
    """
    if not (service_root / "app" / "main.py").exists():
        fail(f"服务端仓库不存在或结构无效：{service_root}")

    firmware_dir = (service_root / "firmwares").resolve()
    service_prefix = str(service_root.resolve()).rstrip("\\") + "\\"
    if not str(firmware_dir).startswith(service_prefix):
        fail(f"固件发布目录越界：{firmware_dir}")
    firmware_dir.mkdir(parents=True, exist_ok=True)

    published = firmware_dir / filename
    manifest_path = firmware_dir / "manifest.json"
    manifest_temp = firmware_dir / f".manifest.{artifact_id}.tmp"

    try:
        if published.exists():
            existing = _file_sha256(published)
            if existing != file_sha256:
                fail("同一 artifact_id 已存在但完整文件摘要不同，已拒绝覆盖")
        else:
            tmp = firmware_dir / f".{filename}.tmp"
            shutil.copy2(firmware_path, tmp)
            os.replace(tmp, published)

        manifest = build_manifest(version, ota_version, artifact_id, file_sha256, size, filename)
        manifest_temp.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(manifest_temp, manifest_path)
    finally:
        if manifest_temp.exists():
            manifest_temp.unlink()

    return published


def _resolve_service_root(service_root_arg: str | None) -> Path:
    """解析服务端目录：显式指定则用之，否则取 DeskSuite 的 services/hub。"""
    if service_root_arg:
        return Path(service_root_arg).resolve()
    return (REPO_ROOT.parents[1] / "services" / "hub").resolve()


def _read_existing_ota_version(service_root: Path) -> int:
    """读服务端现有 manifest 的 ota_version，返回 minimum = ota_version + 1；无则 1。

    服务端 manifest 缺失视为首次发布；已达 UINT64_MAX 时 fail。
    """
    manifest = service_root / "firmwares" / "manifest.json"
    if not manifest.exists():
        return 1
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
        existing = data.get("artifacts", {}).get("app", {}).get("ota_version")
    except (json.JSONDecodeError, OSError) as exc:
        fail(f"无法读取服务端现有 OTA 版本：{exc}")
    if existing is None:
        return 1
    existing = int(existing)
    if existing == UINT64_MAX:
        fail("服务端 OTA 版本已达到上限")
    return existing + 1


def parse_font_offset(partitions_text: str) -> str | None:
    """从 partitions.csv 文本解析 font 分区偏移；无则 None。"""
    for line in partitions_text.splitlines():
        if re.match(r"^\s*font\s*,", line):
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 4 and parts[3]:
                return parts[3]
    return None


def find_font_bin() -> Path | None:
    """查找最新生成的 font.bin。"""
    roots = [
        Path(__file__).resolve().parent / "tools" / "fonts2bin",
        PROJECT_ROOT / "tools" / "fonts2bin",
    ]
    for root in roots:
        if not root.exists():
            continue
        matches = sorted(root.rglob("font.bin"), key=lambda p: p.stat().st_mtime)
        if matches:
            return matches[-1]
    return None


def stop_com_port(port: str) -> None:
    """终止占用指定串口的 idf_monitor / esp_idf_monitor 进程。"""
    write_step(f"释放 {port} 端口")
    script = (
        "$procs = Get-CimInstance Win32_Process | Where-Object { "
        f"($_.CommandLine -match 'idf_monitor|esp_idf_monitor') -and ($_.CommandLine -match '{port}') "
        "}; foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue; "
        "Write-Host (\"  已终止监控进程：\" + $p.Name + \"（PID：\" + $p.ProcessId + \"）\") }"
    )
    run_powershell(script)
    print(f"✅ {port} 端口清理完成")


def write_step(msg: str) -> None:
    """输出中文步骤标题。"""
    bar = "═" * 59
    print(f"\n{bar}\n  {msg}\n{bar}\n")


def fail(msg: str) -> None:
    """输出中文错误并以失败状态结束。"""
    print(f"\n❌ {msg}")
    sys.exit(1)


def run_powershell(
    script: str,
    *,
    stdout=None,
    stderr=None,
    check: bool = False,
) -> subprocess.CompletedProcess:
    """执行 PowerShell 子进程。

    stdout/stderr: None=透传终端；文件对象=重定向；subprocess.PIPE=捕获。
    check=True 时非零退出抛 CalledProcessError。

    固定以 UTF-8 解码子进程输出：Windows PowerShell 在 PIPE 模式下输出
    UTF-8 字节，subprocess.run(text=True) 默认用 locale（中文系统为 GBK）
    解码会失败。显式指定 encoding 避免 UnicodeDecodeError。

    errors="replace"：子进程 stderr 或合并 stdout 仍可能混入非 UTF-8 字节
    （中文 Windows 下 esptool/PowerShell 管道偶发 GBK 字节），默认 "strict"
    会让 subprocess 内部读线程抛 UnicodeDecodeError，导致 stdout=None 下游
    崩溃。replace 用 U+FFFD 替代非法字节，保证调用方始终拿到字符串。
    """
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script]
    return subprocess.run(
        cmd,
        stdout=stdout,
        stderr=stderr,
        check=check,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


# ── 命令实现（Task 2-7 逐步填充，此处先 stub）─────────────────────────────
def cmd_build(args: argparse.Namespace) -> int:
    write_step("编译项目")
    init_project_build_cache()
    new_build_ota_version(OTA_VERSION_STATE_PATH, OTA_VERSION_HEADER_PATH, minimum=1)
    check_fixed_environment()

    runlog = RunLog("build")
    print(f"📝 完整编译日志：{runlog.path}")
    print("⏳ 正在编译，终端仅显示结果摘要……")

    rc = run_ninja(stdout=runlog.file_handle)
    runlog.close_and_finalize()

    if getattr(args, "full_log", False):
        print("\n完整编译日志：")
        print(runlog.read_text())

    if rc != 0:
        _show_build_failure(runlog)
        fail(f"编译失败，完整日志已保留：{runlog.path}")

    summary = extract_build_summary(runlog.read_text())
    if summary["bootloader"] or summary["app"]:
        print("\n构建摘要：")
        if summary["bootloader"]:
            b = summary["bootloader"]
            print(f"引导程序大小：{b[0]} 字节，剩余 {b[1]} 字节（{b[2]}%）")
        if summary["app"]:
            a = summary["app"]
            print(
                f"应用固件大小：{a[0]} 字节，最小应用分区 {a[1]} 字节，剩余 {a[2]} 字节（{a[3]}%）"
            )
    print("\n✅ 编译成功")
    return 0


def _show_log(kind: str, full_log: bool, tail_lines: int) -> int:
    """查看 dm-<kind>-*.log 最新一份（build-log/flash-log/monitor-log 共用）。"""
    latest = find_latest_log(kind)
    if latest is None:
        fail(f"尚无{kind}日志，请先运行 {kind} 命令")
    print(f"📝 最近{kind}日志：{latest}")
    text = latest.read_text(encoding="utf-8")
    if full_log:
        print(text)
    else:
        for line in text.splitlines()[-tail_lines:]:
            _write_log_excerpt(line)
    return 0


def cmd_build_log(args: argparse.Namespace) -> int:
    return _show_log("build", args.full_log, args.tail_lines)


def cmd_flash_log(args: argparse.Namespace) -> int:
    return _show_log("flash", args.full_log, args.tail_lines)


def cmd_monitor_log(args: argparse.Namespace) -> int:
    return _show_log("monitor", args.full_log, args.tail_lines)


def cmd_flash(args: argparse.Namespace) -> int:
    write_step(f"烧录固件到 {args.port}")
    init_project_build_cache()
    check_fixed_environment()
    stop_com_port(args.port)
    runlog = RunLog("flash")
    print(f"📝 完整烧录日志：{runlog.path}")
    rc = run_idf_command(
        ["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "-p", args.port, "flash"],
        stdout=runlog.file_handle,
    )
    runlog.close_and_finalize()
    if rc != 0:
        fail(f"烧录失败，完整日志已保留：{runlog.path}")
    print("\n✅ 烧录成功")
    return 0


def cmd_flash_font(args: argparse.Namespace) -> int:
    font_bin = find_font_bin()
    if not font_bin:
        fail("字库文件不存在，请先生成 font.bin")
    partitions = PROJECT_ROOT / "partitions.csv"
    offset = parse_font_offset(partitions.read_text(encoding="utf-8")) if partitions.exists() else None
    if not offset:
        fail("无法从 partitions.csv 解析 font 分区偏移")
    write_step(f"烧录字库到 font 分区（{offset}）")
    check_fixed_environment()
    stop_com_port(args.port)
    rc, _ = run_esptool(
        ["--chip", "esp32s3", "--port", args.port, "--baud", "921600",
         "write_flash", offset, str(font_bin)]
    )
    if rc != 0:
        fail("字库烧录失败")
    print("\n✅ 字库烧录成功")
    return 0


def cmd_kill_port(args: argparse.Namespace) -> int:
    stop_com_port(args.port)
    return 0


def cmd_clean(args: argparse.Namespace) -> int:
    write_step("清理构建目录")
    resolved = BUILD_PATH.resolve()
    expected_prefix = PROJECT_ROOT.resolve()
    if resolved.parent != expected_prefix and not str(resolved).startswith(
        str(expected_prefix) + "\\"
    ):
        fail(f"构建目录不在项目内，已拒绝删除：{resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)
        print("✅ build 目录已删除")
    else:
        print("⚠️ build 目录不存在")
    return 0


def cmd_menuconfig(args: argparse.Namespace) -> int:
    write_step("打开菜单配置")
    check_fixed_environment()
    return run_idf_command(["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "menuconfig"])


def _monitor_tee(port: str) -> int:
    """monitor 的实际 tee 循环：子进程 stdout 逐行写 RunLog + 显示终端。

    Ctrl+C（KeyboardInterrupt）正常退出。返回 idf.py monitor 退出码。
    """
    runlog = RunLog("monitor")
    print(f"📝 监控日志：{runlog.path}")
    script = _idf_prefix() + "& idf.py " + " ".join(
        _ps_quote(a) for a in ["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "-p", port, "monitor"]
    )
    # 用 Popen 拿管道，逐行 tee
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            runlog.write(line)
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        print("\n（监控已中断）")
    finally:
        runlog.close_and_finalize()
    return proc.returncode if proc.returncode is not None else 0


def cmd__monitor_run(args: argparse.Namespace) -> int:
    """内部子命令：--detach 后台进程入口，跑 _monitor_tee。"""
    check_fixed_environment()
    return _monitor_tee(args.port)


def cmd_monitor(args: argparse.Namespace) -> int:
    write_step(f"监控串口 {args.port}")
    check_fixed_environment()
    stop_com_port(args.port)
    if getattr(args, "detach", False):
        return _launch_monitor_detached(args.port)
    return _monitor_tee(args.port)


def cmd_flash_monitor(args: argparse.Namespace) -> int:
    write_step(f"烧录并监控 {args.port}")
    init_project_build_cache()
    check_fixed_environment()
    stop_com_port(args.port)
    runlog = RunLog("flash")
    print(f"📝 完整烧录日志：{runlog.path}")
    rc = run_idf_command(
        ["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "-p", args.port, "flash"],
        stdout=runlog.file_handle,
    )
    runlog.close_and_finalize()
    if rc != 0:
        fail(f"烧录失败，完整日志已保留：{runlog.path}")
    print("\n✅ 烧录成功，进入监控……")
    return _monitor_tee(args.port)


def _launch_monitor_detached(port: str) -> int:
    """后台启动一个独立 Python 进程跑 _monitor_run，立即返回。"""
    DETACHED_PROCESS = 0x00000008
    CREATE_NO_WINDOW = 0x08000000
    cmd = [
        str(EXPECTED_PYTHON_PATH),
        str(Path(__file__).resolve()),
        "_monitor_run",
        "--port",
        port,
    ]
    # 后台进程的 stdout/stderr 丢弃（它自己写 dm-monitor-latest.log）
    subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW,
        close_fds=True,
    )
    print(f"监控已在后台运行，日志：{latest_log_path('monitor')}")
    print(f"用 monitor-log 查看，用 kill-port -p {port}（或 dm.py kill-port）停止。")
    return 0


def cmd_ota(args: argparse.Namespace) -> int:
    service_root = _resolve_service_root(getattr(args, "service_root", None))
    if not (service_root / "app" / "main.py").exists():
        fail(f"服务端仓库不存在或结构无效：{service_root}")

    minimum = _read_existing_ota_version(service_root)

    # 编译（用指定最小版本生成 OTA 版本头）
    write_step("编译项目")
    init_project_build_cache()
    new_build_ota_version(OTA_VERSION_STATE_PATH, OTA_VERSION_HEADER_PATH, minimum=minimum)
    check_fixed_environment()
    runlog = RunLog("build")
    print(f"📝 完整编译日志：{runlog.path}")
    rc = run_ninja(stdout=runlog.file_handle)
    runlog.close_and_finalize()
    if getattr(args, "full_log", False):
        print(runlog.read_text())
    if rc != 0:
        _show_build_failure(runlog)
        fail(f"编译失败，完整日志已保留：{runlog.path}")

    firmware_path = BUILD_PATH / "PhotoPainter_Device.bin"
    if not firmware_path.exists():
        fail(f"未找到 OTA 固件：{firmware_path}")

    write_step("解析固件身份并发布到服务端")
    rc, info_text = run_esptool(["image-info", str(firmware_path)])
    if rc != 0:
        fail("esptool 无法读取固件身份")
    artifact_id, version = parse_image_info(info_text)
    file_sha256 = _file_sha256(firmware_path)
    size = firmware_path.stat().st_size
    filename = f"firmware-{artifact_id}.bin"

    published = publish_firmware(
        firmware_path, service_root, version,
        int((OTA_VERSION_STATE_PATH.read_text(encoding="utf-8").strip())),
        artifact_id, file_sha256, size, filename,
    )

    print(f"固件版本：{version}")
    print(f"OTA 版本：{OTA_VERSION_STATE_PATH.read_text(encoding='utf-8').strip()}")
    print(f"artifact_id：{artifact_id}")
    print(f"file_sha256：{file_sha256}")
    print(f"发布文件：{published}")
    print("✅ OTA 固件与运行时清单已原子发布")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dm.py",
        description="PhotoPainter_Device 构建工具",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # build
    p = sub.add_parser("build", help="使用固定环境编译，完整日志自动落盘")
    p.add_argument("--full-log", action="store_true", help="编译结束后输出完整日志")
    p.set_defaults(func=cmd_build)

    # flash
    p = sub.add_parser("flash", help="烧录固件")
    p.add_argument("--port", default=DEFAULT_PORT, help="串口（默认 COM5）")
    p.add_argument("--full-log", action="store_true")
    p.set_defaults(func=cmd_flash)

    # flash-font
    p = sub.add_parser("flash-font", help="烧录字库分区")
    p.add_argument("--port", default=DEFAULT_PORT)
    p.set_defaults(func=cmd_flash_font)

    # monitor
    p = sub.add_parser("monitor", help="监控串口")
    p.add_argument("--port", default=DEFAULT_PORT)
    p.add_argument("--detach", action="store_true", help="后台运行，日志写 dm-monitor-latest.log")
    p.set_defaults(func=cmd_monitor)

    # flash-monitor
    p = sub.add_parser("flash-monitor", help="烧录并监控")
    p.add_argument("--port", default=DEFAULT_PORT)
    p.set_defaults(func=cmd_flash_monitor)

    # kill-port
    p = sub.add_parser("kill-port", help="释放指定串口")
    p.add_argument("--port", default=DEFAULT_PORT)
    p.set_defaults(func=cmd_kill_port)

    # clean
    p = sub.add_parser("clean", help="清理 build 目录")
    p.set_defaults(func=cmd_clean)

    # menuconfig
    p = sub.add_parser("menuconfig", help="打开菜单配置")
    p.set_defaults(func=cmd_menuconfig)

    # ota
    p = sub.add_parser("ota", help="编译并向服务端原子发布固件与运行时清单")
    p.add_argument("--service-root", default=None, help="服务端目录（默认 DeskSuite/services/hub）")
    p.add_argument("--full-log", action="store_true")
    p.set_defaults(func=cmd_ota)

    # build-log / flash-log / monitor-log
    log_specs = [("build-log", cmd_build_log), ("flash-log", cmd_flash_log), ("monitor-log", cmd_monitor_log)]
    for name, func in log_specs:
        p = sub.add_parser(name, help=f"查看最近{name.replace('-', '')}日志")
        p.add_argument("--full-log", action="store_true")
        p.add_argument("--tail-lines", type=int, default=40)
        p.set_defaults(func=func)

    # 注意：内部子命令 _monitor_run 不在此注册——Python 3.14 下
    # argparse.SUPPRESS 字面量会被渲染进 dm.py --help，违反"内部命令不暴露"。
    # main() 中手动短路处理 `dm.py _monitor_run --port <port>`。
    return parser


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[1:]
    # 内部子命令短路：不注册到主 subparsers，避免 `dm.py --help` 暴露。
    # 仅供 monitor --detach 派生的后台进程调用。
    if len(argv) >= 1 and argv[0] == "_monitor_run":
        p = argparse.ArgumentParser(prog="dm.py _monitor_run")
        p.add_argument("--port", default=DEFAULT_PORT)
        return cmd__monitor_run(p.parse_args(argv[1:]))
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
