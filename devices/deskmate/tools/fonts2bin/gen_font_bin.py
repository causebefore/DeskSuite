#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 文件说明：生成 RLCD 外部字体分区使用的打包字体二进制。
"""生成 RLCD 外部字体分区使用的打包字体二进制。"""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path


TOOL_ROOT = Path(__file__).resolve().parent
MAGIC = b"RLCD"
VERSION = 1
INDEX_FMT = "<BHII"
DEFAULT_CN_FONT_NAME = "AlibabaPuHuiTi-3-55-Regular.ttf"
DEFAULT_CN_SEMIBOLD_FONT_NAME = "AlibabaPuHuiTi-3-75-SemiBold.ttf"
DEFAULT_NUM_FONT_NAME = "JetBrainsMono-Regular.ttf"


@dataclass(frozen=True)
class FontJob:
    font_id: int
    size_px: int
    font_kind: str
    ranges: tuple[str, ...]
    use_charset: bool = False
    extra_symbols: str = ""


JOBS = [
    FontJob(0, 16, "cn", ("0x20-0x7F",), use_charset=True),
    FontJob(1, 24, "cn", ("0x20-0x7F",), use_charset=True),
    FontJob(2, 32, "cn", ("0x20-0x7F",), use_charset=True),
    FontJob(3, 48, "num", ("0x20-0x7E",), extra_symbols=":. -°%"),
    FontJob(4, 16, "cn_semibold", ("0x20-0x7F",), use_charset=True),
    FontJob(5, 24, "cn_semibold", ("0x20-0x7F",), use_charset=True),
]


def discover_repo_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / "partitions.csv").exists() or (candidate / "idf.ps1").exists():
            return candidate
        esp32_root = candidate / "esp32"
        if (esp32_root / "partitions.csv").exists():
            return esp32_root
    return start


REPO_ROOT = discover_repo_root(TOOL_ROOT)


def repo_relative(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT)).replace("/", "\\")
    except ValueError:
        return str(path)


def first_existing(candidates: list[Path]) -> Path | None:
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def discover_named_file(filename: str, search_root: Path) -> Path | None:
    matches = sorted(
        path for path in search_root.rglob(filename)
        if "node_modules" not in path.parts
    )
    return matches[0] if matches else None


def resolve_file(explicit: Path | None, label: str, candidates: list[Path]) -> Path:
    if explicit is not None:
        resolved = explicit.expanduser().resolve()
        if not resolved.is_file():
            raise SystemExit(f"{label} not found: {resolved}")
        return resolved

    found = first_existing(candidates)
    if found is None or not found.is_file():
        joined = ", ".join(str(item) for item in candidates)
        raise SystemExit(f"{label} not found. tried: {joined}")
    return found.resolve()


def resolve_dir(explicit: Path | None, default_dir: Path) -> Path:
    if explicit is not None:
        resolved = explicit.expanduser().resolve()
        if not resolved.is_dir():
            raise SystemExit(f"directory not found: {resolved}")
        return resolved
    return default_dir.resolve()


def default_output_path(charset_file: Path) -> Path:
    return charset_file.parent / "font.bin"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="生成 RLCD 外置字体包")
    parser.add_argument("--fonts-dir", type=Path, help="字体源目录；未指定时按文件名自动查找")
    parser.add_argument("--charset", type=Path, help="字符集文件路径；未指定时按文件名自动查找")
    parser.add_argument("--cn-font", type=Path, help="中文字体 TTF 路径")
    parser.add_argument("--cn-semibold-font", type=Path, help="半粗中文字体 TTF 路径")
    parser.add_argument("--num-font", type=Path, help="数字字体 TTF 路径")
    parser.add_argument("--output", type=Path, help="输出 font.bin 路径；未指定时输出到字符集所在目录")
    parser.add_argument("--partitions", type=Path, help="分区表路径，默认自动查找仓库根 partitions.csv")
    parser.add_argument("--font-partition", default="font", help="字库分区名，默认 font")
    return parser.parse_args()


def converter_command() -> list[str]:
    local_bin = TOOL_ROOT / "node_modules" / ".bin" / "lv_font_conv"
    if os.name == "nt":
        local_bin = local_bin.with_suffix(".cmd")
    if local_bin.is_file():
        return [str(local_bin)]

    global_conv = shutil.which("lv_font_conv")
    if global_conv:
        return [global_conv]

    local_js = TOOL_ROOT / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
    node = shutil.which("node")
    if node and local_js.is_file():
        return [node, str(local_js)]

    raise SystemExit(
        "lv_font_conv not found. Install it globally or run npm install in tools/fonts2bin."
    )


