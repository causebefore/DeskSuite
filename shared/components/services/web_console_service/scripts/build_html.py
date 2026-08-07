"""组装并压缩 ESP-IDF 网页控制台的公共壳与可选模块。"""

import argparse
import gzip
import pathlib
import re
from collections.abc import Iterable, Sequence


PRESERVE_TAGS = ("pre", "code", "textarea", "script", "style")
MODULE_ORDER = ("files", "settings", "status", "actions")
MODULE_STYLES = {"files": "modules/files.css"}
FIELDS_MODULE_USERS = frozenset(("settings", "status", "actions"))
STYLE_PLACEHOLDER = "<!-- WEB_CONSOLE_STYLES -->"
MODULE_PLACEHOLDER = "<!-- WEB_CONSOLE_MODULES -->"
SCRIPT_PLACEHOLDER = "<!-- WEB_CONSOLE_SCRIPTS -->"


def normalize_modules(modules: Iterable[str]) -> tuple[str, ...]:
    """校验模块列表并按固件约定的固定顺序返回。"""
    requested = tuple(modules)
    unknown = sorted(set(requested).difference(MODULE_ORDER))
    if unknown:
        raise ValueError(f"未知网页控制台模块：{', '.join(unknown)}")

    duplicates = sorted(module for module in set(requested) if requested.count(module) > 1)
    if duplicates:
        raise ValueError(f"网页控制台模块重复：{', '.join(duplicates)}")

    return tuple(module for module in MODULE_ORDER if module in requested)


def resolve_cli_modules(modules: Sequence[str] | None) -> tuple[str, ...]:
    """解析显式构建模块；未传入模块时生成纯 Core 页面。"""
    return normalize_modules(()) if modules is None else normalize_modules(modules)


def read_fragment(web_root: pathlib.Path, relative_path: str) -> str:
    """读取一个构建期网页片段。"""
    return (web_root / relative_path).read_text(encoding="utf-8").strip()


def assemble_html(input_path: pathlib.Path, modules: Iterable[str]) -> str:
    """将公共壳、公共资源和可选模块组装为单页 HTML。"""
    enabled_modules = normalize_modules(modules)
    web_root = input_path.parent
    html = input_path.read_text(encoding="utf-8")

    for placeholder in (STYLE_PLACEHOLDER, MODULE_PLACEHOLDER, SCRIPT_PLACEHOLDER):
        if html.count(placeholder) != 1:
            raise ValueError(f"网页模板必须且只能包含一个占位符：{placeholder}")

    styles = [read_fragment(web_root, "common.css")]
    module_markup: list[str] = []
    navigation: list[str] = []
    if "files" in enabled_modules:
        navigation.append(
            '  { modules: ["files"], navigation: { id: "files", label: "文件管理" } },'
        )
    settings_modules = [
        module for module in MODULE_ORDER
        if module in FIELDS_MODULE_USERS and module in enabled_modules
    ]
    if settings_modules:
        module_list = ", ".join(f'"{module}"' for module in settings_modules)
        navigation.append(
            f'  {{ modules: [{module_list}], '
            'navigation: { id: "settings", label: "设置" } },'
        )
    scripts = [
        "window.webConsoleNavigation = Object.freeze([\n" +
        "\n".join(navigation) +
        "\n]);",
        read_fragment(web_root, "common.js"),
    ]

    for module in enabled_modules:
        if style_path := MODULE_STYLES.get(module):
            styles.append(read_fragment(web_root, style_path))
        module_markup.append(read_fragment(web_root, f"modules/{module}.html"))

    if FIELDS_MODULE_USERS.intersection(enabled_modules):
        styles.append(read_fragment(web_root, "modules/fields.css"))
        module_markup.insert(0, read_fragment(web_root, "modules/fields.html"))
        scripts.append(read_fragment(web_root, "modules/fields.js"))

    for module in enabled_modules:
        scripts.append(read_fragment(web_root, f"modules/{module}.js"))

    joined_styles = "\n".join(styles)
    joined_scripts = "\n".join(scripts)
    html = html.replace(STYLE_PLACEHOLDER, f"<style>\n{joined_styles}\n</style>")
    html = html.replace(MODULE_PLACEHOLDER, "\n".join(module_markup))
    html = html.replace(SCRIPT_PLACEHOLDER, f"<script>\n{joined_scripts}\n</script>")
    return html


