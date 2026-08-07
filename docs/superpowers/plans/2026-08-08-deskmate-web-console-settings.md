# DeskMate Web Console 设置中心 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 DeskMate Web Console 从面向调试的“设备管理”表单改造成面向设备拥有者的设置中心，并完成 Hub URL 安全测试/保存与番茄钟原子持久化。

**Architecture:** 共享 `web_console_service` 只扩展可裁剪的元数据、非破坏性 Actions HTTP 映射和通用设置中心渲染；DeskMate 的 `app_pomodoro` 与 `app_network` 仍是设置、版本、异步请求和持久化结果的唯一所有者。Hub 候选地址由网络 Application Task 串行测试 `/healthz`，保存时只提交单份 `network_cfg` Blob，成功后才公开新 URL 与版本。

**Tech Stack:** ESP-IDF C/C++、FreeRTOS、ESP HTTP Client、cJSON、无框架 HTML/CSS/JavaScript、Python `unittest`、PowerShell 静态契约检查。

## Global Constraints

- 固件只允许从仓库根目录通过 `& .\ds.ps1 build deskmate` 编译；本轮用户未明确要求编译，因此默认只做静态和主机侧验证。
- 共享组件不得硬编码 `pomodoro`、`hub`、`system` 或 DeskMate 产品条件分支。
- Web 页面不得读取、回显、保存或记录访问码、Bearer Token、Wi-Fi 密码和设备 API Token。
- Hub URL 只接受 ASCII `http://`、IPv4 或主机名、可选端口；不得包含用户信息、query、fragment 或业务路径；规范化结果最多 127 字节。
- 番茄钟完成音乐继续使用 Files 提供的 SD 卡 `.mp3` 逻辑路径和 Audio Service 播放链路。
- 设置持久化失败时，运行时值和 version 都必须保持旧值。
- 页面不增加 Wi-Fi、独立声音、OTA、重启、主题、账户或公网能力。
- 所有新增公共 C API 使用中文 Doxygen；新增跨模块术语先登记到受控术语表。
- 只修改和提交本任务文件，保留 `services/hub/` 中既有未提交改动。

---

### Task 1: 固定共享设置中心与 Actions 契约

**Files:**
- Modify: `shared/components/services/web_console_service/scripts/test_build_html.py`
- Modify: `devices/deskmate/tools/tests/check_web_console_product_providers.ps1`
- Create: `devices/deskmate/tools/tests/check_app_network_hub_settings.ps1`
- Test: `shared/components/services/web_console_service/scripts/test_build_html.py`

**Interfaces:**
- Consumes: 当前 `build_html.assemble_html()`、产品 Provider 静态数组和 Application 源码。
- Produces: 能捕获旧“设备管理”导航、无 Actions 裁剪、番茄钟先改内存后写 NVS、Hub 设置缺失的失败验证。

- [ ] **Step 1: 写共享页面失败测试**

```python
def test_management_modules_render_one_settings_center(self):
    html = build_html.assemble_html(
        INDEX_TEMPLATE,
        ("settings", "status", "actions"),
    )
    self.assertIn('navigation: { id: "settings", label: "设置"', html)
    self.assertIn('id="settingsHome"', html)
    self.assertIn("/api/actions", html)
    self.assertNotIn("配置分区", html)
```

- [ ] **Step 2: 写产品所有者失败检查**

```powershell
Assert-InOrder $pomodoroTask @(
    'pomodoro_store_save_settings_copy\s*\(',
    'state->snapshot.settings\s*=\s*update->settings'
) '番茄钟必须先持久化候选，再公开新内存设置'

Assert-Contains $networkTask 'NETWORK_COMMAND_HUB_' '网络 Task 必须拥有 Hub 请求'
Assert-Contains $provider '\.section_id\s*=\s*"hub"' '缺少 Hub 设置 Provider'
```

- [ ] **Step 3: 运行测试并确认按预期失败**

Run: `python -B .\shared\components\services\web_console_service\scripts\test_build_html.py`

