#!/usr/bin/env python3
"""DeskSuite 统一设备构建与 OTA 发布工具。

所有命令都显式选择产品，设备目录不再拥有自己的构建或 OTA 发布脚本。
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
import tempfile
import time
import tomllib
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath

# ── 固定编译环境（DeskSuite 仓库硬约束）──────────────────────────────────
EXPECTED_IDF_PATH = Path(r"C:\esp\v6.0.1\esp-idf")
EXPECTED_PYTHON_PATH = Path(
    r"C:\Users\lbq08\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe"
)
EXPECTED_NINJA_PATH = Path(
    r"C:\Users\lbq08\.espressif\tools\ninja\1.12.1\ninja.exe"
)
EXPECTED_NINJA_VERSION = "1.12.1"

# ── DeskSuite 与产品配置 ─────────────────────────────────────────────────
SUITE_ROOT = Path(__file__).resolve().parent.parent
PRODUCTS_PATH = SUITE_ROOT / "products.toml"
DEFAULT_SERVICE_ROOT = SUITE_ROOT / "services" / "hub"
OTA_STATE_ROOT = SUITE_ROOT / ".build-state" / "ota"


@dataclass(frozen=True)
class ProductConfig:
    """单个设备产品的构建与发布配置。"""

    name: str
    product_id: int
    firmware_target: str
    project_dir: Path
    firmware_file: str
    default_port: str


@dataclass(frozen=True)
class OtaPublishConfig:
    """生产 Hub 的非秘密 OTA 发布配置。"""

    mode: str
    ssh_host: str
    remote_service_root: str
    container_name: str
    container_firmware_root: str
    runtime_uid: int
    runtime_gid: int


class OtaPublishError(RuntimeError):
    """远程 OTA 发布前置条件、传输或复核失败。"""


_PRODUCT_NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
_FIRMWARE_TARGET_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_REMOTE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")
_REMOTE_PATH_RE = re.compile(r"^/[A-Za-z0-9._/-]+$")


def _validated_remote_path(raw: object, field: str) -> str:
    """返回不含 shell 元字符或路径回退的规范 POSIX 绝对路径。"""
    if not isinstance(raw, str) or _REMOTE_PATH_RE.fullmatch(raw) is None:
        fail(f"OTA 发布配置 {field} 必须是安全的 POSIX 绝对路径")
    parsed = PurePosixPath(raw)
    if raw == "/" or str(parsed) != raw or any(part in (".", "..") for part in parsed.parts):
        fail(f"OTA 发布配置 {field} 不是规范路径")
    return raw


def load_ota_publish_config(path: Path = PRODUCTS_PATH) -> OtaPublishConfig:
    """读取并严格校验受版本控制的远端 OTA 发布配置。"""
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    raw = data.get("ota_publish")
    if not isinstance(raw, dict):
        fail(f"产品配置缺少 [ota_publish]：{path}")
    try:
        mode = raw["mode"]
        ssh_host = raw["ssh_host"]
        remote_service_root = raw["remote_service_root"]
        container_name = raw["container_name"]
        container_firmware_root = raw["container_firmware_root"]
        runtime_uid = raw["runtime_uid"]
        runtime_gid = raw["runtime_gid"]
    except KeyError as exc:
        fail(f"OTA 发布配置缺少字段：{exc.args[0]}")
    if mode != "ssh_docker":
        fail("OTA 发布配置 mode 仅支持 ssh_docker")
    if not isinstance(ssh_host, str) or _REMOTE_NAME_RE.fullmatch(ssh_host) is None:
        fail("OTA 发布配置 ssh_host 非法")
    if not isinstance(container_name, str) or _REMOTE_NAME_RE.fullmatch(container_name) is None:
        fail("OTA 发布配置 container_name 非法")
    if (
        isinstance(runtime_uid, bool)
        or not isinstance(runtime_uid, int)
        or runtime_uid < 0
        or isinstance(runtime_gid, bool)
        or not isinstance(runtime_gid, int)
        or runtime_gid < 0
    ):
        fail("OTA 发布配置 runtime_uid/runtime_gid 必须是非负整数")
    return OtaPublishConfig(
        mode=mode,
        ssh_host=ssh_host,
        remote_service_root=_validated_remote_path(
            remote_service_root, "remote_service_root"
        ),
        container_name=container_name,
        container_firmware_root=_validated_remote_path(
            container_firmware_root, "container_firmware_root"
        ),
        runtime_uid=runtime_uid,
        runtime_gid=runtime_gid,
    )


def load_products(path: Path = PRODUCTS_PATH) -> dict[str, ProductConfig]:
    """读取并严格校验受版本控制的产品构建配置。"""
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    raw_products = data.get("products")
    if not isinstance(raw_products, dict) or not raw_products:
        fail(f"产品配置缺少 [products]：{path}")

    products: dict[str, ProductConfig] = {}
    product_ids: set[int] = set()
    firmware_targets: set[str] = set()
    suite_root = SUITE_ROOT.resolve()
    for name, raw in raw_products.items():
        if not isinstance(name, str) or _PRODUCT_NAME_RE.fullmatch(name) is None:
            fail(f"产品名称非法：{name}")
        if not isinstance(raw, dict):
            fail(f"产品配置必须是 TOML 表：{name}")
        try:
            product_id = int(raw["product_id"])
            firmware_target = str(raw["firmware_target"])
            project_dir = (SUITE_ROOT / str(raw["project_dir"])).resolve()
            firmware_file = str(raw["firmware_file"])
            default_port = str(raw["default_port"])
        except (KeyError, TypeError, ValueError) as exc:
            fail(f"产品配置字段无效：{name}: {exc}")
        if product_id <= 0 or product_id in product_ids:
            fail(f"product_id 必须为唯一正整数：{name}")
        if _FIRMWARE_TARGET_RE.fullmatch(firmware_target) is None or firmware_target in firmware_targets:
            fail(f"firmware_target 非法或重复：{name}")
        if project_dir.parent != (suite_root / "devices") or not (project_dir / "CMakeLists.txt").exists():
            fail(f"产品工程目录无效或越界：{project_dir}")
        if Path(firmware_file).name != firmware_file or not firmware_file.endswith(".bin"):
            fail(f"固件文件名非法：{name}")
        if not default_port:
            fail(f"默认串口不能为空：{name}")
        product_ids.add(product_id)
        firmware_targets.add(firmware_target)
        products[name] = ProductConfig(
            name=name,
            product_id=product_id,
            firmware_target=firmware_target,
            project_dir=project_dir,
            firmware_file=firmware_file,
            default_port=default_port,
        )
    return products


PRODUCTS: dict[str, ProductConfig] = {}
PRODUCT: ProductConfig
PROJECT_ROOT: Path
BUILD_PATH: Path
BUILD_LOG_PATH: Path
OTA_VERSION_STATE_PATH: Path
OTA_VERSION_HEADER_PATH: Path


def select_product(product: ProductConfig) -> None:
    """选择本次命令的产品，并更新只供命令实现使用的派生路径。"""
    global PRODUCT, PROJECT_ROOT, BUILD_PATH, BUILD_LOG_PATH
    global OTA_VERSION_STATE_PATH, OTA_VERSION_HEADER_PATH
    PRODUCT = product
    PROJECT_ROOT = product.project_dir
    BUILD_PATH = PROJECT_ROOT / "build"
    BUILD_LOG_PATH = BUILD_PATH / "logs"
    OTA_VERSION_STATE_PATH = OTA_STATE_ROOT / f"{product.firmware_target}.version"
    OTA_VERSION_HEADER_PATH = BUILD_PATH / "generated" / "firmware_ota_build.h"

MAX_SAFE_JSON_INTEGER = 9_007_199_254_740_991


def _now_ms() -> int:
    """当前 UTC 毫秒时间戳。测试可 monkeypatch。"""
    return int(time.time() * 1000)


@contextmanager
def _ota_state_lock(state_path: Path):
    """跨进程独占单个固件目标的版本状态，异常退出后允许清理陈旧锁。"""
    state_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path = state_path.with_name(f"{state_path.name}.lock")
    deadline = time.monotonic() + 30.0
    descriptor: int | None = None
    while descriptor is None:
        try:
            descriptor = os.open(
                lock_path,
                os.O_CREAT | os.O_EXCL | os.O_WRONLY,
            )
            os.write(descriptor, f"{os.getpid()}\n".encode("ascii"))
        except FileExistsError:
            try:
                if time.time() - lock_path.stat().st_mtime > 600:
                    lock_path.unlink()
                    continue
            except FileNotFoundError:
                continue
            if time.monotonic() >= deadline:
                fail(f"等待 OTA 版本状态锁超时：{lock_path}")
            time.sleep(0.05)
    try:
        yield
    finally:
        os.close(descriptor)
        try:
            lock_path.unlink()
        except FileNotFoundError:
            pass


def _atomic_write_text(path: Path, content: str) -> None:
    """在同目录写临时文件后原子替换文本文件。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(content, encoding="utf-8")
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def new_build_ota_version(
    state_path: Path,
    header_path: Path,
    product: ProductConfig,
    minimum: int = 1,
) -> int:
    """生成单调递增的 OTA 版本，并写入产品身份构建头。

    候选 = 当前 UTC 毫秒；若 ≤ 已记录版本则取 记录值+1；最终不低于 minimum。
    状态文件或头文件写入失败、版本达上限、状态文件损坏时 fail()。
    """
    with _ota_state_lock(state_path):
        candidate = _now_ms()
        if state_path.exists():
            text = state_path.read_text(encoding="utf-8").strip()
            try:
                last = int(text)
            except ValueError:
                fail(f"本地 OTA 版本状态无效：{state_path}")
            if candidate <= last:
                if last >= MAX_SAFE_JSON_INTEGER:
                    fail("本地 OTA 版本已达到 JSON 安全整数上限")
                candidate = last + 1
        if candidate < minimum:
            candidate = minimum
        if candidate > MAX_SAFE_JSON_INTEGER:
            fail("OTA 版本超过 JSON 安全整数上限")

        header = (
            "/** @file firmware_ota_build.h @brief 由 DeskSuite 构建工具生成的产品与 OTA 身份。 */\n"
            "#pragma once\n\n"
            "#include <stdint.h>\n\n"
            f"#define DESKSUITE_PRODUCT_ID UINT32_C({product.product_id})\n"
            f'#define DESKSUITE_FIRMWARE_TARGET "{product.firmware_target}"\n'
            f"#define FIRMWARE_OTA_BUILD_VERSION UINT64_C({candidate})\n"
        )
        _atomic_write_text(state_path, f"{candidate}\n")
        _atomic_write_text(header_path, header)
    print(f"OTA 版本：{candidate}")
    return candidate


