#!/usr/bin/env python3
"""为官方 ESP-IDF CI 构建准备 OTA 身份，并在构建后生成发布清单。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path


SUITE_ROOT = Path(__file__).resolve().parent.parent
PRODUCTS_PATH = SUITE_ROOT / "products.toml"
MAX_SAFE_JSON_INTEGER = 9_007_199_254_740_991
_PRODUCT_NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
_FIRMWARE_TARGET_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_VALIDATION_HASH_RE = re.compile(
    r"^\s*Validation hash:\s*([0-9a-f]{64})\s*\(valid\)\s*$",
    re.IGNORECASE | re.MULTILINE,
)
_APP_VERSION_RE = re.compile(
    r"^\s*App version:\s*(.+?)\s*$",
    re.IGNORECASE | re.MULTILINE,
)


@dataclass(frozen=True)
class Product:
    name: str
    product_id: int
    firmware_target: str
    project_dir: Path
    firmware_file: str


def load_product(
    name: str,
    *,
    suite_root: Path = SUITE_ROOT,
    products_path: Path = PRODUCTS_PATH,
) -> Product:
    """读取并校验单个产品的 CI 构建信息。"""
    if _PRODUCT_NAME_RE.fullmatch(name) is None:
        raise ValueError(f"产品名称非法：{name}")
    data = tomllib.loads(products_path.read_text(encoding="utf-8"))
    raw_products = data.get("products")
    raw = raw_products.get(name) if isinstance(raw_products, dict) else None
    if not isinstance(raw, dict):
        raise ValueError(f"产品不存在：{name}")
    try:
        product_id = int(raw["product_id"])
        firmware_target = str(raw["firmware_target"])
        project_dir = (suite_root / str(raw["project_dir"])).resolve()
        firmware_file = str(raw["firmware_file"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"产品配置字段无效：{name}: {exc}") from exc
    if product_id <= 0:
        raise ValueError("product_id 必须为正整数")
    if _FIRMWARE_TARGET_RE.fullmatch(firmware_target) is None:
        raise ValueError("firmware_target 非法")
    if project_dir.parent != (suite_root.resolve() / "devices"):
        raise ValueError("产品工程目录必须是 devices 下的直接子目录")
    if not (project_dir / "CMakeLists.txt").is_file():
        raise ValueError(f"产品工程缺少 CMakeLists.txt：{project_dir}")
    if Path(firmware_file).name != firmware_file or not firmware_file.endswith(".bin"):
        raise ValueError("firmware_file 必须是单个 .bin 文件名")
    return Product(name, product_id, firmware_target, project_dir, firmware_file)


def _relative_posix(path: Path, suite_root: Path) -> str:
    return path.resolve().relative_to(suite_root.resolve()).as_posix()


def _write_outputs(path: Path | None, values: dict[str, str | int]) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8") as stream:
        for key, value in values.items():
            text = str(value)
            if "\n" in text or "\r" in text:
                raise ValueError(f"GitHub 输出 {key} 不能包含换行")
            stream.write(f"{key}={text}\n")


def prepare_build(
    product: Product,
    *,
    suite_root: Path = SUITE_ROOT,
    ota_version: int | None = None,
) -> dict[str, str | int]:
    """生成构建目录中的 OTA 身份覆盖头，并返回官方构建 Action 所需路径。"""
    version = int(time.time() * 1000) if ota_version is None else ota_version
    if version <= 0 or version > MAX_SAFE_JSON_INTEGER:
        raise ValueError("OTA 版本超出 JSON 安全正整数范围")
    build_dir = product.project_dir / "build"
    header_path = build_dir / "generated" / "firmware_ota_build_project.h"
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text(
        "/** @file firmware_ota_build_project.h @brief 由 CI 注入的产品与 OTA 身份覆盖。 */\n"
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"#define DESKSUITE_PRODUCT_ID UINT32_C({product.product_id})\n"
        f'#define DESKSUITE_FIRMWARE_TARGET "{product.firmware_target}"\n'
        f"#define FIRMWARE_OTA_BUILD_VERSION UINT64_C({version})\n",
        encoding="utf-8",
    )
    image_info_path = build_dir / "image-info.txt"
    return {
        "project_dir": _relative_posix(product.project_dir, suite_root),
        "firmware_file": product.firmware_file,
        "firmware_path": _relative_posix(build_dir / product.firmware_file, suite_root),
        "image_info_path": _relative_posix(image_info_path, suite_root),
        "ota_version": version,
    }


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def finalize_release(
    product: Product,
    ota_version: int,
    *,
    suite_root: Path = SUITE_ROOT,
) -> dict[str, str | int]:
    """校验官方构建产物与 image-info 输出，生成待发布 OTA 清单。"""
    if ota_version <= 0 or ota_version > MAX_SAFE_JSON_INTEGER:
        raise ValueError("OTA 版本超出 JSON 安全正整数范围")
    build_dir = product.project_dir / "build"
    firmware_path = build_dir / product.firmware_file
    image_info_path = build_dir / "image-info.txt"
    if not firmware_path.is_file():
        raise ValueError(f"官方构建未生成固件：{firmware_path}")
    if not image_info_path.is_file():
        raise ValueError(f"缺少 esptool image-info 输出：{image_info_path}")
    image_info = image_info_path.read_text(encoding="utf-8", errors="replace")
    hash_match = _VALIDATION_HASH_RE.search(image_info)
    version_match = _APP_VERSION_RE.search(image_info)
    if hash_match is None or version_match is None:
        raise ValueError("无法从 esptool image-info 解析 Validation hash 或 App version")
    artifact_id = hash_match.group(1).lower()
    app_version = version_match.group(1).strip()
    if not app_version or len(app_version) > 64:
        raise ValueError("固件 App version 无效")

    manifest = {
        "protocol_version": 2,
        "product_id": product.product_id,
        "firmware_target": product.firmware_target,
        "artifacts": {
            "app": {
                "version": app_version,
                "ota_version": ota_version,
                "artifact_id": artifact_id,
                "file_sha256": _file_sha256(firmware_path),
                "size": firmware_path.stat().st_size,
            }
        },
    }
    release_dir = suite_root / ".release"
    release_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = release_dir / f"{product.firmware_target}.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return {
        "firmware": str(firmware_path.resolve()),
        "manifest": str(manifest_path.resolve()),
        "artifact_id": artifact_id,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("prepare", "finalize"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--product", required=True)
        command_parser.add_argument(
            "--github-output",
            type=Path,
            default=Path(os.environ["GITHUB_OUTPUT"]) if os.environ.get("GITHUB_OUTPUT") else None,
        )
    subparsers.choices["finalize"].add_argument("--ota-version", required=True, type=int)
    args = parser.parse_args()
    try:
        product = load_product(args.product)
        if args.command == "prepare":
            outputs = prepare_build(product)
        else:
            outputs = finalize_release(product, args.ota_version)
        _write_outputs(args.github_output, outputs)
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        parser.error(str(exc))
    print(json.dumps(outputs, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
