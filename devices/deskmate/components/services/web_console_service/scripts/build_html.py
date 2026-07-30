"""将文件管理页面压缩为 ESP-IDF 组件使用的 C 源文件。"""

import argparse
import gzip
import pathlib
import re


PRESERVE_TAGS = ("pre", "code", "textarea", "script", "style")


def minify_html(html: str) -> str:
    """压缩普通 HTML 空白，同时保留文本和代码块的原始内容。"""
    tags = "|".join(PRESERVE_TAGS)
    blocks: list[str] = []

    def preserve(match: re.Match[str]) -> str:
        blocks.append(match.group(0))
        return f"__DESKMATE_PRESERVE_{len(blocks) - 1}__"

    html = re.sub(rf"<({tags})\b[^>]*>[\s\S]*?</\1>", preserve, html, flags=re.IGNORECASE)
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)
    html = re.sub(r">\s+<", "><", html)
    html = re.sub(r"\s+", " ", html).strip()
    for index, block in enumerate(blocks):
        html = html.replace(f"__DESKMATE_PRESERVE_{index}__", block)
    return html


def render_c_source(compressed: bytes) -> str:
    """将 gzip 数据渲染为固定格式的 C 源码。"""
    lines = [
        "/* 由 scripts/build_html.py 自动生成，请勿手工修改。 */",
        '#include "web_console_service_web.h"',
        "",
        "const uint8_t web_file_index_gz[] = {",
    ]
    for offset in range(0, len(compressed), 16):
        values = ", ".join(f"0x{byte:02x}" for byte in compressed[offset : offset + 16])
        lines.append(f"    {values},")
    lines.extend(("};", "const size_t web_file_index_gz_size = sizeof(web_file_index_gz);", ""))
    return "\n".join(lines)


def main() -> None:
    """读取页面并写出可由组件编译的 gzip C 数组。"""
    parser = argparse.ArgumentParser(description="生成内嵌的 gzip 文件管理页面")
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    html = arguments.input.read_text(encoding="utf-8")
    compressed = gzip.compress(minify_html(html).encode("utf-8"), compresslevel=9, mtime=0)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(render_c_source(compressed), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