Expected: FAIL，原因是模块列表没有 `actions`、页面仍包含“配置分区”。

Run: `& .\devices\deskmate\tools\tests\check_web_console_product_providers.ps1`

Expected: FAIL，原因是番茄钟当前在 NVS 前公开新设置。

Run: `& .\devices\deskmate\tools\tests\check_app_network_hub_settings.ps1`

Expected: FAIL，原因是 Hub URL Application/Provider 契约尚未存在。

### Task 2: 扩展共享 Provider 元数据与非破坏性 Actions

**Files:**
- Modify: `docs/standards/c_cpp_terminology.md`
- Modify: `shared/components/services/web_console_service/include/web_console_provider.h`
- Modify: `shared/components/services/web_console_service/include/web_console_service.h`
- Modify: `shared/components/services/web_console_service/src/providers/web_console_provider_internal.hpp`
- Modify: `shared/components/services/web_console_service/src/providers/web_console_provider_registry.cpp`
- Modify: `shared/components/services/web_console_service/src/providers/web_console_provider_http.cpp`
- Modify: `shared/components/services/web_console_service/src/core/web_console_service_internal.hpp`
- Modify: `shared/components/services/web_console_service/src/core/web_console_service.cpp`
- Modify: `shared/components/services/web_console_service/src/core/web_console_service_http.cpp`
- Modify: `shared/components/services/web_console_service/Kconfig`
- Modify: `shared/components/services/web_console_service/CMakeLists.txt`

**Interfaces:**
- Consumes: 现有 Settings/Status Provider、Core dispatcher、Bearer 认证和 no-store JSON 响应。
- Produces: `web_console_action_provider_t`、`POST /api/actions`、`GET /api/actions/result`、可选说明/单位/摘要/字符串格式元数据和稳定结果原因。

- [ ] **Step 1: 登记受控术语并定义公共契约**

```c
typedef enum {
    WEB_CONSOLE_RESULT_REASON_NONE = 0,
    WEB_CONSOLE_RESULT_REASON_VERSION_CONFLICT,
    WEB_CONSOLE_RESULT_REASON_OWNER_BUSY,
    WEB_CONSOLE_RESULT_REASON_VALIDATION_FAILED,
    WEB_CONSOLE_RESULT_REASON_PERSISTENCE_FAILED,
    WEB_CONSOLE_RESULT_REASON_CONNECTION_FAILED,
    WEB_CONSOLE_RESULT_REASON_HEALTH_CHECK_FAILED,
    WEB_CONSOLE_RESULT_REASON_TIMEOUT,
    WEB_CONSOLE_RESULT_REASON_UNKNOWN,
} web_console_result_reason_t;

typedef struct {
    const char *section_id;
    const char *label;
    const char *description;
    const web_console_action_info_t *actions;
    size_t action_count;
    web_console_action_validate_request_cb_t validate_request;
    web_console_action_request_copy_cb_t request_copy;
    web_console_action_get_result_copy_cb_t get_result_copy;
    void *context;
} web_console_action_provider_t;
```

- [ ] **Step 2: 扩展深复制注册表**

实现 Settings/Status 的 `description`、字段 `description/unit/summary/format` 深复制与校验；新增 Actions section/action/input-field 深复制、重复 ID 和回调契约校验。跨 Provider 类型允许相同 section ID，单个类型内部仍拒绝重复。

- [ ] **Step 3: 增加 Actions HTTP 路由**

```text
POST /api/actions?section=<id>&action=<id>
GET  /api/actions/result?section=<id>&action=<id>&request=<uint64>
```

POST 只解析有界、无重复、无秘密字段的输入并快速提交；GET 把 pending/succeeded/failed 与稳定 reason 编码为 JSON。两条路由复用 Bearer、`Cache-Control: no-store`、handler 记账和停止屏障。

- [ ] **Step 4: 增加构建开关并更新固定路由数量**

```kconfig
config WEB_CONSOLE_ACTIONS
    bool "Enable Web Console Actions module"
    default n
```

