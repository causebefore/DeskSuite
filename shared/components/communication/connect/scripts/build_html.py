"""将配网 Portal 页面压缩为 ESP-IDF 组件使用的 C 源文件。"""

import argparse
import gzip
import html
import pathlib
import re


PRESERVE_TAGS = ("pre", "code", "textarea", "script", "style")
DEFAULT_SERVICE_URL_PLACEHOLDER = "{{CONNECT_PORTAL_DEFAULT_SERVICE_URL}}"


def minify_html(source: str) -> str:
    """压缩普通 HTML 空白，同时保留文本、脚本和样式块的内容。"""
    tags = "|".join(PRESERVE_TAGS)
    blocks: list[str] = []

    def preserve(match: re.Match[str]) -> str:
        blocks.append(match.group(0))
        return f"__CONNECT_PORTAL_PRESERVE_{len(blocks) - 1}__"

    source = re.sub(rf"<({tags})\b[^>]*>[\s\S]*?</\1>", preserve, source, flags=re.IGNORECASE)
    source = re.sub(r"<!--.*?-->", "", source, flags=re.DOTALL)
    source = re.sub(r">\s+<", "><", source)
    source = re.sub(r"\s+", " ", source).strip()
    for index, block in enumerate(blocks):
        source = source.replace(f"__CONNECT_PORTAL_PRESERVE_{index}__", block)
    return source


def load_index_html(path: pathlib.Path, default_service_url: str) -> str:
    """读取首页并安全替换构建配置中的默认服务地址。"""
    source = path.read_text(encoding="utf-8")
    placeholder_count = source.count(DEFAULT_SERVICE_URL_PLACEHOLDER)
    if placeholder_count != 1:
        raise ValueError(
            f"{path} 必须且只能包含一个 {DEFAULT_SERVICE_URL_PLACEHOLDER}，实际为 {placeholder_count}"
        )
    return source.replace(DEFAULT_SERVICE_URL_PLACEHOLDER, html.escape(default_service_url, quote=True))


def render_c_array(symbol: str, compressed: bytes) -> list[str]:
    """把一份 gzip 页面渲染为 C 数组及对应字节数常量。"""
    lines = [f"const uint8_t {symbol}[] = {{"]
    for offset in range(0, len(compressed), 16):
        values = ", ".join(f"0x{byte:02x}" for byte in compressed[offset : offset + 16])
        lines.append(f"    {values},")
    lines.extend(("};", f"const size_t {symbol}_size_bytes = sizeof({symbol});", ""))
    return lines


def compress_html(source: str) -> bytes:
    """以确定性 gzip 格式压缩 HTML。"""
    return gzip.compress(minify_html(source).encode("utf-8"), compresslevel=9, mtime=0)


def render_c_source(index_html: str, success_html: str) -> str:
    """压缩两张页面并生成组件私有 C 源码。"""
    index_gzip = compress_html(index_html)
    success_gzip = compress_html(success_html)

    lines = [
        "/* 由 scripts/build_html.py 自动生成，请勿手工修改。 */",
        '#include "connect_portal_web.h"',
        "",
    ]
    lines.extend(render_c_array("connect_portal_index_gzip", index_gzip))
    lines.extend(render_c_array("connect_portal_success_gzip", success_gzip))
    return "\n".join(lines)


def main() -> None:
    """读取配网页面并写出可由组件编译的 gzip C 数组。"""
    parser = argparse.ArgumentParser(description="生成内嵌的 gzip 配网 Portal 页面")
    parser.add_argument("--index", required=True, type=pathlib.Path)
    parser.add_argument("--success", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--default-service-url", default="")
    arguments = parser.parse_args()

    index_html = load_index_html(arguments.index, arguments.default_service_url)
    success_html = arguments.success.read_text(encoding="utf-8")
    generated_source = render_c_source(index_html, success_html)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(generated_source, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