def load_symbols(charset_file: Path, extra_symbols: str) -> str:
    seen: set[str] = set()
    ordered: list[str] = []

    def push_char(ch: str) -> None:
        if ch.isspace() or ch in seen:
            return
        seen.add(ch)
        ordered.append(ch)

    for line in charset_file.read_text(encoding="utf-8").splitlines():
        content = line.split("#", 1)[0]
        for ch in content:
            push_char(ch)

    for ch in extra_symbols:
        push_char(ch)

    return "".join(ordered)


def merge_symbols(base_symbols: str, extra_symbols: str) -> str:
    seen: set[str] = set()
    ordered: list[str] = []

    for ch in base_symbols:
        if ch.isspace() or ch in seen:
            continue
        seen.add(ch)
        ordered.append(ch)

    for ch in extra_symbols:
        if ch in seen:
            continue
        if ch != " " and ch.isspace():
                continue
        seen.add(ch)
        ordered.append(ch)

    return "".join(ordered)


def run_lv_font_conv(ttf: Path, job: FontJob, symbols: str, out_bin: Path) -> None:
    cmd = converter_command() + [
        "--font", str(ttf),
        "--format", "bin",
        "--bpp", "1",
        "--size", str(job.size_px),
    ]
    for glyph_range in job.ranges:
        cmd += ["--range", glyph_range]
    if symbols:
        cmd += ["--symbols", symbols]
    if os.environ.get("RLCD_FONT_NO_COMPRESS") == "1":
        cmd += ["--no-compress"]
    cmd += ["-o", str(out_bin)]
    subprocess.run(cmd, check=True)


def pack_fonts(blobs: list[tuple[int, int, bytes]]) -> bytes:
    header = MAGIC + struct.pack("<HH", VERSION, len(blobs))
    data_start = len(header) + len(blobs) * struct.calcsize(INDEX_FMT)
    index = bytearray()
    body = bytearray()
    offset = data_start

    for font_id, size_px, data in blobs:
        index += struct.pack(INDEX_FMT, font_id, size_px, offset, len(data))
        body += data
        offset += len(data)

    return header + bytes(index) + bytes(body)


def self_test(raw: bytes, expected_blobs: list[tuple[int, int, bytes]]) -> None:
    if raw[:4] != MAGIC:
        raise SystemExit("self-test failed: invalid magic")

    version, count = struct.unpack("<HH", raw[4:8])
    if version != VERSION or count != len(expected_blobs):
        raise SystemExit("self-test failed: invalid header")

    entry_size = struct.calcsize(INDEX_FMT)
    for index, expected in enumerate(expected_blobs):
        font_id, size_px, offset, length = struct.unpack(
            INDEX_FMT,
            raw[8 + index * entry_size: 8 + (index + 1) * entry_size],
        )
        if (font_id, size_px) != expected[:2]:
            raise SystemExit(f"self-test failed: invalid index entry {index}")
        if offset + length > len(raw):
            raise SystemExit(f"self-test failed: font block {index} out of range")
        if raw[offset: offset + length] != expected[2]:
            raise SystemExit(f"self-test failed: font block {index} payload mismatch")