关闭开关时不编译 Actions 路由、注册表存储或网页脚本；打开后 Provider 路由总数增加 2。

- [ ] **Step 5: 运行共享测试**

Run: `python -B .\shared\components\services\web_console_service\scripts\test_build_html.py`

Expected: Task 1 的 Actions 裁剪断言继续失败，但 Python 脚本本身无语法错误。

### Task 3: 实现通用响应式设置中心

**Files:**
- Modify: `shared/components/services/web_console_service/web/index.html`
- Modify: `shared/components/services/web_console_service/web/common.css`
- Modify: `shared/components/services/web_console_service/web/common.js`
- Create: `shared/components/services/web_console_service/web/modules/fields.html`
- Modify: `shared/components/services/web_console_service/web/modules/fields.css`
- Modify: `shared/components/services/web_console_service/web/modules/fields.js`
- Modify: `shared/components/services/web_console_service/web/modules/settings.html`
- Modify: `shared/components/services/web_console_service/web/modules/settings.js`
- Modify: `shared/components/services/web_console_service/web/modules/status.html`
- Modify: `shared/components/services/web_console_service/web/modules/status.js`
- Create: `shared/components/services/web_console_service/web/modules/actions.html`
- Create: `shared/components/services/web_console_service/web/modules/actions.js`
- Modify: `shared/components/services/web_console_service/scripts/build_html.py`
- Modify: `shared/components/services/web_console_service/scripts/test_build_html.py`

**Interfaces:**
- Consumes: Capabilities 中按模块独立返回、但可共享 section ID 的 Settings/Status/Actions 元数据。
- Produces: 顶层“文件管理 / 设置”、设置首页、一层详情、草稿/冲突/未知结果状态机、通用步进器、单位格式和有界轮询。

- [ ] **Step 1: 让构建器装配 Actions 与共享设置中心壳**

```python
MODULE_ORDER = ("files", "settings", "status", "actions")
FIELDS_MODULE_USERS = frozenset(("settings", "status", "actions"))
```

所有 16 种组合必须确定性 gzip，并且关闭某模块后其 endpoint、文案和代码标记不进入输出。

- [ ] **Step 2: 实现设置中心导航和 section 合并**

`fields.js` 按 section ID 合并三个能力模块，只显示实际存在的 section。设置首页通用读取摘要字段，单项失败只影响对应行；详情页只进入一层，不按产品 ID 分支。

- [ ] **Step 3: 实现字段和草稿状态机**

```javascript
const PAGE_STATE = Object.freeze({
  loading: "loading",
  synced: "synced",
  dirty: "dirty",
  saving: "saving",
  succeeded: "succeeded",
  validationError: "validation_error",
  ownerBusy: "owner_busy",
  networkUnknown: "network_unknown",
  versionConflict: "version_conflict",
  sessionExpired: "session_expired",
});
```

数字字段使用可输入的 `- / number / +` 步进器；摘要字段使用通用单位格式化。保存成功后重新 GET 权威快照，冲突保留草稿并刷新设备基线。

- [ ] **Step 4: 实现离开保护和有界轮询**

页面内离开使用三按钮确认层；`beforeunload` 使用浏览器原生确认。轮询保存 `section/action/request/start` 等非秘密事实到 `sessionStorage`，总期限到达后进入 `network_unknown`，不自动重复提交。

- [ ] **Step 5: 实现响应式和可访问性**

桌面正文最大宽度 720px；390px 手机保持单列、44px 触摸目标和底部粘性保存栏。所有输入使用真实 label/description，异步结果使用克制的 `aria-live`，确认层管理焦点与 Escape，并支持 `prefers-reduced-motion`。

- [ ] **Step 6: 运行页面测试并确认转绿**

Run: `python -B .\shared\components\services\web_console_service\scripts\test_build_html.py`

Expected: PASS，覆盖 16 种裁剪组合、唯一 ID、确定性 gzip、设置中心文案、Actions endpoint、草稿保护与禁止 `.innerHTML`。

