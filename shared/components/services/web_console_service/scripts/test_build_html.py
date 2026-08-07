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
        "保存并离开",
        "保存成功，已重新读取设备设置。",
        "保存结果暂时未知",
        "本地更改尚未保存",
    ),
    "status": (
        'data-web-console-module="status"',
        "/api/status",
        "刷新状态",
        "当前状态",
    ),
    "actions": (
        'data-web-console-module="actions"',
        "/api/actions",
        "可用操作",
        "操作结果暂时未知，请勿重复提交。",
        "操作已完成。",
        "正在执行“",
    ),
}


class BuildHtmlTests(unittest.TestCase):
    def run_node(self, source):
        result = subprocess.run(
            ("node", "-e", source),
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

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

                needs_fields = bool({"settings", "status", "actions"}.intersection(enabled))
                self.assertEqual("window.webConsole.fields" in html, needs_fields)

    def test_core_only_contains_no_module_contract_or_copy(self):
        html = build_html.minify_html(build_html.assemble_html(INDEX_TEMPLATE, ()))
        for markers in MODULE_MARKERS.values():
            for marker in markers:
                self.assertNotIn(marker, html)
        self.assertNotIn("Crosslink", html)
        self.assertNotIn("设备设置", html)
        self.assertNotIn("设备状态", html)
        self.assertNotIn("文件管理", html)

    def test_module_order_is_fixed_independent_of_arguments(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            tuple(reversed(build_html.MODULE_ORDER)),
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

    def test_actions_module_can_be_assembled(self):
        try:
            html = build_html.assemble_html(
                INDEX_TEMPLATE,
                ("files", "settings", "status", "actions"),
            )
        except ValueError as error:
            self.fail(f"Actions 模块必须可组装：{error}")
        self.assertIn('data-web-console-module="actions"', html)
        self.assertIn("/api/actions", html)

    def test_management_navigation_renders_only_files_settings_and_logout(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("files", "settings", "status"),
        )
        navigation_descriptors = re.findall(
            r'navigation: \{ id: "([^"]+)", label: "([^"]+)" \}',
            html,
        )
        self.assertEqual(
            navigation_descriptors,
            [
                ("files", "文件管理"),
                ("settings", "设置"),
            ],
        )
        self.assertEqual(html.count('id="logoutButton"'), 1)
        self.assertIn(">退出登录<", html)
        self.assertNotIn("设备管理", html)
        self.assertNotIn('navigation: { id: "management"', html)

    def test_actions_keep_the_same_top_level_navigation(self):
        try:
            html = build_html.assemble_html(
                INDEX_TEMPLATE,
                ("files", "settings", "status", "actions"),
            )
        except ValueError as error:
            self.fail(f"Actions 启用态必须保持唯一顶部导航：{error}")
        navigation_descriptors = re.findall(
            r'navigation: \{ id: "([^"]+)", label: "([^"]+)" \}',
            html,
        )
        self.assertEqual(
            navigation_descriptors,
            [
                ("files", "文件管理"),
                ("settings", "设置"),
            ],
        )
        self.assertEqual(html.count('id="logoutButton"'), 1)

    def test_settings_home_has_only_customer_groups(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("files", "settings", "status"),
        )
        self.assertIn('id="settingsHome"', html)
        self.assertNotIn("配置分区", html)
        self.assertNotIn("状态分区", html)

    def test_actions_resources_are_trimmed_when_actions_are_disabled(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("files", "settings", "status"),
        )
        for marker in MODULE_MARKERS["actions"]:
            self.assertNotIn(marker, html)
        for relative_path in (
            "modules/actions.html",
            "modules/actions.css",
            "modules/actions.js",
        ):
            path = WEB_ROOT / relative_path
            if path.exists():
                fragment = path.read_text(encoding="utf-8").strip()
                self.assertTrue(fragment, f"{relative_path} 片段不能为空")
                self.assertNotIn(fragment, html)

    def test_assembled_page_has_unique_ids_and_no_placeholders(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            build_html.MODULE_ORDER,
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
            build_html.MODULE_ORDER,
        )
        first = build_html.compress_html(html)
        second = build_html.compress_html(html)
        self.assertEqual(first, second)
        self.assertEqual(first[0:3], b"\x1f\x8b\x08")
        self.assertEqual(first[4:8], b"\x00\x00\x00\x00")

    def test_full_module_gzip_stays_within_review_budget(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, build_html.MODULE_ORDER)
        self.assertLessEqual(len(build_html.compress_html(html)), 23288)

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

    def test_settings_center_exposes_complete_page_state_machine(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("settings", "status", "actions"),
        )
        for state in (
            "loading",
            "synced",
            "dirty",
            "saving",
            "succeeded",
            "validation_error",
            "owner_busy",
            "network_unknown",
            "version_conflict",
            "session_expired",
        ):
            self.assertIn(f'"{state}"', html)
        self.assertIn("PAGE_STATE", html)
        self.assertIn("refreshBaselinePreservingDraft", html)
        self.assertIn("reload authoritative snapshot", html)

    def test_sections_are_merged_from_metadata_without_product_id_branches(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("settings", "status", "actions"),
        )
        self.assertIn("mergeSectionCapabilities", html)
        self.assertIn("field.summary", html)
        self.assertIn("field.unit", html)
        self.assertNotRegex(
            html,
            r'(?:===|!==|case\s+)[ ]*["\'](?:hub|pomodoro|system)["\']',
        )

    def test_number_fields_use_accessible_input_stepper(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("settings",))
        self.assertIn('input.type = "number"', html)
        self.assertIn('minus.textContent = "−"', html)
        self.assertIn('plus.textContent = "+"', html)
        self.assertIn("label.htmlFor = input.id", html)
        self.assertIn('input.setAttribute("aria-describedby"', html)

    def test_dirty_navigation_uses_three_choice_dialog_and_beforeunload(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("files", "settings"))
        for marker in (
            'id="draftDialog"',
            ">保存并离开<",
            ">放弃更改<",
            ">取消<",
            'addEventListener("beforeunload"',
            'event.key === "Escape"',
            "focusableElements",
        ):
            self.assertIn(marker, html)

    def test_discard_clears_renderer_and_preserved_draft_before_leaving(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        self.assertIn("  function discardActiveDraft()", fields_source)
        self.assertIn("  function handleLeaveResult(", fields_source)
        discard_start = fields_source.index("  function discardActiveDraft()")
        discard_end = fields_source.index("\n  function ", discard_start + 3)
        handle_start = fields_source.index("  function handleLeaveResult(")
        handle_end = fields_source.index("\n  function ", handle_start + 3)
        functions = fields_source[discard_start:discard_end] + fields_source[handle_start:handle_end]
        output = self.run_node(f'''\
const LEAVE_RESULT = Object.freeze({{ proceed: "proceed", save: "save", discard: "discard", cancel: "cancel" }});
const preservedDrafts = new Map();
let activeSection = {{ id: "customer-section" }};
let settingsRenderer = {{ hasChanges: () => true }};
{functions}
function discardThenReenter(route) {{
  preservedDrafts.set(activeSection.id, [{{ id: "duration", value: "20" }}]);
  settingsRenderer = {{ hasChanges: () => true }};
  const mayLeave = handleLeaveResult(LEAVE_RESULT.discard);
  return {{ route, mayLeave, rendererCleared: settingsRenderer === null,
    restored: preservedDrafts.get(activeSection.id) || null }};
}}
console.log(JSON.stringify([
  discardThenReenter("home-settings"),
  discardThenReenter("files-settings"),
]));
''')
        self.assertEqual(
            output,
            '[{"route":"home-settings","mayLeave":true,"rendererCleared":true,"restored":null},'
            '{"route":"files-settings","mayLeave":true,"rendererCleared":true,"restored":null}]',
        )
        return_home = fields_source[
            fields_source.index("  async function requestReturnHome()"):
            fields_source.index("\n  function mountModule", fields_source.index("  async function requestReturnHome()"))
        ]
        self.assertLess(return_home.index("handleLeaveResult"), return_home.index("showHome()"))
        common_source = (WEB_ROOT / "common.js").read_text(encoding="utf-8")
        self.assertIn("fields.handleLeaveResult(await fields.confirmLeave(trigger))", common_source)

    def test_generated_description_ids_are_unique_for_consecutive_read_only_fields(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        start = fields_source.index("  function appendFieldHeading(")
        end = fields_source.index("\n  function renderFields", start)
        function_source = fields_source[start:end]
        output = self.run_node(f'''\
let descriptionSequence = 0;
const document = {{
  createElement(tag) {{
    return {{ tag, children: [], className: "", textContent: "", id: "",
      append(...items) {{ this.children.push(...items); }},
      setAttribute() {{}} }};
  }},
}};
function row() {{ return {{ children: [], prepend(...items) {{ this.children.unshift(...items); }} }}; }}
{function_source}
const first = row();
const second = row();
appendFieldHeading(first, {{ label: "只读一", description: "说明一" }}, null);
appendFieldHeading(second, {{ label: "密钥二", description: "说明二", secret: true }}, null);
console.log(JSON.stringify([first.children[0].children[1].id, second.children[0].children[1].id]));
''')
        self.assertEqual(
            output,
            '["console-field-description-1","console-field-description-2"]',
        )

    def test_action_submit_failure_states_are_distinct_and_inputs_stay_in_memory(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        self.assertIn("  function actionFailureState(", fields_source)
        start = fields_source.index("  function actionFailureState(")
        end = fields_source.index("\n  function ", start + 3)
        function_source = fields_source[start:end]
        output = self.run_node(f'''\
const PAGE_STATE = Object.freeze({{
  sessionExpired: "session_expired", validationError: "validation_error",
  ownerBusy: "owner_busy", networkUnknown: "network_unknown",
}});
{function_source}
console.log(JSON.stringify([
  actionFailureState({{ status: 401 }}, false),
  actionFailureState({{ status: 422 }}, true),
  actionFailureState({{ status: 409, payload: {{ error: "owner_busy" }} }}, true),
  actionFailureState({{}}, true),
]));
''')
        self.assertEqual(
            output,
            '["session_expired","validation_error","owner_busy","network_unknown"]',
        )
        submit_start = fields_source.index("  async function submitAction(")
        submit_end = fields_source.index("\n  async function pollAction", submit_start)
        submit_source = fields_source[submit_start:submit_end]
        self.assertLess(submit_source.index("preservedActionInputs.set"), submit_source.index("adapter.submit"))
        self.assertIn("renderer.setErrors(error.payload && error.payload.errors)", submit_source)
        self.assertNotIn("sessionStorage", submit_source)
        self.assertNotIn("submitAction(section", submit_source[submit_source.index("catch (error)"):])

    def test_polling_is_bounded_and_does_not_persist_secret_payloads(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("settings", "actions"),
        )
        self.assertIn("POLL_DEADLINE_MS", html)
        self.assertIn("fact.start", html)
        self.assertIn("PAGE_STATE.networkUnknown", html)
        self.assertIn("PENDING_FACT_KEYS", html)
        pending_writes = re.findall(
            r'sessionStorage\.setItem\(PENDING_STORAGE_KEY,[\s\S]*?\);',
            html,
        )
        self.assertTrue(pending_writes)
        for write in pending_writes:
            self.assertNotRegex(write, r"inputs|changes|draft|url|token")
        self.assertNotIn("schedulePoll(() => submit", html)

    def test_actions_render_and_submit_only_from_action_metadata(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("actions",))
        for marker in (
            "section.actions",
            "action.inputs",
            'method: "POST"',
            "renderer.readAllValues()",
            "JSON.stringify({ inputs })",
            'const ACTION_RESULT_ENDPOINT = "/api/actions/result"',
            "encodeURIComponent(actionId)",
        ):
            self.assertIn(marker, html)

    def test_settings_layout_is_single_column_responsive_and_reduced_motion_safe(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("settings", "status", "actions"),
        )
        for marker in (
            "max-width: 720px",
            "min-height: 44px",
            "position: sticky",
            "@media (max-width: 390px)",
            "prefers-reduced-motion: reduce",
        ):
            self.assertIn(marker, html)

    def test_each_summary_failure_is_isolated_to_its_section(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("settings", "status"))
        self.assertIn("Promise.allSettled", html)
        self.assertIn("暂时无法读取", html)
        self.assertIn("renderSectionSummary", html)

    def test_unknown_result_can_query_original_request_without_resubmitting(self):
        html = build_html.assemble_html(
            INDEX_TEMPLATE,
            ("settings", "actions"),
        )
        self.assertIn('id="pendingResultCheck"', html)
        self.assertIn("pollSettings(pendingFact, true)", html)
        self.assertIn("pollAction(pendingFact, true)", html)
        self.assertNotIn("submitAction(pendingFact", html)

    def test_minifier_compacts_embedded_code_and_preserves_literal_whitespace(self):
        source = '''<style>
          .sample { content: "a > b"; color: red; }
        </style><script>
          // 构建产物不保留整行注释。
          const text = "a  b";
          const template = `first
    second  `;

          run(text);
        </script><pre>  keep
space  </pre>'''
        result = build_html.minify_html(source)
        self.assertIn('.sample{content:"a > b";color:red;}', result)
        self.assertIn('const text = "a  b";', result)
        self.assertIn("const template = `first\n    second  `;", result)
        self.assertNotIn("构建产物不保留整行注释", result)
        self.assertIn("<pre>  keep\nspace  </pre>", result)


if __name__ == "__main__":
    unittest.main()