def minify_html(html: str) -> str:
    """压缩普通 HTML 空白，同时保留文本和代码块的原始内容。"""
    tags = "|".join(PRESERVE_TAGS)
    blocks: list[str] = []

    def compact_css(source: str) -> str:
        """压缩 CSS 结构空白，同时保留字符串字面量内容。"""
        output: list[str] = []
        quote = ""
        escaped = False
        pending_space = False
        index = 0
        delimiters = "{}:;,>"
        while index < len(source):
            character = source[index]
            if quote:
                output.append(character)
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = ""
                index += 1
                continue
            if source.startswith("/*", index):
                closing = source.find("*/", index + 2)
                index = len(source) if closing < 0 else closing + 2
                continue
            if character in "\"'":
                if pending_space and output and output[-1] not in delimiters:
                    output.append(" ")
                pending_space = False
                quote = character
                output.append(character)
            elif character.isspace():
                pending_space = True
            else:
                if character in delimiters:
                    while output and output[-1] == " ":
                        output.pop()
                elif pending_space and output and output[-1] not in delimiters:
                    output.append(" ")
                pending_space = False
                output.append(character)
            index += 1
        return "".join(output).strip()

    def compact_javascript(source: str) -> str:
        """压缩字面量之外的 JS 空白与注释。"""
        output: list[str] = []
        quote = ""
        escaped = False
        pending_space = False
        index = 0
        delimiters = "{}[]();,:"
        while index < len(source):
            character = source[index]
            if quote:
                output.append(character)
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = ""
                index += 1
                continue
            if source.startswith("//", index):
                newline = source.find("\n", index + 2)
                index = len(source) if newline < 0 else newline + 1
                pending_space = True
                continue
            if source.startswith("/*", index):
                closing = source.find("*/", index + 2)
                if closing < 0:
                    index = len(source)
                else:
                    comment = source[index : closing + 2]
                    if "Copyright" in comment or "Crosslink" in comment:
                        if pending_space and output:
                            output.append(" ")
                        output.append(re.sub(r"\s+", " ", comment))
                    index = closing + 2
                pending_space = True
                continue
            if character in "\"'`":
                if pending_space and output and output[-1] not in delimiters:
                    output.append(" ")
                pending_space = False
                quote = character
                output.append(character)
            elif character.isspace():
                pending_space = True
            else:
                if character in delimiters:
                    while output and output[-1] == " ":
                        output.pop()
                elif pending_space and output and output[-1] not in delimiters:
                    output.append(" ")
                pending_space = False
                output.append(character)
            index += 1
        return "".join(output).strip()

    def compact_code_block(block: str, tag: str) -> str:
        opening_end = block.find(">") + 1
        closing_start = block.lower().rfind(f"</{tag}")
        opening = block[:opening_end]
        closing = block[closing_start:]
        source = block[opening_end:closing_start]
        if tag == "style":
            body = compact_css(source)
        else:
            body = compact_javascript(source)
        return f"{opening}\n{body}\n{closing}"

    def preserve(match: re.Match[str]) -> str:
        tag = match.group(1).lower()
        block = match.group(0)
        if tag in ("script", "style"):
            block = compact_code_block(block, tag)
        blocks.append(block)
        return f"__WEB_CONSOLE_PRESERVE_{len(blocks) - 1}__"

    html = re.sub(rf"<({tags})\b[^>]*>[\s\S]*?</\1>", preserve, html, flags=re.IGNORECASE)
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)
    html = re.sub(r">\s+<", "><", html)
    html = re.sub(r"\s+", " ", html).strip()
    for index, block in enumerate(blocks):
        html = html.replace(f"__WEB_CONSOLE_PRESERVE_{index}__", block)
    return html


def compress_html(html: str) -> bytes:
    """以确定性 gzip 参数压缩组装后的页面。"""
    return gzip.compress(minify_html(html).encode("utf-8"), compresslevel=9, mtime=0)


def render_c_source(compressed: bytes) -> str:
    """将 gzip 数据渲染为固定格式的 C 源码。"""
    lines = [
        "/* 由 scripts/build_html.py 自动生成，请勿手工修改。 */",
        '#include "web_console_service_web.h"',
        "",
        "const uint8_t web_console_index_gz[] = {",
    ]
    for offset in range(0, len(compressed), 16):
        values = ", ".join(f"0x{byte:02x}" for byte in compressed[offset : offset + 16])
        lines.append(f"    {values},")
    lines.extend(("};", "const size_t web_console_index_gz_size = sizeof(web_console_index_gz);", ""))
    return "\n".join(lines)


def main() -> None:
    """读取页面片段并写出可由组件编译的 gzip C 数组。"""
    parser = argparse.ArgumentParser(description="生成内嵌的 gzip 网页控制台页面")
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument(
        "--module",
        action="append",
        dest="modules",
        metavar="NAME",
        help="启用网页模块；可重复传入，支持 files、settings、status、actions",
    )
    arguments = parser.parse_args()

    try:
        modules = resolve_cli_modules(arguments.modules)
        html = assemble_html(arguments.input, modules)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    compressed = compress_html(html)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(render_c_source(compressed), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
