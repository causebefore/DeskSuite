"""验证网页控制台构建期模块裁剪与确定性输出。"""

import itertools
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

import build_html


COMPONENT_ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB_ROOT = COMPONENT_ROOT / "web"
INDEX_TEMPLATE = WEB_ROOT / "index.html"
MODULE_MARKERS = {
    "files": (
        'data-web-console-module="files"',
        "/api/files",
        "上传到当前目录",
    ),
    "settings": (
        'data-web-console-module="settings"',
        "/api/settings",
        "保存更改",
    ),
    "status": (
        'data-web-console-module="status"',
        "/api/status",
        "刷新状态",
    ),
}


class BuildHtmlTests(unittest.TestCase):
    def module_combinations(self):
        for length in range(len(build_html.MODULE_ORDER) + 1):
            yield from itertools.combinations(build_html.MODULE_ORDER, length)

    def test_all_module_combinations_are_physically_trimmed(self):
        for enabled in self.module_combinations():
            with self.subTest(enabled=enabled):
                html = build_html.minify_html(build_html.assemble_html(INDEX_TEMPLATE, enabled))
                self.assertIn("/api/session", html)
                self.assertIn("/api/capabilities", html)
                self.assertNotIn(".innerHTML", html)
                for module, markers in MODULE_MARKERS.items():
                    for marker in markers:
                        if module in enabled:
                            self.assertIn(marker, html)
                        else:
                            self.assertNotIn(marker, html)

                needs_fields = bool({"settings", "status"}.intersection(enabled))
                self.assertEqual("window.webConsole.fields" in html, needs_fields)

    def test_core_only_contains_no_module_contract_or_copy(self):
        html = build_html.minify_html(build_html.assemble_html(INDEX_TEMPLATE, ()))
        for markers in MODULE_MARKERS.values():
            for marker in markers:
                self.assertNotIn(marker, html)
        self.assertNotIn("Crosslink", html)
        self.assertNotIn("项目设置", html)
        self.assertNotIn("设备状态", html)
        self.assertNotIn("文件管理", html)

    def test_module_order_is_fixed_independent_of_arguments(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("status", "files", "settings"),
        )
        positions = [
            html.index(f'data-web-console-module="{module}"')
            for module in build_html.MODULE_ORDER
        ]
        self.assertEqual(positions, sorted(positions))

        endpoint_positions = [
            html.index(MODULE_MARKERS[module][1])
            for module in build_html.MODULE_ORDER
        ]
        self.assertEqual(endpoint_positions, sorted(endpoint_positions))

    def test_assembled_page_has_unique_ids_and_no_placeholders(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("files", "settings", "status"),
        )
        ids = re.findall(r'\bid="([^"]+)"', html)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertNotIn("WEB_CONSOLE_STYLES", html)
        self.assertNotIn("WEB_CONSOLE_MODULES", html)
        self.assertNotIn("WEB_CONSOLE_SCRIPTS", html)

    def test_unknown_and_duplicate_modules_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "未知网页控制台模块"):
            build_html.normalize_modules(("files", "unknown"))
        with self.assertRaisesRegex(ValueError, "网页控制台模块重复"):
            build_html.normalize_modules(("status", "status"))

    def test_cli_accepts_repeated_module_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "web_console_page.c"
            result = subprocess.run(
                (
                    sys.executable,
                    str(pathlib.Path(build_html.__file__)),
                    "--input",
                    str(INDEX_TEMPLATE),
                    "--output",
                    str(output),
                    "--module",
                    "status",
                    "--module",
                    "files",
                ),
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = output.read_text(encoding="utf-8")
            self.assertIn("const uint8_t web_console_index_gz[]", generated)
            self.assertIn("web_console_index_gz_size", generated)

    def test_gzip_output_is_deterministic(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("files", "settings", "status"),
        )
        first = build_html.compress_html(html)
        second = build_html.compress_html(html)
        self.assertEqual(first, second)
        self.assertEqual(first[0:3], b"\x1f\x8b\x08")
        self.assertEqual(first[4:8], b"\x00\x00\x00\x00")

    def test_cli_without_modules_builds_core_only(self):
        self.assertEqual(
            build_html.resolve_cli_modules(None),
            (),
        )
        self.assertEqual(
            build_html.resolve_cli_modules(["status", "files"]),
            ("files", "status"),
        )

    def test_files_attribution_stays_with_files_module(self):
        files_html = build_html.minify_html(
            build_html.assemble_html(INDEX_TEMPLATE, ("files",)),
        )
        core_html = build_html.minify_html(
            build_html.assemble_html(INDEX_TEMPLATE, ()),
        )
        self.assertIn("Crosslink src/network/html/FilesPage.html", files_html)
        self.assertIn("Copyright (c) 2025 Dave Allie, MIT License.", files_html)
        self.assertNotIn("Crosslink src/network/html/FilesPage.html", core_html)

    def test_files_module_keeps_existing_feature_contracts(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("files",))
        for marker in (
            'request.open("PUT"',
            'activeUploadRequest.abort()',
            '"X-Web-Console-Overwrite"',
            'loadDirectory(currentPath)',
            'response.blob()',
            'API.directory',
            '"PATCH"',
            '"DELETE"',
            'filterInput.addEventListener("input"',
            'sortSelect.addEventListener("change"',
            'selectAll.addEventListener("change"',
            'payload.totalBytes',
            'payload.freeBytes',
        ):
            self.assertIn(marker, html)

    def test_settings_protocol_keeps_uint64_values_as_strings(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("settings",))
        self.assertIn("expectedVersion: version", html)
        self.assertIn("&request=${encodeURIComponent(requestId)}", html)
        self.assertIn("UINT64_PATTERN.test(payload.requestId", html)
        self.assertIn('payload.error === "version_conflict"', html)
        self.assertNotIn("Number(payload.version)", html)
        self.assertNotIn("Number(payload.requestId)", html)

    def test_secret_fields_never_create_an_editor(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        secret_branch = fields_source.index("if (field.secret === true)")
        editable_branch = fields_source.index(
            "} else if (editable && field.writable === true)",
            secret_branch,
        )
        self.assertLess(secret_branch, editable_branch)
        self.assertIn('entry.configured === true ? "已配置" : "未配置"', fields_source)

    def test_enum_submission_restores_original_json_value(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        self.assertIn("jsonValue: option.value", fields_source)
        self.assertIn("return selected.jsonValue;", fields_source)

    def test_file_picker_is_composed_only_with_files_module(self):
        settings_only = build_html.assemble_html(INDEX_TEMPLATE, ("settings",))
        files_and_settings = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("files", "settings"),
        )
        self.assertNotIn("function createFileFieldPicker", settings_only)
        self.assertIn("function createFileFieldPicker", files_and_settings)
        self.assertIn("createFieldPicker: createFileFieldPicker", files_and_settings)
        self.assertIn("input.readOnly = true", files_and_settings)
        self.assertIn("item.name.toLocaleLowerCase().endsWith(suffix)", files_and_settings)
        self.assertIn("当前文件在 SD 卡中不存在", files_and_settings)


if __name__ == "__main__":
    unittest.main()