def check_fixed_environment() -> None:
    """校验现有 CMake 构建目录及其固定工具绑定，不一致时 fail()。

    固定工具链版本和路径，避免不同设备工程隐式选择不同环境。
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
    """首次构建时生成固定环境的 CMake 缓存。"""
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

    构建工具运行时可用 datetime.now()，不受 workflow 脚本限制。
    """
    now = datetime.now()
    return now.strftime("%Y%m%d-%H%M%S-") + f"{now.microsecond // 1000:03d}"


def latest_log_path(kind: str) -> Path:
    """给定日志类型，返回对应的 ds-{kind}-latest.log 路径。"""
    return BUILD_LOG_PATH / f"ds-{kind}-latest.log"


def find_latest_log(kind: str) -> Path | None:
    """返回 ds-{kind}-*.log 中修改时间最新的一个；无则 None。

    排除 ds-{kind}-latest.log 镜像自身。
    """
    matches = sorted(
        BUILD_LOG_PATH.glob(f"ds-{kind}-*.log"),
        key=lambda p: p.stat().st_mtime,
    )
    matches = [p for p in matches if not p.name.endswith("-latest.log")]
    return matches[-1] if matches else None


class RunLog:
    """日志落盘器：带时间戳新文件 + -latest 镜像。

    kind ∈ "build" | "flash" | "monitor"。__init__ 即在 BUILD_LOG_PATH 下创建
    ds-{kind}-<timestamp>.log；close_and_finalize() 关句柄并 shutil.copy2 到
    ds-{kind}-latest.log 供 build-log / flash-log / monitor-log 查看。
    """

    def __init__(self, kind: str):
        if kind not in ("build", "flash", "monitor"):
            fail(f"未知日志类型：{kind}")
        self.kind = kind
        BUILD_LOG_PATH.mkdir(parents=True, exist_ok=True)
        self.path = BUILD_LOG_PATH / f"ds-{kind}-{_timestamp()}.log"
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
        """关闭句柄并把当前文件复制为 ds-{kind}-latest.log 镜像。"""
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
    r"[A-Za-z0-9_.-]+\.bin binary size (0x[0-9a-f]+) bytes\. "
    r"Smallest app partition is (0x[0-9a-f]+) bytes\. "
    r"(0x[0-9a-f]+) bytes \(([0-9]+)%\) free"
)