def find_partition_offset(partitions_csv: Path, partition_name: str) -> str | None:
    with partitions_csv.open("r", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row:
                continue
            first = row[0].strip()
            if not first or first.startswith("#"):
                continue
            if first != partition_name:
                continue
            if len(row) < 4:
                return None
            offset = row[3].strip()
            return offset or None
    return None


def find_partition_size(partitions_csv: Path, partition_name: str) -> int | None:
    with partitions_csv.open("r", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row:
                continue
            first = row[0].strip()
            if not first or first.startswith("#") or first != partition_name:
                continue
            if len(row) < 5:
                return None
            try:
                return int(row[4].strip(), 0)
            except ValueError:
                return None
    return None


def write_flash_hint(output_path: Path, partitions_csv: Path | None, partition_name: str) -> None:
    flash_hint_path = output_path.with_name("flash_font.txt")
    output_rel = repo_relative(output_path)
    partitions_rel = repo_relative(partitions_csv) if partitions_csv else "partitions.csv"
    offset = find_partition_offset(partitions_csv, partition_name) if partitions_csv else None
    flash_offset = offset or "<font_offset>"

    lines = [
        "# Generate font.bin from the repository root.",
        "python tools\\fonts2bin\\gen_font_bin.py",
        "",
        "# Flash firmware first so the new partition table is active.",
        "idf.py --port COMx flash",
        "",
        "# Then write the generated font image to the font partition.",
        f"esptool.py --chip esp32s3 --port COMx --baud 921600 write_flash {flash_offset} {output_rel}",
    ]

    if offset is None:
        lines += [
            "",
            f"# Font partition offset was not resolved automatically. Check {partitions_rel}.",
        ]
    else:
        lines += [
            "",
            f"# Flash offset resolved from {partitions_rel}.",
        ]

    flash_hint_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()

    fonts_dir = resolve_dir(args.fonts_dir, TOOL_ROOT)
    charset_candidates = [discover_named_file("gb2312_chars.txt", TOOL_ROOT)]
    charset_file = resolve_file(
        args.charset,
        "symbols file",
        [candidate for candidate in charset_candidates if candidate is not None],
    )

    if args.fonts_dir is not None:
        cn_candidates = [fonts_dir / DEFAULT_CN_FONT_NAME]
        cn_semibold_candidates = [fonts_dir / DEFAULT_CN_SEMIBOLD_FONT_NAME]
        num_candidates = [fonts_dir / DEFAULT_NUM_FONT_NAME]
    else:
        cn_candidates = [discover_named_file(DEFAULT_CN_FONT_NAME, TOOL_ROOT)]
        cn_semibold_candidates = [discover_named_file(DEFAULT_CN_SEMIBOLD_FONT_NAME, TOOL_ROOT)]
        num_candidates = [discover_named_file(DEFAULT_NUM_FONT_NAME, TOOL_ROOT)]

    cn_font = resolve_file(args.cn_font, "cn font", [candidate for candidate in cn_candidates if candidate is not None])
    cn_semibold_font = resolve_file(
        args.cn_semibold_font,
        "cn semibold font",
        [candidate for candidate in cn_semibold_candidates if candidate is not None],
    )
    num_font = resolve_file(args.num_font, "numeric font", [candidate for candidate in num_candidates if candidate is not None])

    output_path = args.output.expanduser().resolve() if args.output else default_output_path(charset_file).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    partitions_csv = None
    if args.partitions is not None:
        partitions_csv = resolve_file(args.partitions, "partition table", [args.partitions])
    else:
        candidate = first_existing([REPO_ROOT / "partitions.csv", TOOL_ROOT / "partitions.csv"])
        if candidate is not None and candidate.is_file():
            partitions_csv = candidate.resolve()

    font_paths = {
        "cn": cn_font,
        "cn_semibold": cn_semibold_font,
        "num": num_font,
    }
    charset_symbols = load_symbols(charset_file, "")
    blobs: list[tuple[int, int, bytes]] = []

    print(f"repo root: {REPO_ROOT}")
    print(f"fonts dir: {cn_font.parent}")
    print(f"charset: {charset_file}")
    print(f"output: {output_path}")

    for job in JOBS:
        font_file = font_paths[job.font_kind]
        base_symbols = charset_symbols if job.use_charset else ""
        symbols = merge_symbols(base_symbols, job.extra_symbols)

        tmp = output_path.parent / f"_tmp_{job.font_id}.bin"
        run_lv_font_conv(font_file, job, symbols, tmp)
        data = tmp.read_bytes()
        tmp.unlink()
        blobs.append((job.font_id, job.size_px, data))
        print(f"{job.size_px}px: {len(data)} bytes")

    packed = pack_fonts(blobs)
    if partitions_csv is not None:
        partition_size = find_partition_size(partitions_csv, args.font_partition)
        if partition_size is not None and len(packed) > partition_size:
            raise SystemExit(
                f"font image too large: {len(packed)} bytes > "
                f"{args.font_partition} partition {partition_size} bytes"
            )
    output_path.write_bytes(packed)
    print(f"generated {output_path}: {len(packed)} bytes")

    self_test(output_path.read_bytes(), blobs)
    write_flash_hint(output_path, partitions_csv, args.font_partition)
    print(f"self-test passed: {len(blobs)} font blocks")
    print(f"flash hint: {output_path.with_name('flash_font.txt')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