### Task 4: 修正番茄钟原子持久化

**Files:**
- Modify: `devices/deskmate/main/application/app_pomodoro_task.c`
- Modify: `devices/deskmate/main/application/app_pomodoro.h`
- Modify: `devices/deskmate/main/application/README.md`
- Modify: `devices/deskmate/tools/tests/check_web_console_product_providers.ps1`

**Interfaces:**
- Consumes: `pomodoro_store_save_settings_copy()`、单 pending 请求和 `state_lock`。
- Produces: “执行点重检 → 锁外持久化候选 → 短锁内发布设置/version/result”的原子可见性。

- [ ] **Step 1: 保持旧状态并在锁外保存候选**

```c
const app_pomodoro_settings_t candidate = update->settings;
xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
const esp_err_t error = pomodoro_store_save_settings_copy(&stored);
xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
```

- [ ] **Step 2: 只在 NVS 成功后发布设置和版本**

```c
if (error == ESP_OK) {
    state->snapshot.settings = candidate;
    state->snapshot.settings_version++;
    state->snapshot.settings_saved = true;
}
finish_settings_update_locked(
    command->settings_request_id,
    error == ESP_OK ? APP_POMODORO_SETTINGS_UPDATE_STATE_SUCCEEDED
                    : APP_POMODORO_SETTINGS_UPDATE_STATE_FAILED,
    error);
```

失败时保留旧 `settings`、`settings_version` 和原 `settings_saved`，只更新请求失败事实与 `last_error`。

- [ ] **Step 3: 运行产品静态测试**

Run: `& .\devices\deskmate\tools\tests\check_web_console_product_providers.ps1`

Expected: PASS，确认持久化调用位于内存发布之前，并且失败分支不覆盖旧设置/version。

### Task 5: 实现 Hub URL Application 所有者与产品 Providers

**Files:**
- Create: `devices/deskmate/main/application/app_network_hub_url.h`
- Create: `devices/deskmate/main/application/app_network_hub_url.c`
- Modify: `devices/deskmate/main/application/app_network.h`
- Modify: `devices/deskmate/main/application/app_network_task.c`
- Modify: `devices/deskmate/main/application/app_web_console_provider.h`
- Modify: `devices/deskmate/main/application/app_web_console_provider.c`
- Modify: `devices/deskmate/main/application/app_web_console.cpp`
- Modify: `devices/deskmate/main/CMakeLists.txt`
- Modify: `devices/deskmate/sdkconfig.defaults`
- Create: `devices/deskmate/tools/tests/test_app_network_hub_url.c`
- Modify: `devices/deskmate/tools/tests/check_app_network_hub_settings.ps1`

**Interfaces:**
- Consumes: `transport_http_perform_borrow()`、`system_storage_get/set_network_config_*()`、`remote_log_stop/configure/start()`、现有网络 Application queue。
- Produces: `app_network_get_hub_settings_snapshot_copy()`、`app_network_request_test_hub_url_copy()`、`app_network_request_update_hub_url_copy()` 和统一结果查询。

- [ ] **Step 1: 实现纯 C URL 解析/规范化并主机测试**

```c
esp_err_t app_network_hub_url_parse_copy(
    const char *input,
    char out_url[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U]);
```

测试覆盖 scheme/host 小写、唯一末尾 `/`、显式端口保留、IPv4/主机名，以及凭据、path、query、fragment、空白、非 ASCII、非法端口和超长输入拒绝。

- [ ] **Step 2: 增加 Hub 快照、版本、单 pending 请求与结果**

```c
typedef struct {
    char service_url[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U];
    uint64_t version;
} app_network_hub_settings_snapshot_t;
```

请求 ID 严格递增且不回绕；测试和保存共享同一个 pending 槽。HTTP 回调只复制/排队，网络 I/O 只在 `app_network_task` 执行。

- [ ] **Step 3: 实现无凭据 `/healthz` 探测**

