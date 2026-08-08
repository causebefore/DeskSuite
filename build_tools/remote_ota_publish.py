#!/usr/bin/env python3
"""在 Hub 容器内把 OTA artifact 与 manifest 原子发布到 bind mount。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator


MAX_SAFE_JSON_INTEGER = 9_007_199_254_740_991
MAX_MANIFEST_BYTES = 64 * 1024
_FIRMWARE_TARGET_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class PublishError(RuntimeError):
    """远端 OTA 发布输入或文件状态不满足安全契约。"""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _positive_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise PublishError(f"manifest {field} 必须是正整数")
    return value


def _load_manifest(path: Path) -> tuple[dict, dict]:
    if not path.is_file():
        raise PublishError(f"manifest 不存在：{path}")
    if path.stat().st_size <= 0 or path.stat().st_size > MAX_MANIFEST_BYTES:
        raise PublishError("manifest 大小非法")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PublishError(f"manifest 无法解析：{exc}") from exc
    if not isinstance(data, dict) or data.get("protocol_version") != 2:
        raise PublishError("manifest protocol_version 必须为 2")
    _positive_int(data.get("product_id"), "product_id")
    target = data.get("firmware_target")
    if not isinstance(target, str) or _FIRMWARE_TARGET_RE.fullmatch(target) is None:
        raise PublishError("manifest firmware_target 非法")
    artifacts = data.get("artifacts")
    app = artifacts.get("app") if isinstance(artifacts, dict) else None
    if not isinstance(app, dict):
        raise PublishError("manifest 缺少 artifacts.app")
    version = app.get("version")
    if (
        not isinstance(version, str)
        or not version
        or len(version) > 64
        or "\x00" in version
    ):
        raise PublishError("manifest version 非法")
    ota_version = _positive_int(app.get("ota_version"), "ota_version")
    if ota_version > MAX_SAFE_JSON_INTEGER:
        raise PublishError("manifest ota_version 超过 JSON 安全整数上限")
    artifact_id = app.get("artifact_id")
    file_sha256 = app.get("file_sha256")
    if not isinstance(artifact_id, str) or _SHA256_RE.fullmatch(artifact_id) is None:
        raise PublishError("manifest artifact_id 非法")
    if not isinstance(file_sha256, str) or _SHA256_RE.fullmatch(file_sha256) is None:
        raise PublishError("manifest file_sha256 非法")
    _positive_int(app.get("size"), "size")
    return data, app


def _validate_firmware(path: Path, app: dict) -> None:
    if not path.is_file():
        raise PublishError(f"固件不存在：{path}")
    if path.stat().st_size != app["size"]:
        raise PublishError("固件大小与 manifest 不一致")
    if _sha256(path) != app["file_sha256"]:
        raise PublishError("固件 SHA-256 与 manifest 不一致")


def _apply_owner_mode(
    path: Path,
    mode: int,
    runtime_uid: int | None,
    runtime_gid: int | None,
) -> None:
    os.chmod(path, mode)
    if runtime_uid is None and runtime_gid is None:
        return
    if runtime_uid is None or runtime_gid is None:
        raise PublishError("runtime_uid/runtime_gid 必须同时提供")
    os.chown(path, runtime_uid, runtime_gid)


def _ensure_directory(
    path: Path,
    runtime_uid: int | None,
    runtime_gid: int | None,
) -> None:
    path.mkdir(parents=True, exist_ok=True)
    _apply_owner_mode(path, 0o750, runtime_uid, runtime_gid)


def _fsync_directory(path: Path) -> None:
    if os.name == "nt":
        return
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


@contextmanager
def _exclusive_lock(path: Path) -> Iterator[None]:
    """Linux 使用 flock；Windows 测试使用 msvcrt 单字节锁。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        handle.seek(0, os.SEEK_END)
        if handle.tell() == 0:
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def _atomic_copy(
    source: Path,
    destination: Path,
    mode: int,
    runtime_uid: int | None,
    runtime_gid: int | None,
) -> None:
    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
    try:
        with source.open("rb") as source_handle, temporary.open("xb") as target_handle:
            shutil.copyfileobj(source_handle, target_handle, length=1024 * 1024)
            target_handle.flush()
            os.fsync(target_handle.fileno())
        _apply_owner_mode(temporary, mode, runtime_uid, runtime_gid)
        os.replace(temporary, destination)
        _fsync_directory(destination.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def _atomic_write_manifest(
    data: dict,
    destination: Path,
    runtime_uid: int | None,
    runtime_gid: int | None,
) -> None:
    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
    content = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    try:
        with temporary.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        _apply_owner_mode(temporary, 0o640, runtime_uid, runtime_gid)
        os.replace(temporary, destination)
        _fsync_directory(destination.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def publish_remote_files(
    firmware_path: Path,
    manifest_path: Path,
    firmware_root: Path,
    runtime_uid: int | None,
    runtime_gid: int | None,
) -> Path:
    """校验并发布固件；manifest 的原子替换是唯一提交点。"""
    manifest, app = _load_manifest(manifest_path)
    _validate_firmware(firmware_path, app)
    target = manifest["firmware_target"]
    product_id = manifest["product_id"]

    root = firmware_root.resolve()
    artifact_dir = root / "artifacts"
    manifest_dir = root / "manifests"
    _ensure_directory(root, runtime_uid, runtime_gid)
    _ensure_directory(artifact_dir, runtime_uid, runtime_gid)
    _ensure_directory(manifest_dir, runtime_uid, runtime_gid)

    artifact_path = artifact_dir / f"{app['artifact_id']}.bin"
    current_manifest_path = manifest_dir / f"{target}.json"
    lock_path = manifest_dir / ".publish.lock"

    with _exclusive_lock(lock_path):
        current_manifest: dict | None = None
        current_app: dict | None = None
        if current_manifest_path.exists():
            current_manifest, current_app = _load_manifest(current_manifest_path)
            if (
                current_manifest["product_id"] != product_id
                or current_manifest["firmware_target"] != target
            ):
                raise PublishError("当前 manifest 身份与待发布固件不一致")
            if app["ota_version"] <= current_app["ota_version"]:
                raise PublishError("待发布 ota_version 不高于当前生产版本")

        if artifact_path.exists():
            _validate_firmware(artifact_path, app)
        else:
            _atomic_copy(
                firmware_path,
                artifact_path,
                0o440,
                runtime_uid,
                runtime_gid,
            )
            _validate_firmware(artifact_path, app)

        if current_manifest is not None and current_app is not None:
            history_dir = manifest_dir / "history" / target
            _ensure_directory(history_dir, runtime_uid, runtime_gid)
            history_path = history_dir / f"{current_app['ota_version']}.json"
            if history_path.exists():
                archived, _ = _load_manifest(history_path)
                if archived != current_manifest:
                    raise PublishError("相同版本的 history manifest 内容冲突")
            else:
                _atomic_write_manifest(
                    current_manifest,
                    history_path,
                    runtime_uid,
                    runtime_gid,
                )

        _atomic_write_manifest(
            manifest,
            current_manifest_path,
            runtime_uid,
            runtime_gid,
        )

        final_manifest, final_app = _load_manifest(current_manifest_path)
        if final_manifest != manifest or final_app != app:
            raise PublishError("最终 manifest 复核不一致")
        _validate_firmware(artifact_path, final_app)

    return artifact_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="容器内原子发布 DeskSuite OTA 固件")
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--firmware-root", required=True, type=Path)
    parser.add_argument("--runtime-uid", required=True, type=int)
    parser.add_argument("--runtime-gid", required=True, type=int)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        published = publish_remote_files(
            args.firmware,
            args.manifest,
            args.firmware_root,
            args.runtime_uid,
            args.runtime_gid,
        )
    except (OSError, PublishError) as exc:
        print(f"远端 OTA 发布失败：{exc}", file=sys.stderr)
        return 1
    print(f"远端 OTA artifact 已发布：{published}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
