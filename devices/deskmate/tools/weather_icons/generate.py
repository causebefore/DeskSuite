#!/usr/bin/env python3
"""从 JSON manifest 生成全部静态 LVGL I1 资源包。"""
from __future__ import annotations

import argparse
import json
import os
import tempfile
from pathlib import Path

from i1_codegen import generate

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parents[1]
MANIFEST_DIR = HERE / "manifests"


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=path.name, dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        os.replace(temp_name, path)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)


def process(path: Path, check: bool) -> bool:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    source, header = generate(manifest, PROJECT_ROOT)
    outputs = {
        PROJECT_ROOT / manifest["outputs"]["c"]: source,
        PROJECT_ROOT / manifest["outputs"]["h"]: header,
    }
    changed = False
    for output, content in outputs.items():
        current = output.read_text(encoding="utf-8") if output.exists() else None
        if current == content:
            continue
        changed = True
        if check:
            print(f"[stale] {output}")
        else:
            write_atomic(output, content)
            print(f"[ok] {output}")
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 DeskMate 静态 I1 图片资源")
    parser.add_argument("manifests", nargs="*", type=Path)
    parser.add_argument("--all", action="store_true", help="处理 manifests 目录全部 JSON")
    parser.add_argument("--check", action="store_true", help="只检查生成文件是否过期")
    args = parser.parse_args()
    paths = sorted(MANIFEST_DIR.glob("*.json")) if args.all else args.manifests
    if not paths:
        parser.error("请指定 manifest 或使用 --all")
    stale = False
    for path in paths:
        stale = process(path.resolve(), args.check) or stale
    return 1 if args.check and stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