用规范化候选拼接 `/healthz`，执行有界 GET，不设置 Authorization 或设备 Token。只有 HTTP 2xx 且 JSON `status == "ok"` 视为成功；传输、超时和健康响应失败映射为不同稳定 reason。

- [ ] **Step 4: 实现单 Blob 安全保存与最佳努力日志重配**

健康检查再次成功后读取现有 `system_storage_network_config_t`，只替换 `service_url`，同步提交 `network_cfg`。提交成功后才在短锁内公开 URL/version；失败保留旧值。随后最佳努力停止、重配并重启远端日志，失败只记录中文事实，不回滚 URL。

- [ ] **Step 5: 装配 Hub Settings/Actions 与客户元数据**

Hub Settings 与 Action 使用同一个 `section_id = "hub"`；番茄钟增加说明、单位、摘要标记；系统 section 改成“设备与系统”。移除 DeskMate 对调试型 network Status Provider 的装配，顶层只形成 Hub、番茄钟、设备与系统三项。

- [ ] **Step 6: 运行 URL 与产品契约测试**

Run: `& .\devices\deskmate\tools\tests\check_app_network_hub_settings.ps1`

Expected: PASS，包含主机编译 URL 测试和网络 Task/Provider/秘密边界静态检查。

Run: `& .\devices\deskmate\tools\tests\check_web_console_product_providers.ps1`

Expected: PASS。

### Task 6: 同步文档、资源预算与最终验证

**Files:**
- Modify: `devices/deskmate/docs/architecture/web_console_service.md`
- Modify: `shared/components/services/web_console_service/README.md`
- Modify: `devices/deskmate/main/application/README.md`
- Modify: `devices/deskmate/README.md`
- Modify: `docs/superpowers/specs/2026-08-08-deskmate-web-console-settings-design.md` only if implementation exposes a confirmed wording correction

**Interfaces:**
- Consumes: 最终实现、测试输出和完整模块 gzip 大小。
- Produces: 与代码一致的 Settings/Status/Actions、Hub URL、持久化顺序和验证边界文档。

- [ ] **Step 1: 更新契约文档**

文档明确顶层“文件管理 / 设置”、三个客户 section、Actions 2 条路由、Provider 生命周期、Hub URL 单 Blob 提交、番茄钟先持久化后发布、远程日志最佳努力重配和不包含 OTA/重启/Wi-Fi 的边界。

- [ ] **Step 2: 记录 gzip 资源变化**

Run a PowerShell command that imports `build_html.py`, assembles the old baseline module set and the final full module set, and prints exact compressed byte counts. If final increment exceeds 25%, first remove repeated CSS/JS before accepting the result.

- [ ] **Step 3: 运行完整静态和主机验证**

Run: `python -B .\shared\components\services\web_console_service\scripts\test_build_html.py`

Run: `& .\devices\deskmate\tools\tests\check_web_console_product_providers.ps1`

Run: `& .\devices\deskmate\tools\tests\check_app_network_hub_settings.ps1`

Run: `& .\shared\components\services\web_console_network_provider\tests\check_web_console_network_provider.ps1`

Run: `git diff --check`

Expected: 全部退出码为 0；仅本任务路径有改动，`services/hub/` 既有改动保持不变。

- [ ] **Step 4: 浏览器级静态验收**

生成完整 HTML，在 1440×900 与 390×844 检查首页三项、详情返回、步进器、粘性保存栏、确认层和焦点；无真实设备 API 时明确记录为模拟/静态验收，不冒充实机验证。

- [ ] **Step 5: 按职责创建提交**

```powershell
git add -- <shared-contract-and-web-files>
git commit -m "feat(web-console): 实现设置中心与非破坏性操作"

git add -- <deskmate-owner-provider-and-doc-files>
git commit -m "feat(deskmate): 增加 Hub 设置并保证持久化原子性"
```

提交前再次执行 `git diff --cached --check`，并确认未暂存 `services/hub/` 既有改动。
