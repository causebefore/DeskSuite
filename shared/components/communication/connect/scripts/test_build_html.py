"""验证配网 HTML 生成脚本的资源契约。"""

import gzip
import pathlib
import unittest

import build_html


COMPONENT_DIR = pathlib.Path(__file__).resolve().parent.parent


class BuildHtmlTest(unittest.TestCase):
    """覆盖构建配置注入、页面接口和确定性压缩。"""

    def test_index_preserves_portal_contract(self) -> None:
        index_html = build_html.load_index_html(
            COMPONENT_DIR / "web" / "index.html",
            'https://example.test/a?x=1&y="quoted"',
        )
        minified = build_html.minify_html(index_html)

        self.assertNotIn(build_html.DEFAULT_SERVICE_URL_PLACEHOLDER, minified)
        self.assertIn(
            'value="https://example.test/a?x=1&amp;y=&quot;quoted&quot;"',
            minified,
        )
        for required in (
            'action="/save"',
            'name="ssid"',
            'name="pass"',
            'name="service"',
            'name="token"',
            'fetch("/scan/start")',
            'fetch("/scan")',
            'fetch("/activity"',
        ):
            with self.subTest(required=required):
                self.assertIn(required, minified)

    def test_success_preserves_status_contract(self) -> None:
        success_html = (COMPONENT_DIR / "web" / "success.html").read_text(encoding="utf-8")
        minified = build_html.minify_html(success_html)

        for required in ('fetch("/status")', 'href="/"', 'id="status"'):
            with self.subTest(required=required):
                self.assertIn(required, minified)

    def test_compression_is_deterministic_and_reversible(self) -> None:
        source = "<!doctype html><html><body>配网</body></html>"
        first = build_html.compress_html(source)
        second = build_html.compress_html(source)

        self.assertEqual(first, second)
        self.assertEqual(build_html.minify_html(source), gzip.decompress(first).decode("utf-8"))

    def test_generated_source_exports_both_pages(self) -> None:
        generated = build_html.render_c_source("<html>index</html>", "<html>success</html>")

        self.assertEqual(1, generated.count("connect_portal_index_gzip_size_bytes"))
        self.assertEqual(1, generated.count("connect_portal_success_gzip_size_bytes"))


if __name__ == "__main__":
    unittest.main()