def extract_build_summary(log_text: str) -> dict:
    """从编译日志解析 bootloader / app 分区占用。

    匹配多次时取最后一次。返回
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


def build_manifest(
    product: ProductConfig,
    version: str,
    ota_version: int,
    artifact_id: str,
    file_sha256: str,
    size: int,
) -> dict:
    """构造运行时清单字典。

    @param[in] product        产品与固件目标配置
    @param[in] version        App version（来自 image-info）
    @param[in] ota_version    单调递增的 OTA 版本（来自本地状态文件）
    @param[in] artifact_id    Validation hash（64 位十六进制），作为固件唯一标识
    @param[in] file_sha256    完整固件文件的 SHA-256 摘要
    @param[in] size           固件字节数
    @return 满足 OTA 协议的 manifest 字典
    """
    return {
        "protocol_version": 2,
        "product_id": product.product_id,
        "firmware_target": product.firmware_target,
        "artifacts": {
            "app": {
                "version": version,
                "ota_version": ota_version,
                "artifact_id": artifact_id,
                "file_sha256": file_sha256,
                "size": size,
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


def publish_firmware(
    firmware_path: Path,
    service_root: Path,
    product: ProductConfig,
    version: str,
    ota_version: int,
    artifact_id: str,
    file_sha256: str,
    size: int,
) -> Path:
    """把固件发布到全局哈希制品库，并原子替换目标清单。

    @param[in] firmware_path  本地刚编译出的固件路径
    @param[in] service_root   DeskSuite Hub 根目录（须含 app/main.py）
    @param[in] product        产品与固件目标配置
    @param[in] version        App version
    @param[in] ota_version    OTA 单调递增版本
    @param[in] artifact_id    Validation hash，作为固件唯一标识
    @param[in] file_sha256    完整固件文件 SHA-256
    @param[in] size           固件字节数
    @return 发布后的固件路径

    契约：
    - 校验 service_root/app/main.py 存在，缺失即 fail；
    - 全局制品写入 firmwares/artifacts/<artifact_id>.bin；
    - 目标清单写入 firmwares/manifests/<firmware_target>.json；
    - 同 artifact_id 已存在：sha256 相同则跳过复制（去重），不同则 fail；
    - 清单用临时文件 + os.replace 原子替换，崩溃不会留下半截文件。
    """
    if not (service_root / "app" / "main.py").exists():
        fail(f"服务端仓库不存在或结构无效：{service_root}")
    if (
        product.product_id <= 0
        or _FIRMWARE_TARGET_RE.fullmatch(product.firmware_target) is None
    ):
        fail("产品身份或 firmware_target 无效")
    if re.fullmatch(r"[0-9a-f]{64}", artifact_id) is None:
        fail("artifact_id 必须是 64 位小写 SHA-256")
    if re.fullmatch(r"[0-9a-f]{64}", file_sha256) is None:
        fail("file_sha256 必须是 64 位小写 SHA-256")
    if (
        not firmware_path.is_file()
        or size <= 0
        or firmware_path.stat().st_size != size
        or _file_sha256(firmware_path) != file_sha256
    ):
        fail("待发布固件的大小或完整文件摘要与参数不一致")
    if (
        not version
        or len(version) > 64
        or ota_version <= 0
        or ota_version > MAX_SAFE_JSON_INTEGER
    ):
        fail("固件版本或 ota_version 无效")

    firmware_dir = (service_root / "firmwares").resolve()
    if firmware_dir.parent != service_root.resolve():
        fail(f"固件发布目录越界：{firmware_dir}")
    artifact_dir = firmware_dir / "artifacts"
    manifest_dir = firmware_dir / "manifests"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    manifest_dir.mkdir(parents=True, exist_ok=True)

    published = artifact_dir / f"{artifact_id}.bin"
    manifest_path = manifest_dir / f"{product.firmware_target}.json"
    process_id = os.getpid()
    manifest_temp = manifest_dir / (
        f".{product.firmware_target}.{artifact_id}.{process_id}.tmp"
    )
    artifact_temp = artifact_dir / f".{artifact_id}.{process_id}.tmp"

    try:
        with _ota_state_lock(manifest_path):
            minimum = _read_existing_ota_version(service_root, product)
            if ota_version < minimum:
                fail(
                    f"OTA 版本 {ota_version} 早于当前目标清单要求的最小值 {minimum}"
                )

            # 不同固件目标可以引用同一全局制品；按 artifact_id 串行校验与落盘，
            # 避免两个并发发布者在“文件尚不存在”的检查后互相覆盖。
            with _ota_state_lock(published):
                if published.exists():
                    existing = _file_sha256(published)
                    if existing != file_sha256:
                        fail("同一 artifact_id 已存在但完整文件摘要不同，已拒绝覆盖")
                else:
                    shutil.copy2(firmware_path, artifact_temp)
                    if _file_sha256(artifact_temp) != file_sha256:
                        fail("复制后的 OTA 制品摘要不一致")
                    os.replace(artifact_temp, published)

            manifest = build_manifest(
                product,
                version,
                ota_version,
                artifact_id,
                file_sha256,
                size,
            )
            manifest_temp.write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            os.replace(manifest_temp, manifest_path)
    finally:
        if manifest_temp.exists():
            manifest_temp.unlink()
        if artifact_temp.exists():
            artifact_temp.unlink()

    return published


def _resolve_service_root(service_root_arg: str | None) -> Path:
    """解析服务端目录：显式指定则用之，否则取 DeskSuite 的 services/hub。"""
    if service_root_arg:
        return Path(service_root_arg).resolve()
    return DEFAULT_SERVICE_ROOT.resolve()


def resolve_ota_publish_target(
    service_root_arg: str | None,
) -> tuple[str, Path | OtaPublishConfig]:
    """解析 OTA 目标；显式 ServiceRoot 保留本地发布，否则使用生产远端。"""
    if service_root_arg:
        service_root = Path(service_root_arg).resolve()
        if not (service_root / "app" / "main.py").exists():
            fail(f"服务端仓库不存在或结构无效：{service_root}")
        return "local", service_root
    return "remote", load_ota_publish_config()


def _read_existing_ota_version(service_root: Path, product: ProductConfig) -> int:
    """读目标现有清单的 ota_version，返回 minimum = ota_version + 1；无则 1。

    服务端 manifest 缺失视为首次发布；达到 JSON 安全整数上限时 fail。
    """
    manifest = (
        service_root
        / "firmwares"
        / "manifests"
        / f"{product.firmware_target}.json"
    )
    try:
        text = manifest.read_text(encoding="utf-8") if manifest.exists() else None
    except OSError as exc:
        fail(f"无法读取服务端现有 OTA 版本：{exc}")
    return ota_minimum_from_manifest_text(text, product)


def ota_minimum_from_manifest_text(
    text: str | None,
    product: ProductConfig,
) -> int:
    """校验现有 manifest 并返回下一次发布允许的最小 OTA 版本。"""
    if text is None:
        return 1
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        fail(f"服务端 OTA 清单 JSON 无效：{exc}")
    if not isinstance(data, dict) or (
        data.get("protocol_version") != 2
        or data.get("product_id") != product.product_id
        or data.get("firmware_target") != product.firmware_target
    ):
        fail("服务端 OTA 清单身份与产品配置不一致")
    artifacts = data.get("artifacts")
    app = artifacts.get("app") if isinstance(artifacts, dict) else None
    existing = app.get("ota_version") if isinstance(app, dict) else None
    if isinstance(existing, bool) or not isinstance(existing, int) or existing < 0:
        fail("服务端 OTA 清单的 ota_version 无效")
    if existing >= MAX_SAFE_JSON_INTEGER:
        fail("服务端 OTA 版本已达到 JSON 安全整数上限")
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
    """在所选产品工程中查找最新生成的 font.bin。"""
    root = PROJECT_ROOT / "tools" / "fonts2bin"
    if not root.exists():
        return None
    matches = sorted(root.rglob("font.bin"), key=lambda p: p.stat().st_mtime)
    return matches[-1] if matches else None


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


def run_external(
    argv: list[str] | tuple[str, ...],
    *,
    capture_output: bool = False,
    input_text: str | None = None,
) -> subprocess.CompletedProcess:
    """无 shell 执行 SSH/SCP 等外部命令，并统一使用 UTF-8 解码。"""
    return subprocess.run(
        [str(value) for value in argv],
        input=input_text,
        stdout=subprocess.PIPE if capture_output else None,
        stderr=subprocess.PIPE if capture_output else None,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


class RemoteOtaPublisher:
    """通过 SSH 与 Docker 把 OTA 发布到 Ubuntu 生产 Hub。"""

    def __init__(self, config: OtaPublishConfig, *, runner=run_external) -> None:
        self.config = config
        self.runner = runner

    def _run(
        self,
        argv: list[str] | tuple[str, ...],
        *,
        capture_output: bool = False,
        allow_failure: bool = False,
    ) -> subprocess.CompletedProcess:
        try:
            result = self.runner(
                argv,
                capture_output=capture_output,
                input_text=None,
            )
        except OSError as exc:
            raise OtaPublishError(f"无法启动远程发布命令：{argv[0]}: {exc}") from exc
        if result.returncode != 0 and not allow_failure:
            detail = (result.stderr or result.stdout or "").strip()
            suffix = f"：{detail[:500]}" if detail else ""
            raise OtaPublishError(
                f"远程发布命令失败（{argv[0]}，退出码 {result.returncode}）{suffix}"
            )
        return result

    def _ssh(self, *remote_argv: str, capture_output: bool = False) -> subprocess.CompletedProcess:
        return self._run(
            ["ssh", self.config.ssh_host, *remote_argv],
            capture_output=capture_output,
        )

    def preflight(self) -> None:
        """确认目标容器健康，且固件目录正由预期宿主路径读写挂载。"""
        result = self._ssh(
            "docker",
            "inspect",
            self.config.container_name,
            capture_output=True,
        )
        try:
            inspected = json.loads(result.stdout)
        except (TypeError, json.JSONDecodeError) as exc:
            raise OtaPublishError("docker inspect 输出不是有效 JSON") from exc
        if not isinstance(inspected, list) or len(inspected) != 1:
            raise OtaPublishError("docker inspect 未返回唯一目标容器")
        container = inspected[0]
        state = container.get("State") if isinstance(container, dict) else None
        health = state.get("Health") if isinstance(state, dict) else None
        if (
            not isinstance(state, dict)
            or state.get("Status") != "running"
            or not isinstance(health, dict)
            or health.get("Status") != "healthy"
        ):
            raise OtaPublishError("生产 Hub 容器未处于 running/healthy 状态")

        expected_source = f"{self.config.remote_service_root}/firmwares"
        mounts = container.get("Mounts")
        matched = [
            mount
            for mount in mounts if isinstance(mount, dict)
            and mount.get("Destination") == self.config.container_firmware_root
        ] if isinstance(mounts, list) else []
        if len(matched) != 1:
            raise OtaPublishError("生产 Hub 容器缺少唯一的固件目录挂载")
        mount = matched[0]
        if mount.get("Source") != expected_source or mount.get("RW") is not True:
            raise OtaPublishError(
                "生产 Hub 固件挂载来源或读写属性与发布配置不一致"
            )

    def _manifest_path(self, product: ProductConfig) -> str:
        return (
            f"{self.config.container_firmware_root}/manifests/"
            f"{product.firmware_target}.json"
        )

    def _read_manifest_text(self, product: ProductConfig) -> str | None:
        manifest_path = self._manifest_path(product)
        exists = self._run(
            [
                "ssh",
                self.config.ssh_host,
                "docker",
                "exec",
                self.config.container_name,
                "test",
                "-f",
                manifest_path,
            ],
            capture_output=True,
            allow_failure=True,
        )
        if exists.returncode == 1:
            return None
        if exists.returncode != 0:
            raise OtaPublishError("无法确认生产 Hub 当前 OTA 清单")
        return self._ssh(
            "docker",
            "exec",
            self.config.container_name,
            "cat",
            manifest_path,
            capture_output=True,
        ).stdout

    def minimum_ota_version(self, product: ProductConfig) -> int:
        self.preflight()
        try:
            return ota_minimum_from_manifest_text(
                self._read_manifest_text(product),
                product,
            )
        except SystemExit as exc:
            raise OtaPublishError("生产 Hub 当前 OTA 清单不满足发布契约") from exc

    def _load_release(
        self,
        firmware_path: Path,
        manifest_path: Path,
        product: ProductConfig,
    ) -> tuple[dict, dict]:
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise OtaPublishError(f"本地 OTA 清单无法解析：{exc}") from exc
        if not isinstance(manifest, dict) or (
            manifest.get("protocol_version") != 2
            or manifest.get("product_id") != product.product_id
            or manifest.get("firmware_target") != product.firmware_target
        ):
            raise OtaPublishError("本地 OTA 清单身份与产品配置不一致")
        artifacts = manifest.get("artifacts")
        app = artifacts.get("app") if isinstance(artifacts, dict) else None
        if not isinstance(app, dict):
            raise OtaPublishError("本地 OTA 清单缺少 artifacts.app")
        artifact_id = app.get("artifact_id")
        file_sha256 = app.get("file_sha256")
        size = app.get("size")
        ota_version = app.get("ota_version")
        if (
            not isinstance(artifact_id, str)
            or re.fullmatch(r"[0-9a-f]{64}", artifact_id) is None
            or not isinstance(file_sha256, str)
            or re.fullmatch(r"[0-9a-f]{64}", file_sha256) is None
            or isinstance(size, bool)
            or not isinstance(size, int)
            or size <= 0
            or isinstance(ota_version, bool)
            or not isinstance(ota_version, int)
            or ota_version <= 0
            or ota_version > MAX_SAFE_JSON_INTEGER
        ):
            raise OtaPublishError("本地 OTA 清单的 artifact 字段无效")
        if not firmware_path.is_file():
            raise OtaPublishError(f"本地 OTA 固件不存在：{firmware_path}")
        if firmware_path.stat().st_size != size or _file_sha256(firmware_path) != file_sha256:
            raise OtaPublishError("本地 OTA 固件大小或 SHA-256 与清单不一致")
        return manifest, app

    def _cleanup(self, host_paths: list[str], container_paths: list[str]) -> list[str]:
        errors: list[str] = []
        cleanup_commands = (
            [
                "ssh",
                self.config.ssh_host,
                "docker",
                "exec",
                "--user",
                "0",
                self.config.container_name,
                "rm",
                "-f",
                *container_paths,
            ],
            ["ssh", self.config.ssh_host, "rm", "-f", *host_paths],
        )
        for command in cleanup_commands:
            try:
                result = self.runner(
                    command,
                    capture_output=True,
                    input_text=None,
                )
            except OSError as exc:
                errors.append(f"{command[2]}: {exc}")
                continue
            if result.returncode != 0:
                errors.append(f"{command[2]} 退出码 {result.returncode}")
        return errors

    def publish(
        self,
        firmware_path: Path,
        manifest_path: Path,
        product: ProductConfig,
    ) -> str:
        """暂存输入、调用容器 helper、远端复核，并始终清理暂存文件。"""
        manifest, app = self._load_release(firmware_path, manifest_path, product)
        self.preflight()
        token = f"desksuite-ota-{product.firmware_target}-{app['artifact_id'][:12]}-{os.getpid()}-{time.time_ns()}"
        host_firmware = f"/tmp/{token}.bin"
        host_manifest = f"/tmp/{token}.json"
        host_helper = f"/tmp/{token}-remote_ota_publish.py"
        container_firmware = host_firmware
        container_manifest = host_manifest
        container_helper = host_helper
        host_paths = [host_firmware, host_manifest, host_helper]
        container_paths = [container_firmware, container_manifest, container_helper]
        artifact_path = (
            f"{self.config.container_firmware_root}/artifacts/"
            f"{app['artifact_id']}.bin"
        )
        failure: BaseException | None = None
        try:
            helper_path = Path(__file__).with_name("remote_ota_publish.py")
            for source, destination in (
                (firmware_path, host_firmware),
                (manifest_path, host_manifest),
                (helper_path, host_helper),
            ):
                self._run(
                    ["scp", str(source), f"{self.config.ssh_host}:{destination}"]
                )
            for source, destination in zip(host_paths, container_paths):
                self._ssh(
                    "docker",
                    "cp",
                    source,
                    f"{self.config.container_name}:{destination}",
                )
            self._ssh(
                "docker",
                "exec",
                "--user",
                "0",
                self.config.container_name,
                "python",
                container_helper,
                "--firmware",
                container_firmware,
                "--manifest",
                container_manifest,
                "--firmware-root",
                self.config.container_firmware_root,
                "--runtime-uid",
                str(self.config.runtime_uid),
                "--runtime-gid",
                str(self.config.runtime_gid),
            )

            remote_manifest_text = self._read_manifest_text(product)
            try:
                remote_manifest = json.loads(remote_manifest_text or "")
            except json.JSONDecodeError as exc:
                raise OtaPublishError("远端发布后的 manifest 无法解析") from exc
            if remote_manifest != manifest:
                raise OtaPublishError("远端发布后的 manifest 与本地输入不一致")
            remote_sha = self._ssh(
                "docker",
                "exec",
                self.config.container_name,
                "sha256sum",
                artifact_path,
                capture_output=True,
            ).stdout.split()
            remote_size = self._ssh(
                "docker",
                "exec",
                self.config.container_name,
                "stat",
                "-c",
                "%s",
                artifact_path,
                capture_output=True,
            ).stdout.strip()
            if not remote_sha or remote_sha[0] != app["file_sha256"]:
                raise OtaPublishError("远端 OTA artifact SHA-256 复核失败")
            if remote_size != str(app["size"]):
                raise OtaPublishError("远端 OTA artifact 大小复核失败")
            return artifact_path
        except BaseException as exc:
            failure = exc
            raise
        finally:
            cleanup_errors = self._cleanup(host_paths, container_paths)
            if cleanup_errors and failure is None:
                raise OtaPublishError(
                    "OTA 已发布，但远端暂存清理失败：" + "; ".join(cleanup_errors)
                )


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


# 导入模块时建立可测试的默认上下文；命令入口会按用户参数重新选择产品。
PRODUCTS = load_products()
select_product(PRODUCTS[sorted(PRODUCTS)[0]])


# ── 命令实现 ──────────────────────────────────────────────────────────────
def cmd_build(args: argparse.Namespace) -> int:
    write_step("编译项目")
    init_project_build_cache()
    check_fixed_environment()
    new_build_ota_version(
        OTA_VERSION_STATE_PATH,
        OTA_VERSION_HEADER_PATH,
        PRODUCT,
        minimum=1,
    )

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
    """查看 ds-<kind>-*.log 最新一份（build-log/flash-log/monitor-log 共用）。"""
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
    new_build_ota_version(
        OTA_VERSION_STATE_PATH,
        OTA_VERSION_HEADER_PATH,
        PRODUCT,
        minimum=1,
    )
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
    init_project_build_cache()
    check_fixed_environment()
    return run_idf_command(["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "menuconfig"])


def cmd_set_target_s3(args: argparse.Namespace) -> int:
    """把所选工程重新配置为 ESP32-S3 目标。"""
    write_step("设置目标芯片为 ESP32-S3")
    OTA_VERSION_HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    rc = run_idf_command(
        ["-C", str(PROJECT_ROOT), "-B", str(BUILD_PATH), "set-target", "esp32s3"]
    )
    if rc != 0:
        fail("设置 ESP32-S3 目标失败")
    print("✅ 目标已设置为 ESP32-S3")
    return 0


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
    new_build_ota_version(
        OTA_VERSION_STATE_PATH,
        OTA_VERSION_HEADER_PATH,
        PRODUCT,
        minimum=1,
    )
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
        PRODUCT.name,
        "--port",
        port,
    ]
    # 后台进程的 stdout/stderr 丢弃（它自己写 ds-monitor-latest.log）
    subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW,
        close_fds=True,
    )
    print(f"监控已在后台运行，日志：{latest_log_path('monitor')}")
    print(f"用 monitor-log 查看，用 kill-port --port {port} 停止。")
    return 0


def cmd_ota(args: argparse.Namespace) -> int:
    publish_mode, publish_target = resolve_ota_publish_target(
        getattr(args, "service_root", None)
    )
    remote_publisher: RemoteOtaPublisher | None = None
    if publish_mode == "local":
        service_root = publish_target
        assert isinstance(service_root, Path)
        minimum = _read_existing_ota_version(service_root, PRODUCT)
    else:
        assert isinstance(publish_target, OtaPublishConfig)
        remote_publisher = RemoteOtaPublisher(publish_target)
        try:
            minimum = remote_publisher.minimum_ota_version(PRODUCT)
        except OtaPublishError as exc:
            fail(str(exc))

    # 编译（用指定最小版本生成 OTA 版本头）
    write_step("编译项目")
    init_project_build_cache()
    check_fixed_environment()
    new_build_ota_version(
        OTA_VERSION_STATE_PATH,
        OTA_VERSION_HEADER_PATH,
        PRODUCT,
        minimum=minimum,
    )
    runlog = RunLog("build")
    print(f"📝 完整编译日志：{runlog.path}")
    rc = run_ninja(stdout=runlog.file_handle)
    runlog.close_and_finalize()
    if getattr(args, "full_log", False):
        print(runlog.read_text())
    if rc != 0:
        _show_build_failure(runlog)
        fail(f"编译失败，完整日志已保留：{runlog.path}")

    firmware_path = BUILD_PATH / PRODUCT.firmware_file
    if not firmware_path.exists():
        fail(f"未找到 OTA 固件：{firmware_path}")

    write_step("解析固件身份并发布到服务端")
    rc, info_text = run_esptool(["image-info", str(firmware_path)])
    if rc != 0:
        fail("esptool 无法读取固件身份")
    artifact_id, version = parse_image_info(info_text)
    file_sha256 = _file_sha256(firmware_path)
    size = firmware_path.stat().st_size
    ota_version = int(OTA_VERSION_STATE_PATH.read_text(encoding="utf-8").strip())
    if publish_mode == "local":
        published = publish_firmware(
            firmware_path,
            service_root,
            PRODUCT,
            version,
            ota_version,
            artifact_id,
            file_sha256,
            size,
        )
    else:
        assert remote_publisher is not None
        manifest = build_manifest(
            PRODUCT,
            version,
            ota_version,
            artifact_id,
            file_sha256,
            size,
        )
        with tempfile.TemporaryDirectory(prefix="desksuite-ota-") as temp_dir:
            manifest_path = Path(temp_dir) / f"{PRODUCT.firmware_target}.json"
            manifest_path.write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            try:
                published = remote_publisher.publish(
                    firmware_path,
                    manifest_path,
                    PRODUCT,
                )
            except OtaPublishError as exc:
                fail(str(exc))

    print(f"固件版本：{version}")
    print(f"产品：{PRODUCT.name}（product_id={PRODUCT.product_id}）")
    print(f"固件目标：{PRODUCT.firmware_target}")
    print(f"OTA 版本：{ota_version}")
    print(f"artifact_id：{artifact_id}")
    print(f"file_sha256：{file_sha256}")
    print(f"发布文件：{published}")
    print("✅ OTA 固件与运行时清单已原子发布")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ds.py",
        description="DeskSuite 统一设备构建与 OTA 发布工具",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add_product_argument(command_parser: argparse.ArgumentParser) -> None:
        command_parser.add_argument(
            "product",
            choices=sorted(PRODUCTS),
            help="目标设备产品",
        )

    p = sub.add_parser("build", help="使用固定环境编译，完整日志自动落盘")
    add_product_argument(p)
    p.add_argument("--full-log", action="store_true", help="编译结束后输出完整日志")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("flash", help="烧录固件")
    add_product_argument(p)
    p.add_argument("--port", default=None, help="串口；省略时使用产品默认值")
    p.add_argument("--full-log", action="store_true")
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser("flash-font", help="烧录字库分区")
    add_product_argument(p)
    p.add_argument("--port", default=None)
    p.set_defaults(func=cmd_flash_font)

    p = sub.add_parser("monitor", help="监控串口")
    add_product_argument(p)
    p.add_argument("--port", default=None)
    p.add_argument("--detach", action="store_true", help="后台运行，日志写 ds-monitor-latest.log")
    p.set_defaults(func=cmd_monitor)

    p = sub.add_parser("flash-monitor", help="烧录并监控")
    add_product_argument(p)
    p.add_argument("--port", default=None)
    p.set_defaults(func=cmd_flash_monitor)

    p = sub.add_parser("kill-port", help="释放指定串口")
    add_product_argument(p)
    p.add_argument("--port", default=None)
    p.set_defaults(func=cmd_kill_port)

    p = sub.add_parser("clean", help="清理产品 build 目录")
    add_product_argument(p)
    p.set_defaults(func=cmd_clean)

    p = sub.add_parser("menuconfig", help="打开菜单配置")
    add_product_argument(p)
    p.set_defaults(func=cmd_menuconfig)

    p = sub.add_parser("set-target-s3", help="把工程目标重新配置为 ESP32-S3")
    add_product_argument(p)
    p.set_defaults(func=cmd_set_target_s3)

    p = sub.add_parser("ota", help="编译并原子发布固件与目标清单")
    add_product_argument(p)
    p.add_argument(
        "--service-root",
        default=None,
        help="显式改为本地 Hub 目录发布；省略时发布到 Ubuntu 生产 Hub",
    )
    p.add_argument("--full-log", action="store_true")
    p.set_defaults(func=cmd_ota)

    log_specs = [
        ("build-log", cmd_build_log),
        ("flash-log", cmd_flash_log),
        ("monitor-log", cmd_monitor_log),
    ]
    for name, func in log_specs:
        p = sub.add_parser(name, help=f"查看最近{name.replace('-', '')}日志")
        add_product_argument(p)
        p.add_argument("--full-log", action="store_true")
        p.add_argument("--tail-lines", type=int, default=40)
        p.set_defaults(func=func)

    return parser


def main(argv: list[str] | None = None) -> int:
    global PRODUCTS
    PRODUCTS = load_products()
    if argv is None:
        argv = sys.argv[1:]
    if len(argv) >= 1 and argv[0] == "_monitor_run":
        p = argparse.ArgumentParser(prog="ds.py _monitor_run")
        p.add_argument("product", choices=sorted(PRODUCTS))
        p.add_argument("--port", default=None)
        args = p.parse_args(argv[1:])
        select_product(PRODUCTS[args.product])
        args.port = args.port or PRODUCT.default_port
        return cmd__monitor_run(args)
    parser = build_parser()
    args = parser.parse_args(argv)
    select_product(PRODUCTS[args.product])
    if hasattr(args, "port"):
        args.port = args.port or PRODUCT.default_port
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
