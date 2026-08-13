"""验证网页控制台构建期模块裁剪与确定性输出。"""

import itertools
import json
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
MODULE_CODE_MARKERS = {
    "settings": (
        "function saveActive(",
        "function pollSettings(",
        "refreshBaselinePreservingDraft",
        "preservedDrafts",
    ),
    "actions": (
        "function submitAction(",
        "function pollAction(",
        "actionFailureState",
        "preservedActionInputs",
    ),
}


class BuildHtmlTests(unittest.TestCase):
    def run_node(self, source):
        result = subprocess.run(
            ("node", "-"),
            check=False,
            capture_output=True,
            input=source,
            encoding="utf-8",
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
                for module, markers in MODULE_CODE_MARKERS.items():
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
        baseline_html = build_html.assemble_html(INDEX_TEMPLATE, ("files", "settings", "status"))
        full_html = build_html.assemble_html(INDEX_TEMPLATE, build_html.MODULE_ORDER)
        baseline_size = len(build_html.compress_html(baseline_html))
        full_size = len(build_html.compress_html(full_html))
        self.assertLessEqual(full_size, baseline_size * 1.25)

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
            "terminal_failed",
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
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        self.assertIn("mergeSectionCapabilities", html)
        self.assertIn(
            'description.textContent = section.description || "查看详情";',
            fields_source,
        )
        self.assertNotIn("field.summary", fields_source)
        self.assertNotIn("renderSectionSummary", fields_source)
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
        settings_source = (WEB_ROOT / "modules" / "settings.js").read_text(encoding="utf-8")
        self.assertIn("  function discardActiveDraft()", settings_source)
        self.assertIn("  function handleLeaveResult(", settings_source)
        discard_start = settings_source.index("  function discardActiveDraft()")
        discard_end = settings_source.index("\n  function ", discard_start + 3)
        handle_start = settings_source.index("  function handleLeaveResult(")
        handle_end = settings_source.index("\n  function ", handle_start + 3)
        functions = settings_source[discard_start:discard_end] + settings_source[handle_start:handle_end]
        output = self.run_node(f'''\
const LEAVE_RESULT = Object.freeze({{ proceed: "proceed", save: "save", discard: "discard", cancel: "cancel" }});
const preservedDrafts = new Map();
let activeSectionId = "customer-section";
let settingsRenderer = {{ hasChanges: () => true }};
{functions}
function discardThenReenter(route) {{
  preservedDrafts.set(activeSectionId, [{{ id: "duration", value: "20" }}]);
  settingsRenderer = {{ hasChanges: () => true }};
  const mayLeave = handleLeaveResult(LEAVE_RESULT.discard);
  return {{ route, mayLeave, rendererCleared: settingsRenderer === null,
    restored: preservedDrafts.get(activeSectionId) || null }};
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
        common_source = (WEB_ROOT / "common.js").read_text(encoding="utf-8")
        self.assertIn("fields.handleLeaveResult(await fields.confirmLeave(trigger))", common_source)

    def test_discard_unmount_remount_returns_to_clean_home_dom(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        output = self.run_node(f'''\
class ClassList {{
  constructor() {{ this.values = new Set(); }}
  add(...names) {{ names.forEach((name) => this.values.add(name)); }}
  remove(...names) {{ names.forEach((name) => this.values.delete(name)); }}
  contains(name) {{ return this.values.has(name); }}
  toggle(name, force) {{
    const enabled = force === undefined ? !this.contains(name) : force;
    if (enabled) this.add(name); else this.remove(name);
    return enabled;
  }}
}}
class Element {{
  constructor(id = "") {{
    this.id = id; this.children = []; this.listeners = {{}}; this.classList = new ClassList();
    this.dataset = {{}}; this.textContent = ""; this.attributes = {{}};
  }}
  append(...items) {{ this.children.push(...items); }}
  prepend(...items) {{ this.children.unshift(...items); }}
  replaceChildren(...items) {{ this.children = [...items]; }}
  addEventListener(type, handler) {{ (this.listeners[type] ||= []).push(handler); }}
  async emit(type, event = {{}}) {{
    for (const handler of this.listeners[type] || []) await handler({{
      preventDefault() {{}}, key: "", state: null, ...event,
    }});
  }}
  setAttribute(name, value) {{ this.attributes[name] = String(value); }}
  removeAttribute(name) {{ delete this.attributes[name]; }}
  focus() {{ document.activeElement = this; }}
  querySelector() {{ return null; }}
  querySelectorAll() {{ return []; }}
}}
const ids = ["settingsCenter", "settingsHome", "settingsDetail", "settingsDetailTitle",
  "settingsDetailDescription", "settingsSectionList", "settingsHomeEmpty", "settingsLiveMessage",
  "settingsRetry", "pendingResultCheck", "settingsBack", "settingsModuleHost", "statusModuleHost",
  "actionsModuleHost", "settingsModule", "oldDetail"];
const elements = Object.fromEntries(ids.map((id) => [id, new Element(id)]));
elements.settingsCenter.classList.add("hidden"); elements.settingsDetail.classList.add("hidden");
const document = {{
  activeElement: null,
  getElementById(id) {{ return elements[id] || null; }},
  createElement() {{ return new Element(); }},
  querySelectorAll() {{ return []; }},
}};
const window = {{
  webConsole: {{ setNotice(target, text) {{ target.textContent = text; }} }},
  addEventListener() {{}}, clearTimeout() {{}}, setTimeout() {{ return 1; }},
}};
const history = {{ pushState() {{}}, replaceState() {{}} }};
{fields_source}
let discarded = false;
const controller = {{
  async loadDetail() {{ elements.oldDetail.textContent = "旧详情"; return null; }},
  confirmLeave() {{ return Promise.resolve("discard"); }},
  handleLeaveResult(result) {{ discarded = result === "discard"; return true; }},
  unmount() {{ elements.oldDetail.textContent = ""; }},
}};
const capability = {{ sections: [{{ id: "customer", label: "客户分组", description: "描述", fields: [] }}] }};
(async () => {{
window.webConsole.fields.mountModule("settings", capability, {{}}, controller);
await elements.settingsSectionList.children[0].emit("click");
window.webConsole.fields.handleLeaveResult("discard");
window.webConsole.fields.unmountModule("settings");
window.webConsole.fields.mountModule("settings", capability, {{}}, controller);
console.log(JSON.stringify({{
  discarded,
  homeVisible: !elements.settingsHome.classList.contains("hidden"),
  detailHidden: elements.settingsDetail.classList.contains("hidden"),
  oldDetail: elements.oldDetail.textContent,
}}));
}})().catch((error) => {{ console.error(error); process.exitCode = 1; }});
''')
        self.assertEqual(
            output,
            '{"discarded":true,"homeVisible":true,"detailHidden":true,"oldDetail":""}',
        )

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
        actions_source = (WEB_ROOT / "modules" / "actions.js").read_text(encoding="utf-8")
        self.assertIn("  function actionFailureState(", actions_source)
        reason_start = actions_source.index("  function reasonState(")
        reason_end = actions_source.index("\n  function ", reason_start + 3)
        reason_source = actions_source[reason_start:reason_end]
        start = actions_source.index("  function actionFailureState(")
        end = actions_source.index("\n  function ", start + 3)
        function_source = actions_source[start:end]
        output = self.run_node(f'''\
const PAGE_STATE = Object.freeze({{
  sessionExpired: "session_expired", validationError: "validation_error",
  ownerBusy: "owner_busy", terminalFailed: "terminal_failed", networkUnknown: "network_unknown",
}});
{reason_source}
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
        submit_start = actions_source.index("  async function submitAction(")
        submit_end = actions_source.index("\n  async function pollAction", submit_start)
        submit_source = actions_source[submit_start:submit_end]
        self.assertLess(submit_source.index("preservedActionInputs.set"), submit_source.index("adapter.submit"))
        self.assertIn("renderer.setErrors(error.payload && error.payload.errors)", submit_source)
        self.assertNotIn("sessionStorage", submit_source)
        self.assertNotIn("submitAction(section", submit_source[submit_source.index("catch (error)"):])

    def test_known_action_result_reasons_are_terminal_customer_failures(self):
        actions_source = (WEB_ROOT / "modules" / "actions.js").read_text(encoding="utf-8")
        reason_state_start = actions_source.index("  function reasonState(")
        reason_state_end = actions_source.index("\n  function ", reason_state_start + 3)
        reason_message_start = actions_source.index("  function reasonMessage(")
        reason_message_end = actions_source.index("\n  function ", reason_message_start + 3)
        output = self.run_node(f'''\
const PAGE_STATE = Object.freeze({{
  validationError: "validation_error", ownerBusy: "owner_busy",
  terminalFailed: "terminal_failed", networkUnknown: "network_unknown",
}});
{actions_source[reason_state_start:reason_state_end]}
{actions_source[reason_message_start:reason_message_end]}
const reasons = ["connection_failed", "health_check_failed", "timeout",
  "version_conflict", "persistence_failed", "unknown"];
console.log(JSON.stringify(reasons.map((reason) => [
  reasonState(reason), reasonMessage(reason),
])));
''')
        self.assertEqual(
            output,
            '[["terminal_failed","无法连接 Hub，请检查地址与局域网。"],'
            '["terminal_failed","Hub 健康检查未通过。"],'
            '["terminal_failed","连接 Hub 超时。"],'
            '["terminal_failed","设备状态已变化，本次操作未执行。"],'
            '["terminal_failed","设备未能保存操作结果。"],'
            '["terminal_failed","操作已失败，设备未提供更具体原因。"]]',
        )
        poll_start = actions_source.index("  async function pollAction(")
        poll_end = actions_source.index("\n  function ", poll_start + 3)
        poll_source = actions_source[poll_start:poll_end]
        self.assertLess(
            poll_source.index("clearPendingFact()"),
            poll_source.index("payload.state === \"succeeded\""),
        )
        self.assertNotIn("查询原请求", poll_source[poll_source.index("clearPendingFact()"):])

    def test_settings_known_terminal_reasons_keep_draft_without_network_unknown(self):
        settings_source = (WEB_ROOT / "modules" / "settings.js").read_text(encoding="utf-8")
        reason_state_start = settings_source.index("  function reasonState(")
        reason_state_end = settings_source.index("\n  function ", reason_state_start + 3)
        reason_message_start = settings_source.index("  function reasonMessage(")
        reason_message_end = settings_source.index("\n  function ", reason_message_start + 3)
        output = self.run_node(f'''\
const PAGE_STATE = Object.freeze({{
  validationError: "validation_error", ownerBusy: "owner_busy",
  versionConflict: "version_conflict", terminalFailed: "terminal_failed",
  networkUnknown: "network_unknown",
}});
{settings_source[reason_state_start:reason_state_end]}
{settings_source[reason_message_start:reason_message_end]}
const reasons = ["persistence_failed", "connection_failed", "health_check_failed",
  "timeout", "unknown"];
console.log(JSON.stringify(reasons.map((reason) => [
  reasonState(reason), reasonMessage(reason),
])));
''')
        self.assertEqual(
            output,
            '[["terminal_failed","设备未能保存更改，原设置保持不变。"],'
            '["terminal_failed","设备无法连接 Hub，原设置保持不变。"],'
            '["terminal_failed","Hub 健康检查未通过，原设置保持不变。"],'
            '["terminal_failed","连接 Hub 超时，原设置保持不变。"],'
            '["terminal_failed","设备未能应用更改，原设置保持不变。"]]',
        )

    def test_save_and_leave_failures_restore_real_dialog_without_navigation_or_resubmit(self):
        settings_source = (WEB_ROOT / "modules" / "settings.js").read_text(encoding="utf-8")
        script = r'''
class FakeElement {
  constructor(id) {
    this.id = id;
    this.disabled = false;
    this.listeners = {};
    this.children = [];
    const classes = new Set(["hidden"]);
    this.classList = {
      add: (name) => classes.add(name),
      remove: (name) => classes.delete(name),
      contains: (name) => classes.has(name),
    };
  }
  addEventListener(type, listener) {
    (this.listeners[type] ||= []).push(listener);
  }
  async emit(type, event = {}) {
    for (const listener of this.listeners[type] || []) {
      await listener({ preventDefault() {}, ...event });
    }
  }
  focus() { document.activeElement = this; }
  querySelectorAll(selector) {
    if (selector === "button" || selector === ".confirm-actions button") return [...this.children];
    if (selector === "button:not(:disabled)") {
      return this.children.filter((button) => !button.disabled);
    }
    throw new Error(`unexpected selector: ${selector}`);
  }
  replaceChildren() {}
}

const ids = [
  "settingsForm", "settingsUndo", "settingsSave", "settingsChangeCount",
  "settingsFields", "settingsFieldsRegion", "settingsControls", "draftDialog",
  "draftSaveLeave", "draftDiscardLeave", "draftCancelLeave",
];
const elements = Object.fromEntries(ids.map((id) => [id, new FakeElement(id)]));
const dialogButtons = [
  elements.draftSaveLeave, elements.draftDiscardLeave, elements.draftCancelLeave,
];
elements.draftDialog.children = dialogButtons;
const document = {
  activeElement: null,
  getElementById(id) { return elements[id]; },
};
const storage = new Map();
const sessionStorage = {
  getItem(key) { return storage.has(key) ? storage.get(key) : null; },
  setItem(key, value) { storage.set(key, value); },
  removeItem(key) { storage.delete(key); },
};

let mode = "submit_failed";
let submitCalls = 0;
let queryCalls = 0;
let fieldErrors = null;
let registration;
let controller;
function response(status, payload) {
  return {
    ok: status >= 200 && status < 300,
    status,
    async json() { return payload; },
  };
}
const consoleApi = {
  async apiFetch(url, options = {}) {
    if (url.includes("/result")) {
      queryCalls += 1;
      if (mode === "validation_failed") {
        return response(200, {
          state: "failed",
          reason: "validation_failed",
          message: "字段校验失败。",
          errors: [{ id: "duration", message: "时长超出范围。" }],
        });
      }
      throw new Error("network unavailable");
    }
    if (options.method === "PATCH") {
      submitCalls += 1;
      if (mode === "submit_failed") return response(500, { message: "提交失败" });
      return response(202, { state: "pending", reason: "none", requestId: "7" });
    }
    return response(200, { section: "customer", version: "1", values: [] });
  },
  fields: {
    mountModule(_kind, _capability, _adapter, value) { controller = value; },
    unmountModule() {},
  },
  getToken() { return "token"; },
  async readApiError() { return "读取失败"; },
  registerModule(value) { registration = value; },
};
const window = {
  webConsole: consoleApi,
  clearTimeout() {},
  setTimeout() { throw new Error("unexpected retry timer"); },
};
''' + f"\nconst settingsSource = {json.dumps(settings_source)};\neval(settingsSource);\n" + r'''

const renderer = {
  applyDraft() {},
  changeCount() { return 1; },
  exportDraft() { return [{ id: "duration", value: 2 }]; },
  hasChanges() { return true; },
  readChanges() { return [{ id: "duration", value: 2 }]; },
  setDisabled() {},
  setErrors(errors) { fieldErrors = errors; },
};
let state = "dirty";
let pageMessage = "";
const view = {
  getPageState() { return state; },
  isCurrent() { return true; },
  reload() {},
  renderFields() { return renderer; },
  setPageState(next, message = "") { state = next; pageMessage = message; },
  setPendingCheck() {},
};
const part = { id: "customer", fields: [] };
const context = { section: { id: "customer" }, options: {}, view };

async function runScenario(nextMode) {
  controller.resetDetail();
  storage.clear();
  mode = nextMode;
  state = "dirty";
  pageMessage = "";
  submitCalls = 0;
  queryCalls = 0;
  fieldErrors = null;
  for (const button of dialogButtons) button.disabled = false;
  elements.draftDialog.classList.add("hidden");
  await controller.loadDetail(part, context);
  let navigated = 0;
  const leave = controller.confirmLeave(elements.settingsUndo).then((result) => {
    if (controller.handleLeaveResult(result)) navigated += 1;
    return result;
  });
  await elements.draftSaveLeave.emit("click");
  await Promise.resolve();
  await Promise.resolve();
  const result = {
    state,
    pageMessage,
    fieldErrors,
    draftRetained: controller.hasUnsaved(),
    enabled: dialogButtons.map((button) => !button.disabled),
    operable: !elements.draftDialog.classList.contains("hidden"),
    navigated,
    submitCalls,
    queryCalls,
  };
  await elements.draftCancelLeave.emit("click");
  await leave;
  return result;
}

(async () => {
  await registration.mount({});
  console.log(JSON.stringify([
    await runScenario("submit_failed"),
    await runScenario("network_unknown"),
    await runScenario("validation_failed"),
  ]));
})().catch((error) => { console.error(error); process.exitCode = 1; });
'''
        output = self.run_node(script)
        self.assertEqual(
            output,
            '[{"state":"terminal_failed","pageMessage":"提交失败",'
            '"fieldErrors":null,"draftRetained":true,"enabled":[true,true,true],"operable":true,'
            '"navigated":0,"submitCalls":1,"queryCalls":0},'
            '{"state":"network_unknown","pageMessage":"保存结果暂时未知，请查询原请求，勿重复提交。",'
            '"fieldErrors":null,"draftRetained":true,"enabled":[true,true,true],"operable":true,'
            '"navigated":0,"submitCalls":1,"queryCalls":1},'
            '{"state":"validation_error","pageMessage":"设备未接受这些设置，请检查标记字段。",'
            '"fieldErrors":[{"id":"duration","message":"时长超出范围。"}],'
            '"draftRetained":true,"enabled":[true,true,true],"operable":true,'
            '"navigated":0,"submitCalls":1,"queryCalls":1}]',
        )

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

    def test_effect_tokens_and_duration_are_customer_facing(self):
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        self.assertIn('field.effect !== "none"', fields_source)
        effect_start = fields_source.index("  function effectLabel(")
        effect_end = fields_source.index("\n  function ", effect_start + 3)
        format_start = fields_source.index("  function formatDisplayValue(")
        format_end = fields_source.index("\n  function ", format_start + 3)
        output = self.run_node(f'''\
const hasOwn = (object, key) => object !== null && object !== undefined &&
  Object.prototype.hasOwnProperty.call(object, key);
function optionParts(option) {{ return {{ value: String(option.value), label: String(option.label) }}; }}
{fields_source[effect_start:effect_end]}
{fields_source[format_start:format_end]}
const effects = ["immediate", "next_transaction", "reconnect", "restart", "idle_only"]
  .map(effectLabel);
const duration = formatDisplayValue(
  {{ id: "uptime_sec", label: "运行时长", type: "uint32", format: "duration_seconds" }},
  {{ id: "uptime_sec", value: 3660 }},
);
console.log(JSON.stringify({{ effects, duration }}));
''')
        self.assertEqual(
            output,
            '{"effects":["立即生效","下一次使用时生效","重新连接后生效",'
            '"设备重启后生效","仅空闲时可修改"],"duration":"1 小时 1 分"}',
        )

    def test_settings_home_is_static_and_detail_reads_are_bounded(self):
        html = build_html.assemble_html(INDEX_TEMPLATE, ("settings", "status"))
        fields_source = (WEB_ROOT / "modules" / "fields.js").read_text(encoding="utf-8")
        settings_source = (WEB_ROOT / "modules" / "settings.js").read_text(encoding="utf-8")
        status_source = (WEB_ROOT / "modules" / "status.js").read_text(encoding="utf-8")
        self.assertNotIn("renderSectionSummary", fields_source)
        self.assertNotIn("getSummary", settings_source)
        self.assertNotIn("getSummary", status_source)
        self.assertNotIn('summary.textContent = "正在读取…"', fields_source)
        self.assertIn("new AbortController()", fields_source)
        self.assertIn("signal: requestController.signal", fields_source)
        self.assertIn("const SNAPSHOT_TIMEOUT_MS = 8000", settings_source)
        self.assertIn("const SNAPSHOT_TIMEOUT_MS = 8000", status_source)
        self.assertIn("timeoutMs: SNAPSHOT_TIMEOUT_MS", settings_source)
        self.assertIn("timeoutMs: SNAPSHOT_TIMEOUT_MS", status_source)
        self.assertIn('id="settingsRetry"', html)
        self.assertIn("读取设备数据超时，请重试。", html)

    def test_api_fetch_distinguishes_timeout_from_caller_cancel(self):
        common_source = (WEB_ROOT / "common.js").read_text(encoding="utf-8")
        start = common_source.index("  async function apiFetch(")
        end = common_source.index("\n  function validateCapabilities", start)
        api_fetch_source = common_source[start:end]
        output = self.run_node(f'''\
function token() {{ return "session-token"; }}
function showLogin() {{}}
const window = globalThis;
const requestSignals = [];
globalThis.fetch = (_url, options) => new Promise((_resolve, reject) => {{
  requestSignals.push(options.signal);
  const abort = () => {{
    const error = new Error("aborted");
    error.name = "AbortError";
    reject(error);
  }};
  if (options.signal.aborted) abort();
  else options.signal.addEventListener("abort", abort, {{ once: true }});
}});
{api_fetch_source}
(async () => {{
  let timeoutMessage = "";
  try {{
    await apiFetch("/api/settings", {{ timeoutMs: 5 }});
  }} catch (error) {{
    timeoutMessage = error.message;
  }}
  const caller = new AbortController();
  window.setTimeout(() => caller.abort(), 5);
  let callerError = "";
  try {{
    await apiFetch("/api/status", {{ timeoutMs: 1000, signal: caller.signal }});
  }} catch (error) {{
    callerError = error.name;
  }}
  console.log(JSON.stringify({{
    timeoutMessage,
    callerError,
    timeoutAborted: requestSignals[0].aborted,
    callerAborted: requestSignals[1].aborted,
  }}));
}})();
''')
        self.assertEqual(
            output,
            '{"timeoutMessage":"读取设备数据超时，请重试。",'
            '"callerError":"AbortError","timeoutAborted":true,"callerAborted":true}',
        )

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
        self.assertIn('.sample{content:"a > b";color:red}', result)
        self.assertIn('const text="a  b";', result)
        self.assertIn("const template=`first\n    second  `;", result)
        self.assertNotIn("构建产物不保留整行注释", result)
        self.assertIn("<pre>  keep\nspace  </pre>", result)


if __name__ == "__main__":
    unittest.main()
