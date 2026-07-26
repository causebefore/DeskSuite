# Web File Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 DeskMate 增加一个从设备设置页按需启停、通过一次性访问码认证、可浏览/下载/上传完整 `/sdcard` 的局域网页文件管理器。

**Architecture:** 新增独立 `web_file_service`，由 ESP-IDF HTTPD handler 串行完成认证、目录遍历和事务式文件传输；新增 `app_web_file` Application Task 管理 SD 检查、网络租约、Service 启停和设备端状态。现有 `app_network` 租约泛化为互斥的语音/Web 文件租约，Presentation 只暴露有界 View Model，LVGL 设置页只渲染状态并上报启停意图。

**Tech Stack:** ESP-IDF C/CMake、`esp_http_server`、FatFs/VFS、FreeRTOS、PSRAM heap capabilities、LVGL、Python 3 HTML minify/gzip 生成脚本、Unity 组件测试。

## Global Constraints

- 只在设备已经加入的局域网中使用，不创建 SoftAP，不合并配网 Portal，不提供公网访问。
- 网页逻辑根目录固定映射到完整 `SYSTEM_FILESYSTEM_MOUNT_POINT`（当前为 `/sdcard`）。
- 第一阶段只实现目录浏览、普通文件下载和普通文件上传；不实现删除、移动、重命名、复制、新建目录或配置编辑。
- 不引入 Arduino、WebDAV、WebSocket、ZIP、JSZip、二维码或浏览器端图片转换；`CONFIG_HTTPD_WS_SUPPORT` 保持关闭。
- 单文件上传上限固定为 `500U * 1024U * 1024U` 字节。
- 同时只允许一个已认证浏览器会话和一个目录/下载/上传文件请求。
- 访问码为带前导零的 6 位十进制数；Bearer 令牌为 128 位随机值的 32 字符小写十六进制编码。
- 会话空闲 10 分钟失效，活动传输期间不失效；连续 5 次错误访问码后锁定登录 30 秒。
- 上传和下载共享一个 32 KiB 缓冲，必须使用 `heap_caps_malloc(32U * 1024U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`；分配失败不得回退到内部 RAM 或栈。
- 上传采用原始 HTTP `PUT` 和有效 `Content-Length`；同名覆盖必须收到 `X-DeskMate-Overwrite: confirm`。
- `/sdcard/.deskmate-web` 只保存一个有界上传事务，不出现在目录列表，也不能通过文件 API 访问。
- 所有公共 API 使用 C ABI 和中文 Doxygen；项目日志、错误和运行期提示使用中文。
- Crosslink `FilesPage.html` 与 `scripts/build_html.py` 的实质改造必须保留 `Copyright (c) 2025 Dave Allie` 和完整 MIT License。
- 不修改 `docs/architecture/` 的已确认目标边界；新增 Service、Application 和 Presentation README 必须与现有架构规范一致。
- 只在 `dev` 分支开发；每个任务只暂存本任务文件，保留当前工作区已有的 `AGENTS.md`、`components/bsp/src/bsp_power.c`、`components/communication/transport/src/transport_http.cpp`、`main/main.c` 和 `sd卡路径.md` 改动。
- Agent 不主动编译。只有用户明确要求编译时，才在仓库根目录运行 `& .\dm.ps1 build`；不得直接运行 `idf.py`、`cmake` 或 `ninja`，也不得用其他编译器替代。

---

## File Map

### New `web_file_service` component

- `components/services/web_file_service/CMakeLists.txt`：组件源文件、依赖和构建期 HTML 生成规则。
- `components/services/web_file_service/README.md`：Service 职责、生命周期、并发、事务和失败终态。
- `components/services/web_file_service/include/web_file_service.h`：生命周期与只读状态快照公共 API。
- `components/services/web_file_service/src/web_file_service.c`：HTTPD 生命周期、URI 注册、停止协作和共享状态。
- `components/services/web_file_service/src/web_file_service_auth.c`：访问码、Bearer 会话、锁定和空闲超时。
- `components/services/web_file_service/src/web_file_service_path.c`：查询值百分号解码、UTF-8/路径校验、VFS 映射和 JSON/HTTP 文件名转义。
- `components/services/web_file_service/src/web_file_service_transfer.c`：目录双遍历校验、流式 JSON、下载和上传接收循环。
- `components/services/web_file_service/src/web_file_service_transaction.c`：事务记录、覆盖提交、回滚和启动恢复。
- `components/services/web_file_service/src/web_file_service_internal.h`：仅组件私有的常量、状态和协作接口。
- `components/services/web_file_service/src/web_file_service_web.h`：构建期生成网页字节数组的固定声明。
- `components/services/web_file_service/web/index.html`：由 Crosslink 文件页裁剪改造的单页中文 UI。
- `components/services/web_file_service/scripts/build_html.py`：确定性 minify、gzip 和 C 源码生成。
- `components/services/web_file_service/scripts/test_build_html.py`：生成器的 Python 单元测试。
- `components/services/web_file_service/test/test_web_file_service.c`：路径、认证、长度限制和事务决策 Unity 测试。

### Application, Presentation, and UI

- `main/application/app_web_file.h`：网页文件管理产品状态与非阻塞启停命令。
- `main/application/app_web_file.c`：按需 Application Task、网络租约和 Service 编排。
- `main/presentation/web_file_presenter.h`：设备端有界 View Model。
- `main/presentation/web_file_presenter.c`：Application 状态到 View Model 的转换。
- `main/application/app_network.h`、`main/application/app_network_task.c`：泛化互斥租约并增加 Web 文件专用 API。
- `main/application/app_settings.c`：关闭设置会话时触发 Web 文件服务安全停止。
- `main/ui/include/ui_runtime.h`、`main/app_main.c`：新增 Web 文件启停用户意图和 Composition Root 接线。
- `main/ui/pages/ui_settings_page.c`：新增“网页文件管理”子页及退出等待状态。
- `main/CMakeLists.txt`：注册新增 Application/Presenter 和 Service 依赖。

### Configuration, notices, and documentation

- `sdkconfig.defaults`、`sdkconfig.ci`、`sdkconfig`：FatFs API 编码统一为 UTF-8，保持 HTTPD WebSocket 关闭。
- `THIRD_PARTY_NOTICES.md`：Crosslink 来源、修改说明和 MIT License 全文。
- `README.md`、`main/application/README.md`、`main/presentation/README.md`、`main/ui/README.md`：同步新增组件、数据流和用户交互。

---

### Task 1: Migrate and embed the browser page

**Files:**
- Create: `components/services/web_file_service/CMakeLists.txt`
- Create: `components/services/web_file_service/src/web_file_service_web.h`
- Create: `components/services/web_file_service/web/index.html`
- Create: `components/services/web_file_service/scripts/build_html.py`
- Create: `components/services/web_file_service/scripts/test_build_html.py`
- Create: `THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Consumes: Crosslink `C:\Users\lbq08\Desktop\crosslink\src\network\html\FilesPage.html`, `C:\Users\lbq08\Desktop\crosslink\scripts\build_html.py`, and `C:\Users\lbq08\Desktop\crosslink\LICENSE`.
- Produces: `extern const uint8_t web_file_index_gz[];` and `extern const size_t web_file_index_gz_size;` in `web_file_service_web.h`; build output `web_file_index.generated.c`.

- [ ] **Step 1: Write the generator tests before the generator**

Create `scripts/test_build_html.py` with three concrete tests:

```python
import gzip
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("build_html.py")


class BuildHtmlTest(unittest.TestCase):
    def run_generator(self, html: str) -> str:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "index.html"
            output = root / "web.generated.c"
            source.write_text(html, encoding="utf-8")
            subprocess.run(
                [sys.executable, str(SCRIPT), "--input", str(source), "--output", str(output)],
                check=True,
            )
            return output.read_text(encoding="utf-8")

    def test_output_is_deterministic(self):
        first = self.run_generator("<html><body>  中文  </body></html>")
        second = self.run_generator("<html><body>  中文  </body></html>")
        self.assertEqual(first, second)

    def test_output_declares_expected_symbols(self):
        generated = self.run_generator("<html><body>DeskMate</body></html>")
        self.assertIn("const uint8_t web_file_index_gz[]", generated)
        self.assertIn("const size_t web_file_index_gz_size", generated)

    def test_embedded_bytes_are_gzip(self):
        generated = self.run_generator("<html><body>DeskMate</body></html>")
        self.assertIn("0x1f, 0x8b", generated)
```

- [ ] **Step 2: Run the generator test and confirm the missing script fails**

Run:

```powershell
python .\components\services\web_file_service\scripts\test_build_html.py -v
```

Expected: FAIL because `build_html.py` does not exist.

- [ ] **Step 3: Implement the deterministic generator**

Implement `build_html.py` with `argparse`, UTF-8 input, Crosslink-compatible preserved `pre/code/textarea/script/style` blocks, `gzip.compress(..., compresslevel=9, mtime=0)`, and C output that starts with:

```c
/* 由 scripts/build_html.py 自动生成，请勿手工修改。 */
#include "web_file_service_web.h"

const uint8_t web_file_index_gz[] = {
    /* 每行 16 字节，由脚本写出 */
};
const size_t web_file_index_gz_size = sizeof(web_file_index_gz);
```

The script CLI is exactly:

```text
build_html.py --input <index.html> --output <web_file_index.generated.c>
```

Use `mtime=0` and a terminal newline so two runs produce byte-identical C.

- [ ] **Step 4: Migrate only the approved page behavior**

Create `web/index.html` from the Crosslink layout and retain a source comment:

```html
<!--
  Adapted from Crosslink src/network/html/FilesPage.html.
  Copyright (c) 2025 Dave Allie, MIT License.
  Modified for DeskMate authenticated HTTP file management.
-->
```

The page must contain these exact client contracts:

```javascript
const API = {
  session: "/api/session",
  files: "/api/files",
  file: "/api/file",
};
const TOKEN_KEY = "deskmateWebFileToken";
let currentPath = "/";
let transferActive = false;
```

Implement:

- a 6-digit access-code form posting plain UTF-8 text to `/api/session`;
- `sessionStorage` only for the returned token;
- `Authorization: Bearer <token>` on all file API calls;
- breadcrumb navigation and a table containing name, type, and size;
- file download through authenticated `fetch`;
- raw `XMLHttpRequest.send(file)` upload to the current directory;
- `upload.onprogress` rendering sent bytes, percentage, and bytes/second;
- retry after `409` only when the user confirms, with `X-DeskMate-Overwrite: confirm`;
- global transfer disable while upload or download is active;
- return to login and clear the token on `401`;
- no CORS, CDN, JSZip, WebSocket, WebDAV, delete, rename, move, copy, mkdir, ZIP, configuration, image conversion, or QR code.

- [ ] **Step 5: Wire generation into component CMake**

Use the ESP-IDF build Python interpreter and a generated C file under the component build directory:

```cmake
idf_build_get_property(python PYTHON)
set(web_file_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(web_file_generated_source "${web_file_generated_dir}/web_file_index.generated.c")

add_custom_command(
    OUTPUT "${web_file_generated_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${web_file_generated_dir}"
    COMMAND "${python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            --input "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            --output "${web_file_generated_source}"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    VERBATIM
)

idf_component_register(
    SRCS "${web_file_generated_source}"
    PRIV_INCLUDE_DIRS "src"
)
```

Declare the generated symbols in `src/web_file_service_web.h` using `<stddef.h>` and `<stdint.h>`.

- [ ] **Step 6: Add the third-party notice**

Create `THIRD_PARTY_NOTICES.md` with:

- source project and copied paths;
- a statement that DeskMate removed unsupported Crosslink functions and changed the transport/authentication;
- `Copyright (c) 2025 Dave Allie`;
- the complete, unmodified MIT License text from Crosslink.

- [ ] **Step 7: Run page-pipeline checks**

Run:

```powershell
python .\components\services\web_file_service\scripts\test_build_html.py -v
$tempFile = Join-Path $env:TEMP 'deskmate-web-file-index.generated.c'
python .\components\services\web_file_service\scripts\build_html.py --input .\components\services\web_file_service\web\index.html --output $tempFile
Select-String -LiteralPath $tempFile -Pattern 'const uint8_t web_file_index_gz|const size_t web_file_index_gz_size'
```

Expected: three Python tests pass and both generated symbols are found.

- [ ] **Step 8: Commit the page migration**

```powershell
git add -- components/services/web_file_service/CMakeLists.txt components/services/web_file_service/src/web_file_service_web.h components/services/web_file_service/web/index.html components/services/web_file_service/scripts/build_html.py components/services/web_file_service/scripts/test_build_html.py THIRD_PARTY_NOTICES.md
git commit -m "feat(web-file): 迁移并内嵌文件管理页面"
```

---

### Task 2: Implement path and authentication safety kernels

**Files:**
- Create: `components/services/web_file_service/src/web_file_service_internal.h`
- Create: `components/services/web_file_service/src/web_file_service_path.c`
- Create: `components/services/web_file_service/src/web_file_service_auth.c`
- Create: `components/services/web_file_service/test/test_web_file_service.c`
- Modify: `components/services/web_file_service/CMakeLists.txt`

**Interfaces:**
- Consumes: `SYSTEM_FILESYSTEM_MOUNT_POINT`, `esp_fill_random`, monotonic microsecond timestamps supplied by callers.
- Produces:
  - `esp_err_t web_file_path_decode_and_map(const char *encoded, char *logical, size_t logical_size, char *filesystem, size_t filesystem_size);`
  - `esp_err_t web_file_json_escape(const char *input, char *output, size_t output_size);`
  - `esp_err_t web_file_percent_encode(const char *input, char *output, size_t output_size);`
  - `void web_file_auth_reset(web_file_auth_state_t *state, const char access_code[7]);`
  - `web_file_auth_result_t web_file_auth_create_session(web_file_auth_state_t *state, const char *code, const uint8_t random_token[16], int64_t now_us, char out_token[33]);`
  - `web_file_auth_result_t web_file_auth_authorize(web_file_auth_state_t *state, const char *bearer, int64_t now_us, bool transfer_active);`

- [ ] **Step 1: Write the failing Unity safety tests**

Add tests with explicit cases:

```c
TEST_CASE("网页文件路径接受中文和保留字符", "[web_file][path]")
{
    char logical[512];
    char filesystem[544];
    TEST_ASSERT_EQUAL(
        ESP_OK,
        web_file_path_decode_and_map("/%E7%85%A7%E7%89%87/%E6%98%A5%20%E8%8A%82%2523.jpg",
                                     logical,
                                     sizeof(logical),
                                     filesystem,
                                     sizeof(filesystem)));
    TEST_ASSERT_EQUAL_STRING("/照片/春 节%23.jpg", logical);
    TEST_ASSERT_EQUAL_STRING("/sdcard/照片/春 节%23.jpg", filesystem);
}

TEST_CASE("网页文件路径拒绝越界和无效编码", "[web_file][path]")
{
    char logical[512];
    char filesystem[544];
    const char *invalid[] = {
        "/../secret", "/./file", "/a//b", "/a\\b", "/%00", "/%2e%2e/x",
        "/%C0%AF", "/%ED%A0%80", "/.deskmate-web/file", "relative/path",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        TEST_ASSERT_NOT_EQUAL(
            ESP_OK,
            web_file_path_decode_and_map(invalid[i],
                                         logical,
                                         sizeof(logical),
                                         filesystem,
                                         sizeof(filesystem)));
    }
}

TEST_CASE("网页文件认证执行锁定空闲失效和单会话", "[web_file][auth]")
{
    web_file_auth_state_t auth;
    const uint8_t token_bytes[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    char token[33];
    web_file_auth_reset(&auth, "012345");
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        TEST_ASSERT_EQUAL(WEB_FILE_AUTH_BAD_CODE,
                          web_file_auth_create_session(
                              &auth, "999999", token_bytes, attempt, token));
    }
    TEST_ASSERT_EQUAL(WEB_FILE_AUTH_LOCKED,
                      web_file_auth_create_session(&auth, "999999", token_bytes, 4, token));
    TEST_ASSERT_EQUAL(WEB_FILE_AUTH_LOCKED,
                      web_file_auth_create_session(&auth, "012345", token_bytes,
                                                   29LL * 1000LL * 1000LL, token));
    TEST_ASSERT_EQUAL(WEB_FILE_AUTH_OK,
                      web_file_auth_create_session(&auth, "012345", token_bytes,
                                                   31LL * 1000LL * 1000LL, token));
    TEST_ASSERT_EQUAL_STRING("00112233445566778899aabbccddeeff", token);
    TEST_ASSERT_EQUAL(WEB_FILE_AUTH_SESSION_BUSY,
                      web_file_auth_create_session(&auth, "012345", token_bytes,
                                                   32LL * 1000LL * 1000LL, token));
    TEST_ASSERT_EQUAL(WEB_FILE_AUTH_EXPIRED,
                      web_file_auth_authorize(&auth, token,
                                              (31LL + 601LL) * 1000LL * 1000LL, false));
}
```

Add segment-length cases for 255-byte acceptance, 256-byte rejection, truncated `%` escapes, decoded control characters, quotes/backslashes in JSON, and exact output-buffer exhaustion.

- [ ] **Step 2: Record the C test execution gate**

The repository currently has no approved command that executes component Unity tests. Keep the failing tests in `test/test_web_file_service.c`; do not invoke `idf.py` or another compiler. When the user later authorizes compilation, only `& .\dm.ps1 build` may be used, and this test gap must remain documented in the component README.

- [ ] **Step 3: Define bounded private types and constants**

In `web_file_service_internal.h`, define:

```c
#define WEB_FILE_ACCESS_CODE_LENGTH          6U
#define WEB_FILE_ACCESS_CODE_BUFFER_SIZE     7U
#define WEB_FILE_TOKEN_BYTES                 16U
#define WEB_FILE_TOKEN_BUFFER_SIZE           33U
#define WEB_FILE_LOGICAL_PATH_BUFFER_SIZE    512U
#define WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE 544U
#define WEB_FILE_SESSION_IDLE_TIMEOUT_US     (10LL * 60LL * 1000LL * 1000LL)
#define WEB_FILE_LOGIN_LOCKOUT_US            (30LL * 1000LL * 1000LL)
#define WEB_FILE_LOGIN_MAX_FAILURES          5U

typedef enum
{
    WEB_FILE_AUTH_OK = 0,
    WEB_FILE_AUTH_BAD_CODE,
    WEB_FILE_AUTH_LOCKED,
    WEB_FILE_AUTH_SESSION_BUSY,
    WEB_FILE_AUTH_UNAUTHORIZED,
    WEB_FILE_AUTH_EXPIRED,
} web_file_auth_result_t;

typedef struct
{
    char     access_code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE];
    uint8_t  token[WEB_FILE_TOKEN_BYTES];
    bool     session_active;
    uint8_t  failed_attempts;
    int64_t  lockout_until_us;
    int64_t  last_activity_us;
} web_file_auth_state_t;
```

Declare each private function used by another `.c` file and add Chinese Doxygen to non-obvious UTF-8, constant-time comparison and session transition functions.

- [ ] **Step 4: Implement strict percent decode and UTF-8 validation**

`web_file_path_decode_and_map()` must:

1. require one leading `/`;
2. decode each `%HH` exactly once and leave literal `+` unchanged;
3. reject malformed percent escapes, decoded NUL/control bytes, `\`, empty interior segments, `.` and `..`;
4. validate shortest-form UTF-8, scalar ranges, surrogate exclusion and `U+10FFFF` ceiling;
5. reject any segment over 255 encoded bytes;
6. reject the first segment `.deskmate-web`;
7. write the normalized logical path and `SYSTEM_FILESYSTEM_MOUNT_POINT + logical`;
8. fail without truncated output when either destination is too small.

Implement JSON escaping for `"`, `\`, tab/newline/control values and percent encoding for `Content-Disposition filename*`.

- [ ] **Step 5: Implement constant-time authentication transitions**

Use a loop that accumulates XOR differences for the six code bytes and sixteen decoded token bytes. `web_file_auth_create_session()` must:

- enforce six ASCII digits;
- reject during lockout without modifying the lockout deadline;
- set 30-second lockout on the fifth consecutive failure;
- reject a new login while an unexpired session exists;
- copy caller-supplied random bytes only after successful code verification;
- reset the failure counter and write exactly 32 lowercase hex characters.

`web_file_auth_authorize()` must decode exactly 32 lowercase/uppercase hex characters, compare all 16 bytes in constant time, expire idle sessions after 10 minutes when `transfer_active == false`, and refresh `last_activity_us` on success.

- [ ] **Step 6: Add sources to CMake and perform non-compiling checks**

Add `src/web_file_service_path.c` and `src/web_file_service_auth.c` to `SRCS`, `src` to `PRIV_INCLUDE_DIRS`, and `esp_common`, `esp_timer`, and `sys` to private requirements as actually included.

Run:

```powershell
rg -n "web_file_path_decode_and_map|web_file_auth_create_session|WEB_FILE_LOGIN_MAX_FAILURES" .\components\services\web_file_service
git diff --check
```

Expected: declarations, implementations, and tests are all found; `git diff --check` emits no output.

- [ ] **Step 7: Commit the safety kernels**

```powershell
git add -- components/services/web_file_service/CMakeLists.txt components/services/web_file_service/src/web_file_service_internal.h components/services/web_file_service/src/web_file_service_path.c components/services/web_file_service/src/web_file_service_auth.c components/services/web_file_service/test/test_web_file_service.c
git commit -m "feat(web-file): 增加路径与会话安全内核"
```

---

### Task 3: Add Service lifecycle and authenticated HTTP session

**Files:**
- Create: `components/services/web_file_service/include/web_file_service.h`
- Create: `components/services/web_file_service/src/web_file_service.c`
- Create: `components/services/web_file_service/README.md`
- Modify: `components/services/web_file_service/CMakeLists.txt`
- Modify: `components/services/web_file_service/src/web_file_service_internal.h`
- Modify: `components/services/web_file_service/test/test_web_file_service.c`

**Interfaces:**
- Consumes: generated `web_file_index_gz`, authentication kernel, ESP-IDF HTTPD.
- Produces:

```c
typedef enum
{
    WEB_FILE_SERVICE_STATE_UNINITIALIZED = 0,
    WEB_FILE_SERVICE_STATE_INITIALIZED,
    WEB_FILE_SERVICE_STATE_STARTING,
    WEB_FILE_SERVICE_STATE_RUNNING,
    WEB_FILE_SERVICE_STATE_STOPPING,
    WEB_FILE_SERVICE_STATE_CLEANUP_FAILED,
} web_file_service_state_t;

typedef struct
{
    web_file_service_state_t state;
    bool                     session_active;
    bool                     transfer_active;
    char                     access_code[7];
    esp_err_t                last_error;
} web_file_service_status_t;

esp_err_t web_file_service_init(void);
esp_err_t web_file_service_start(void);
esp_err_t web_file_service_stop(uint32_t timeout_ms);
esp_err_t web_file_service_get_status_copy(web_file_service_status_t *out_status);
esp_err_t web_file_service_deinit(void);
```

- [ ] **Step 1: Extend tests with lifecycle argument and secret-reset cases**

Add Unity cases that assert:

```c
TEST_CASE("网页文件服务状态快照拒绝空输出", "[web_file][lifecycle]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, web_file_service_get_status_copy(NULL));
}

TEST_CASE("认证重置立即清除会话令牌", "[web_file][auth]")
{
    web_file_auth_state_t auth;
    memset(&auth, 0xa5, sizeof(auth));
    web_file_auth_reset(&auth, "000001");
    TEST_ASSERT_FALSE(auth.session_active);
    for (size_t i = 0; i < sizeof(auth.token); ++i)
    {
        TEST_ASSERT_EQUAL_UINT8(0U, auth.token[i]);
    }
}
```

- [ ] **Step 2: Define the public lifecycle contract**

Create `include/web_file_service.h` with the exact types and APIs above. Document:

- `init()` creates fixed synchronization resources but does not touch the SD card or start HTTPD;
- `start()` requires `INITIALIZED`, generates a new code with `esp_fill_random`, registers four URI handlers and enters `RUNNING` only after HTTPD starts;
- `stop(timeout_ms)` rejects zero timeout, invalidates secrets first, rejects new handlers, closes client sessions, waits for active handlers, then stops HTTPD;
- `deinit()` only succeeds after all HTTPD/file/buffer resources are gone;
- `CLEANUP_FAILED` refuses a new start until a later successful stop finishes cleanup.

- [ ] **Step 3: Implement Service state ownership and handler accounting**

The private state must contain:

```c
typedef struct
{
    SemaphoreHandle_t          lock;
    SemaphoreHandle_t          handlers_drained;
    httpd_handle_t             server;
    web_file_service_state_t   state;
    web_file_auth_state_t      auth;
    uint32_t                   active_handlers;
    bool                       accepting_requests;
    bool                       transfer_active;
    int                        active_transfer_socket;
    uint8_t                   *transfer_buffer;
    esp_err_t                  last_error;
} web_file_service_context_t;
```

All handlers call `web_file_handler_enter()` and `web_file_handler_leave()`. Never hold `lock` while receiving, sending, waiting, traversing a directory, opening a file, writing, syncing, renaming, closing or calling an external API.

- [ ] **Step 4: Implement `GET /`**

Register an exact URI with no wildcard:

```c
static const httpd_uri_t s_index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = handle_index_get,
};
```

Respond with:

```text
Content-Type: text/html; charset=utf-8
Content-Encoding: gzip
Cache-Control: no-store
X-Content-Type-Options: nosniff
Content-Length: <web_file_index_gz_size>
```

Send the generated byte array directly from flash and never expose the access code in HTML or logs.

- [ ] **Step 5: Implement `POST /api/session`**

Require:

- `Content-Type` beginning with `text/plain`;
- `Content-Length == 6`;
- exactly six request-body bytes;
- no `Authorization` requirement for this route.

Generate 16 random bytes only after the code is valid. Return:

```json
{"token":"00112233445566778899aabbccddeeff"}
```

Map authentication outcomes:

- bad code → `401 Unauthorized`;
- active lockout → `423 Locked`;
- an existing unexpired session → `409 Conflict`;
- malformed body → `400 Bad Request`.

All JSON error bodies use:

```json
{"error":"unauthorized","message":"访问码错误"}
```

and set `Cache-Control: no-store`. Do not add CORS headers.

- [ ] **Step 6: Implement bounded stop**

`web_file_service_stop(timeout_ms)` must:

1. atomically enter `STOPPING`, clear code/token, and set `accepting_requests = false`;
2. call `httpd_get_client_list()` and `httpd_sess_trigger_close()` for each connected socket;
3. wait on `handlers_drained` only for the caller’s remaining timeout;
4. call `httpd_stop()` only after `active_handlers == 0`;
5. free the PSRAM buffer if allocated, null every resource pointer, and return to `INITIALIZED`;
6. preserve `STOPPING`/`CLEANUP_FAILED` and `last_error` if the deadline or HTTPD cleanup fails.

Every recv/send wait timeout in `httpd_config_t` is 5 seconds; configure port 80, exactly four URI slots initially, no WebSocket, no LRU purge, and only the open sockets needed for one browser plus HTTPD control.

- [ ] **Step 7: Document the Service**

Write `README.md` sections matching other Service READMEs:

- positioning and non-goals;
- HTTP and file data flow;
- public API table;
- `UNINITIALIZED → INITIALIZED → STARTING → RUNNING → STOPPING`;
- HTTPD Task ownership and no extra polling Task;
- one session/one transfer;
- secret lifetime;
- stop timeout and `CLEANUP_FAILED`;
- dependency table;
- automated-test gap and hardware checks.

- [ ] **Step 8: Wire dependencies and run static checks**

Update CMake to include `src/web_file_service.c`, public `include`, and:

```cmake
REQUIRES esp_common
PRIV_REQUIRES esp_http_server esp_system esp_timer freertos heap log sys
```

Run:

```powershell
rg -n "httpd_register_uri_handler|Cache-Control|web_file_service_stop|CLEANUP_FAILED" .\components\services\web_file_service
rg -n "access_code|token" .\components\services\web_file_service\src\web_file_service.c
git diff --check
```

Review each secret match to ensure it is copied/cleared but never logged.

- [ ] **Step 9: Commit the HTTP lifecycle**

```powershell
git add -- components/services/web_file_service/CMakeLists.txt components/services/web_file_service/README.md components/services/web_file_service/include/web_file_service.h components/services/web_file_service/src/web_file_service.c components/services/web_file_service/src/web_file_service_internal.h components/services/web_file_service/test/test_web_file_service.c
git commit -m "feat(web-file): 建立认证文件服务生命周期"
```

---

### Task 4: Implement directory listing and file download

**Files:**
- Create: `components/services/web_file_service/src/web_file_service_transfer.c`
- Modify: `components/services/web_file_service/src/web_file_service.c`
- Modify: `components/services/web_file_service/src/web_file_service_internal.h`
- Modify: `components/services/web_file_service/CMakeLists.txt`
- Modify: `components/services/web_file_service/test/test_web_file_service.c`
- Modify: `components/services/web_file_service/README.md`

**Interfaces:**
- Consumes: `web_file_path_decode_and_map()`, `web_file_auth_authorize()`, `system_filesystem_get_info_copy()`, shared 32 KiB PSRAM buffer.
- Produces:
  - `esp_err_t web_file_handle_files_get(httpd_req_t *request);`
  - `esp_err_t web_file_handle_file_get(httpd_req_t *request);`
  - authenticated `GET /api/files?path=/...` and `GET /api/file?path=/...`.

- [ ] **Step 1: Add failing pure-output tests**

Add cases for JSON and `Content-Disposition` escaping:

```c
TEST_CASE("目录 JSON 文件名正确转义", "[web_file][json]")
{
    char output[128];
    TEST_ASSERT_EQUAL(ESP_OK,
                      web_file_json_escape("天气\"图标\\一.png", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("天气\\\"图标\\\\一.png", output);
}

TEST_CASE("下载文件名按 UTF-8 百分号编码", "[web_file][download]")
{
    char output[128];
    TEST_ASSERT_EQUAL(ESP_OK,
                      web_file_percent_encode("春节 #1.png", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("%E6%98%A5%E8%8A%82%20%231.png", output);
}
```

- [ ] **Step 2: Add a shared authenticated transfer guard**

For `/api/files` and `/api/file`:

1. read `Authorization` into a bounded 48-byte buffer;
2. require the exact case-sensitive prefix `Bearer `;
3. authorize against the active token;
4. atomically acquire `transfer_active` and record `httpd_req_to_sockfd(request)`;
5. return `409 Conflict` with `{"error":"busy","message":"另一个文件请求正在进行"}` if occupied;
6. release the flag and socket in one cleanup block on every return path.

Authentication occurs before path parsing or filesystem metadata access so unauthenticated callers cannot infer file existence.

Read the query with `httpd_req_get_url_query_len()` followed by one exact-size internal-RAM allocation and `httpd_req_get_url_query_str()`. Require the query to contain exactly one `path=<encoded-value>` field and no `&`; pass only the still-encoded value to `web_file_path_decode_and_map()`. The allocation is capped by `CONFIG_HTTPD_MAX_URI_LEN` and is freed before handler cleanup.

- [ ] **Step 3: Implement double-pass directory listing**

The first `opendir/readdir/stat` pass validates every visible entry before HTTP headers are sent:

- skip only `.deskmate-web` in logical root `/`;
- validate each name as UTF-8 and reject control characters;
- reject symbolic links and unsupported file types;
- ensure the escaped single-entry JSON fits the fixed response scratch buffer;
- propagate `readdir`, `stat`, and close errors.

Reopen the directory for the second pass and stream:

```json
{"path":"/images","totalBytes":34359738368,"freeBytes":21474836480,"entries":[
{"name":"天气图标.png","type":"file","sizeBytes":12345}
]}
```

Use `httpd_resp_send_chunk()` for the prefix, each comma/entry, suffix, and terminal zero-length chunk. Do not allocate an entry array or sort on the device.

- [ ] **Step 4: Implement streamed download**

Before sending:

- map and `lstat` the path;
- require a regular file;
- record the file length;
- acquire the 32 KiB PSRAM buffer with the exact global-constraint allocation;
- select MIME from a bounded extension table (`txt`, `html`, `json`, `pdf`, `png`, `jpg/jpeg`, `gif`, `webp`; fallback `application/octet-stream`);
- set `Content-Length`;
- set `Content-Disposition: attachment; filename="download"; filename*=UTF-8''<encoded-name>`.

Then loop `fread()` → `httpd_resp_send_chunk()` and check the Service cancellation flag between chunks. On disconnect, short read, send failure or cancellation, close the file and release the transfer/buffer state immediately.

- [ ] **Step 5: Register routes and error mapping**

Register:

```c
{ .uri = "/api/files", .method = HTTP_GET, .handler = web_file_handle_files_get }
{ .uri = "/api/file",  .method = HTTP_GET, .handler = web_file_handle_file_get }
```

Map invalid query/path to `400`, missing paths to `404`, wrong type/active transfer to `409`, PSRAM allocation to `500`, and filesystem/I/O failure to `500`. Every error response is UTF-8 JSON and no-store.

- [ ] **Step 6: Update documentation and run non-compiling checks**

Run:

```powershell
rg -n "opendir|readdir|httpd_resp_send_chunk|Content-Disposition|MALLOC_CAP_SPIRAM" .\components\services\web_file_service\src
rg -n "malloc\\(32|uint8_t .*\\[32.*1024" .\components\services\web_file_service\src
git diff --check
```

Expected: listing/download streaming and explicit PSRAM allocation are found; no 32 KiB ordinary heap or stack buffer is found.

- [ ] **Step 7: Commit browsing and download**

```powershell
git add -- components/services/web_file_service/CMakeLists.txt components/services/web_file_service/README.md components/services/web_file_service/src/web_file_service.c components/services/web_file_service/src/web_file_service_internal.h components/services/web_file_service/src/web_file_service_transfer.c components/services/web_file_service/test/test_web_file_service.c
git commit -m "feat(web-file): 支持目录浏览与流式下载"
```

---

### Task 5: Implement transactional upload and recovery

**Files:**
- Create: `components/services/web_file_service/src/web_file_service_transaction.c`
- Modify: `components/services/web_file_service/src/web_file_service_transfer.c`
- Modify: `components/services/web_file_service/src/web_file_service.c`
- Modify: `components/services/web_file_service/src/web_file_service_internal.h`
- Modify: `components/services/web_file_service/CMakeLists.txt`
- Modify: `components/services/web_file_service/test/test_web_file_service.c`
- Modify: `components/services/web_file_service/README.md`

**Interfaces:**
- Consumes: authenticated transfer guard, strict path mapping, `system_filesystem_get_info_copy()`.
- Produces:
  - `esp_err_t web_file_handle_file_put(httpd_req_t *request);`
  - `esp_err_t web_file_transaction_recover(void);`
  - `esp_err_t web_file_transaction_commit(const web_file_transaction_t *transaction);`
  - `web_file_recovery_action_t web_file_transaction_decide_recovery(web_file_transaction_phase_t phase, bool target_exists, bool backup_exists, bool part_exists, bool target_matches_expected_length);`
  - `esp_err_t web_file_upload_validate_length(size_t content_length);`
  - authenticated `PUT /api/file?path=/...`.

- [ ] **Step 1: Add failing length and recovery-decision tests**

Define and test:

```c
typedef enum
{
    WEB_FILE_TRANSACTION_PREPARED = 0,
    WEB_FILE_TRANSACTION_BACKUP_MOVED,
    WEB_FILE_TRANSACTION_TARGET_COMMITTED,
} web_file_transaction_phase_t;

typedef enum
{
    WEB_FILE_RECOVERY_REMOVE_PART = 0,
    WEB_FILE_RECOVERY_RESTORE_BACKUP,
    WEB_FILE_RECOVERY_ACCEPT_COMMIT,
    WEB_FILE_RECOVERY_AMBIGUOUS,
} web_file_recovery_action_t;

typedef struct
{
    web_file_transaction_phase_t phase;
    uint64_t                     expected_length;
    char                         target_path[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
} web_file_transaction_t;
```

Test the decision matrix:

```c
TEST_CASE("覆盖事务恢复决策不猜测用户文件", "[web_file][transaction]")
{
    TEST_ASSERT_EQUAL(WEB_FILE_RECOVERY_RESTORE_BACKUP,
                      web_file_transaction_decide_recovery(
                          WEB_FILE_TRANSACTION_BACKUP_MOVED,
                          false, true, true, false));
    TEST_ASSERT_EQUAL(WEB_FILE_RECOVERY_ACCEPT_COMMIT,
                      web_file_transaction_decide_recovery(
                          WEB_FILE_TRANSACTION_BACKUP_MOVED,
                          true, true, false, true));
    TEST_ASSERT_EQUAL(WEB_FILE_RECOVERY_AMBIGUOUS,
                      web_file_transaction_decide_recovery(
                          WEB_FILE_TRANSACTION_BACKUP_MOVED,
                          true, true, true, true));
}

TEST_CASE("上传长度边界固定为五百MiB", "[web_file][upload]")
{
    TEST_ASSERT_EQUAL(ESP_OK, web_file_upload_validate_length(0));
    TEST_ASSERT_EQUAL(ESP_OK, web_file_upload_validate_length(500U * 1024U * 1024U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      web_file_upload_validate_length((500U * 1024U * 1024U) + 1U));
}
```

The booleans are `target_exists`, `backup_exists`, `part_exists`, and `target_matches_expected_length`.

- [ ] **Step 2: Implement an unambiguous journal**

Use fixed Service-owned paths:

```c
#define WEB_FILE_TRANSACTION_DIR     "/sdcard/.deskmate-web"
#define WEB_FILE_TRANSACTION_PART    "/sdcard/.deskmate-web/upload.part"
#define WEB_FILE_TRANSACTION_BACKUP  "/sdcard/.deskmate-web/upload.bak"
#define WEB_FILE_TRANSACTION_JOURNAL "/sdcard/.deskmate-web/transaction"
#define WEB_FILE_TRANSACTION_NEW     "/sdcard/.deskmate-web/transaction.new"
```

The journal is UTF-8 text with four lines:

```text
version=1
phase=BACKUP_MOVED
length=12345
target=/photos/春节.png
```

Path validation already rejects line-control characters, so parsing remains unambiguous. Write a new journal to `transaction.new`, call `fflush()` and `fsync(fileno(file))`, close it, then rename to `transaction`. Every phase transition rewrites and syncs the journal before the next rename.

- [ ] **Step 3: Implement startup recovery**

Call `web_file_transaction_recover()` during `web_file_service_start()` before generating secrets or starting HTTPD.

Recovery rules:

- no journal and only `.part` → remove `.part`;
- no journal and only `transaction.new` → remove `transaction.new`;
- `PREPARED` with original target intact → remove `.part` and journal;
- `BACKUP_MOVED`, target missing, backup present → restore backup, remove `.part`, remove journal;
- `BACKUP_MOVED`, target present with expected length, backup present, part absent → accept commit, remove backup and journal;
- `TARGET_COMMITTED`, valid target and backup present → remove backup and journal;
- `TARGET_COMMITTED`, target missing and backup present → restore backup and remove journal;
- malformed journal, path outside `/sdcard`, missing required artifacts, or any conflicting artifact set → return an error without deleting or renaming user-visible files.

Never expose or accept the transaction directory through HTTP.

- [ ] **Step 4: Validate upload before receiving content**

For `PUT /api/file`:

1. authorize and acquire the transfer guard;
2. require non-negative valid `Content-Length` no greater than 500 MiB;
3. validate the final path and require an existing real parent directory;
4. reject a directory or symbolic-link target;
5. if a regular target exists and the overwrite header is absent, return `409` before receiving any body;
6. query free space and require `content_length + 1 MiB` safety margin;
7. create/reuse the transaction directory only after all checks pass;
8. allocate the shared 32 KiB PSRAM buffer.

Use `507 Insufficient Storage` for the free-space check and `413 Content Too Large` for the size limit.

- [ ] **Step 5: Receive and sync the temporary file**

Open `upload.part` exclusively, then loop:

```c
while (received_total < expected_length)
{
    const size_t wanted = MIN(WEB_FILE_TRANSFER_BUFFER_SIZE,
                              expected_length - received_total);
    const int received = httpd_req_recv(request, (char *) buffer, wanted);
    /* timeout retries are bounded by HTTPD receive timeout and stop state */
    /* fwrite must consume the complete received block or fail */
}
```

After the loop:

- `fflush`, `fsync`, close;
- `stat` and require exact expected length;
- on receive timeout exhaustion, disconnect, cancellation, short write, sync or length mismatch, close and remove `.part`;
- never delete the existing target in this receive phase.

- [ ] **Step 6: Commit new and replacement files**

For a new target, rename `.part` directly to the target and remove empty Service metadata.

For overwrite:

1. write/sync `PREPARED`;
2. rename target to `upload.bak`;
3. write/sync `BACKUP_MOVED`;
4. rename `.part` to target;
5. write/sync `TARGET_COMMITTED`;
6. remove `upload.bak`;
7. remove the journal.

If step 4 fails, immediately restore `upload.bak` to the target. If later cleanup fails, leave the journal/artifacts in a recoverable state and report `500`; do not claim success before the durable target is in place.

- [ ] **Step 7: Register PUT and integrate stop cancellation**

Register `{ .uri = "/api/file", .method = HTTP_PUT, .handler = web_file_handle_file_put }`.

At each received block boundary, before transaction commit, and between durable rename phases, check `accepting_requests`. During rename phases, finish the current recovery-safe phase before returning so `stop()` never observes an undocumented partial state.

- [ ] **Step 8: Update README and run transaction checks**

Run:

```powershell
rg -n "500U \\* 1024U \\* 1024U|Insufficient Storage|upload\\.part|upload\\.bak|TARGET_COMMITTED|fsync" .\components\services\web_file_service
rg -n "remove\\(.*target|unlink\\(.*target" .\components\services\web_file_service\src
git diff --check
```

Expected: size/space/journal/fsync logic is found; no code directly removes the requested target to make room for an upload.

- [ ] **Step 9: Commit transactional upload**

```powershell
git add -- components/services/web_file_service/CMakeLists.txt components/services/web_file_service/README.md components/services/web_file_service/src/web_file_service.c components/services/web_file_service/src/web_file_service_internal.h components/services/web_file_service/src/web_file_service_transfer.c components/services/web_file_service/src/web_file_service_transaction.c components/services/web_file_service/test/test_web_file_service.c
git commit -m "feat(web-file): 支持可恢复的事务式上传"
```

---

### Task 6: Generalize the network lease for Web file management

**Files:**
- Modify: `main/application/app_network.h:18-31,147-177`
- Modify: `main/application/app_network_task.c:45-71,93-119,1296-1396,1807-2075`
- Modify: `main/application/README.md`

**Interfaces:**
- Consumes: current synchronous control-response slot mechanism and Network Manager ONLINE status.
- Produces:
  - enum member `APP_NETWORK_LEASE_WEB_FILE`;
  - `esp_err_t app_network_acquire_web_file_lease(uint32_t timeout_ms, uint32_t *out_generation);`
  - `esp_err_t app_network_release_web_file_lease(uint32_t generation, uint32_t timeout_ms);`
  - unchanged voice lease API behavior.

- [ ] **Step 1: Define the generic internal lease state**

Replace:

```c
static bool     s_realtime_lease_active;
static uint32_t s_realtime_lease_generation;
```

with:

```c
static app_network_lease_type_t s_active_lease_type = APP_NETWORK_LEASE_NONE;
static uint32_t                 s_active_lease_generation;
```

Add `app_network_lease_type_t lease_type;` to `network_command_t`. Add `APP_NETWORK_LEASE_WEB_FILE` to the public enum and change snapshot documentation from “实时语音” to “互斥网络产品租约”.

- [ ] **Step 2: Refactor acquire/release into typed helpers**

Add private wrappers:

```c
static esp_err_t acquire_network_lease(app_network_lease_type_t type,
                                       uint32_t timeout_ms,
                                       uint32_t *out_generation);
static esp_err_t release_network_lease(app_network_lease_type_t type,
                                       uint32_t generation,
                                       uint32_t timeout_ms);
```

Each acquire command carries the requested type. Grant only when:

- Network Manager is `NETWORK_STATE_ONLINE`;
- `s_active_lease_type == APP_NETWORK_LEASE_NONE`;
- light sleep is not suspended;
- Portal/validation and OTA are inactive.

Release succeeds only when both type and generation match. A stale voice release must never release a Web file lease, and a stale Web release must never release a voice lease. Releasing when no lease is active remains idempotent `ESP_OK`.

- [ ] **Step 3: Preserve all existing conflict gates**

Replace every `s_realtime_lease_active` conflict test with:

```c
s_active_lease_type != APP_NETWORK_LEASE_NONE
```

This includes:

- Dashboard manual/periodic sync;
- OTA check/install;
- explicit Portal start;
- light-sleep suspend;
- timer rescheduling;
- lease snapshot;
- automatic no-config Portal transition inside `handle_manager_changed_command()`.

Do not block Network Manager reconnect commands: an active Web file lease must allow reconnecting the saved STA so the same HTTPD can resume on a new IPv4 address.

`reschedule_periodic_timer()` and `reschedule_ota_timer()` must leave their timers stopped while any lease is active, then restore policy timing only after the matching typed release succeeds.

- [ ] **Step 4: Add dedicated public wrappers**

Keep voice APIs as:

```c
return acquire_network_lease(APP_NETWORK_LEASE_REALTIME_VOICE,
                             timeout_ms,
                             out_generation);
```

and add Web wrappers using `APP_NETWORK_LEASE_WEB_FILE`. Log only the lease type and generation in Chinese; never log Web access codes or Bearer tokens.

- [ ] **Step 5: Update network Application documentation and inspect the blast radius**

Document that only one typed lease exists, both types block light sleep/Portal/OTA/sync, and reconnect remains allowed.

Run:

```powershell
rg -n "s_realtime_lease_active|s_realtime_lease_generation" .\main\application
rg -n "APP_NETWORK_LEASE_WEB_FILE|acquire_web_file_lease|release_web_file_lease|s_active_lease_type" .\main\application
git diff --check
```

Expected: the old state names have no matches; the enum, wrappers and generic state are present.

If the user has explicitly authorized compilation, run only:

```powershell
& .\dm.ps1 build
```

Otherwise record compilation as deferred and continue without substituting another build command.

- [ ] **Step 6: Commit the typed lease**

```powershell
git add -- main/application/app_network.h main/application/app_network_task.c main/application/README.md
git commit -m "refactor(network): 泛化互斥网络产品租约"
```

---

### Task 7: Add the Web file Application and Presenter

**Files:**
- Create: `main/application/app_web_file.h`
- Create: `main/application/app_web_file.c`
- Create: `main/presentation/web_file_presenter.h`
- Create: `main/presentation/web_file_presenter.c`
- Modify: `main/app_main.c:1-46,321-393`
- Modify: `main/CMakeLists.txt`
- Modify: `main/application/README.md`
- Modify: `main/presentation/README.md`

**Interfaces:**
- Consumes: Web file Service lifecycle, typed Web network lease, `system_filesystem_get_info_copy()`, `connect_get_link_snapshot_copy()`, Presentation status update.
- Produces:

```c
typedef enum
{
    APP_WEB_FILE_STATE_STOPPED = 0,
    APP_WEB_FILE_STATE_CHECKING_STORAGE,
    APP_WEB_FILE_STATE_ACQUIRING_NETWORK,
    APP_WEB_FILE_STATE_STARTING_SERVICE,
    APP_WEB_FILE_STATE_RUNNING,
    APP_WEB_FILE_STATE_STOPPING,
    APP_WEB_FILE_STATE_ERROR,
} app_web_file_state_t;

typedef struct
{
    app_web_file_state_t state;
    char                 url[32];
    char                 access_code[7];
    uint64_t             total_bytes;
    uint64_t             free_bytes;
    esp_err_t            last_error;
} app_web_file_status_t;

esp_err_t app_web_file_init(void);
esp_err_t app_web_file_request_start(void);
esp_err_t app_web_file_request_stop(void);
esp_err_t app_web_file_get_status_copy(app_web_file_status_t *out_status);
```

- [ ] **Step 1: Define non-blocking Application semantics**

Document in `app_web_file.h`:

- `request_start()` only changes protected state and creates/notifies the one-shot Application Task; it never waits for Wi-Fi, SD, HTTPD or file recovery;
- `request_stop()` only sets a stop request and notifies the Task;
- all status fields are copied under a short critical section;
- `url` is refreshed from the latest associated IPv4 during status-copy while the Service is running;
- public APIs have complete Chinese Doxygen.

- [ ] **Step 2: Implement the one-shot Application Task**

Use a private state lock, `TaskHandle_t`, `stop_requested`, and `lease_generation`. The task executes:

```text
CHECKING_STORAGE
  → system_filesystem_get_info_copy
ACQUIRING_NETWORK
  → app_network_acquire_web_file_lease
STARTING_SERVICE
  → web_file_service_start
RUNNING
  → wait for stop notification
STOPPING
  → web_file_service_stop
  → app_network_release_web_file_lease
STOPPED
  → clear Task handle and self-delete
```

Use fixed bounds:

```c
#define APP_WEB_FILE_LEASE_TIMEOUT_MS  1000U
#define APP_WEB_FILE_STOP_TIMEOUT_MS   6000U
#define APP_WEB_FILE_TASK_STACK_SIZE   4096U
#define APP_WEB_FILE_TASK_PRIORITY     4U
```

After every state transition call `presentation_dispatch_status_update()` outside the state lock.

If storage, lease or Service start fails, release only resources actually acquired, set `ERROR`, and clear the Task handle. If Service stop fails, retain the lease, keep an error state that allows another stop request, and never falsely publish `STOPPED`.

- [ ] **Step 3: Keep URL and secrets bounded**

On a running snapshot:

- get `connect_link_info_t`;
- require `associated && has_ipv4`;
- format exactly `http://<IPv4>/` into `url[32]`;
- copy the current Service access code;
- refresh total/free bytes with `system_filesystem_get_info_copy()`.

If Wi-Fi temporarily loses IPv4, keep state `RUNNING` but expose an empty URL until reconnect. Do not regenerate the code or restart HTTPD.

- [ ] **Step 4: Implement the Presenter**

Define:

```c
typedef struct
{
    app_web_file_state_t state;
    bool                 running;
    bool                 exit_allowed;
    char                 title[24];
    char                 url[32];
    char                 access_code[7];
    char                 total_size[24];
    char                 free_size[24];
    esp_err_t            error;
} web_file_view_model_t;

esp_err_t web_file_presenter_init(void);
void web_file_presenter_get_view_copy(web_file_view_model_t *out_view);
```

Map states to these exact titles:

- `STOPPED` → `网页文件管理`;
- `CHECKING_STORAGE` → `正在检查 SD 卡`;
- `ACQUIRING_NETWORK` → `正在申请网络`;
- `STARTING_SERVICE` → `正在启动服务`;
- `RUNNING` → `网页文件管理已开启`;
- `STOPPING` → `正在关闭服务`;
- `ERROR` → `启动或关闭失败`.

Format capacity as B/KiB/MiB/GiB with one decimal only when needed. `exit_allowed` is true for `STOPPED` and start failures with no retained Service/lease.

- [ ] **Step 5: Wire the Composition Root**

In `main/CMakeLists.txt` add:

```cmake
"application/app_web_file.c"
"presentation/web_file_presenter.c"
```

and add `web_file_service` to `PRIV_REQUIRES`.

In `app_main.c`:

- call `web_file_service_init()` in `init_runtime_capabilities()` after existing base Services;
- call `web_file_presenter_init()` in `init_presenters()`;
- call `app_web_file_init()` in `init_applications()`;
- preserve existing initialization order and error text style;
- do not edit the unrelated dirty `main/main.c`.

- [ ] **Step 6: Update Application and Presentation READMEs**

Document:

- Application owns product phase, typed lease generation and safe release;
- Service owns HTTP/file transaction state;
- Presenter only formats a copy and performs no side effects;
- state/URL refresh data flow;
- the one-shot Task exists only while starting/running/stopping.

- [ ] **Step 7: Run interface consistency checks**

Run:

```powershell
rg -n "app_web_file_(init|request_start|request_stop|get_status_copy)" .\main
rg -n "web_file_presenter_(init|get_view_copy)" .\main
rg -n "web_file_service" .\main\CMakeLists.txt .\main\app_main.c
git diff --check
```

Expected: every produced interface has one declaration and implementation, and Composition Root/CMake references are present.

- [ ] **Step 8: Commit Application and Presenter**

```powershell
git add -- main/CMakeLists.txt main/app_main.c main/application/app_web_file.h main/application/app_web_file.c main/presentation/web_file_presenter.h main/presentation/web_file_presenter.c main/application/README.md main/presentation/README.md
git commit -m "feat(web-file): 编排文件服务与设备展示状态"
```

---

### Task 8: Add the device Settings page and start/stop intents

**Files:**
- Modify: `main/ui/include/ui_runtime.h:22-39`
- Modify: `main/app_main.c:53-79`
- Modify: `main/application/app_settings.c:17-32`
- Modify: `main/ui/pages/ui_settings_page.c:20-68,250-401,602-894`
- Modify: `main/ui/README.md`

**Interfaces:**
- Consumes: `web_file_presenter_get_view_copy()`, `app_web_file_request_start()`, `app_web_file_request_stop()`.
- Produces:
  - `UI_USER_INTENT_SETTINGS_START_WEB_FILE`;
  - `UI_USER_INTENT_SETTINGS_STOP_WEB_FILE`;
  - a fourth root item named `网页文件管理`.

- [ ] **Step 1: Add narrow user intents**

Insert before `UI_USER_INTENT_COUNT`:

```c
UI_USER_INTENT_SETTINGS_START_WEB_FILE, /*!< 用户进入子页并请求启动网页文件管理 */
UI_USER_INTENT_SETTINGS_STOP_WEB_FILE,  /*!< 用户离开子页并请求停止网页文件管理 */
```

Map them in `app_main_ui_user_intent_callback()`:

```c
case UI_USER_INTENT_SETTINGS_START_WEB_FILE:
    return app_web_file_request_start();
case UI_USER_INTENT_SETTINGS_STOP_WEB_FILE:
    return app_web_file_request_stop();
```

These callback paths remain non-blocking because Application only posts state/Task notifications.

- [ ] **Step 2: Add settings-page objects and location**

Extend enums and UI state:

```c
SETTINGS_LOCATION_WEB_FILE,
SETTINGS_ITEM_WEB_FILE,

lv_obj_t *web_file_page;
lv_obj_t *web_file_body;
bool      web_file_exit_pending;
```

Create the root item between Network and System:

```c
(void) create_root_item(SETTINGS_ITEM_WEB_FILE,
                        s_view.web_file_page,
                        "网页文件管理");
```

Use the existing non-wrapping `lv_group` and header font behavior.

- [ ] **Step 3: Start automatically when the user enters the subpage**

In `on_root_item_clicked()` for `SETTINGS_ITEM_WEB_FILE`:

```c
s_view.location = SETTINGS_LOCATION_WEB_FILE;
s_view.web_file_exit_pending = false;
const ui_user_intent_t intent = {
    .id = UI_USER_INTENT_SETTINGS_START_WEB_FILE,
};
const esp_err_t error = ui_runtime_emit_user_intent(&intent);
render_web_file(error);
```

This is the explicit manual start action: selecting the settings item. Do not start the Service at boot or merely opening the settings menu.

- [ ] **Step 4: Render every device-side state**

`render_web_file(action_error)` reads `web_file_view_model_t` and draws:

- progress title for storage/network/Service start;
- running URL, six-digit code, total capacity and free capacity;
- `正在关闭服务` while stop is pending;
- Chinese error text containing `esp_err_to_name(error)`;
- a `长按右键重试` action only in a start-failure state;
- no QR code.

When `web_file_exit_pending` is true and the View Model reaches `STOPPED` or an exit-allowed error, call `return_to_root()` only after clearing the pending flag.

- [ ] **Step 5: Make Back perform cooperative stop**

Add `handle_web_file_action()`:

```c
if (action == PRESENTATION_SETTINGS_ACTION_BACK)
{
    const ui_user_intent_t intent = {
        .id = UI_USER_INTENT_SETTINGS_STOP_WEB_FILE,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    s_view.web_file_exit_pending = error == ESP_OK;
    render_web_file(error);
    return error;
}
```

If the current state is already `STOPPED`, return to root immediately. While start/stop is in progress, consume PREV/NEXT/ACTIVATE without leaving. On `ACTIVATE` in an exit-allowed error state, send the start intent again.

- [ ] **Step 6: Add lifecycle safety cleanup**

Before `app_settings_reset()` clears `s_menu_active`, call `app_web_file_request_stop()`. Treat `ESP_OK` and `ESP_ERR_INVALID_STATE` as safe idempotent outcomes; propagate other failures so the existing navigation gate does not claim cleanup succeeded.

This covers screen destruction, leaving the entire Settings page, and menu close even if a UI transition bypasses the Web child Back handler.

- [ ] **Step 7: Refresh the active subpage on Presentation updates**

Add:

```c
case SETTINGS_LOCATION_WEB_FILE:
    render_web_file(ESP_OK);
    break;
```

to `ui_settings_page_update()`, and route actions to `handle_web_file_action()`. Ensure `ui_settings_page_deinit()` does not directly call Application APIs; product cleanup remains in `app_settings_reset()`.

- [ ] **Step 8: Update UI documentation and inspect intent flow**

Run:

```powershell
rg -n "SETTINGS_(ITEM|LOCATION)_WEB_FILE|START_WEB_FILE|STOP_WEB_FILE|render_web_file|web_file_exit_pending" .\main
rg -n "app_web_file_request_stop" .\main\application\app_settings.c .\main\app_main.c
git diff --check
```

Expected: enter starts, Back stops and waits, and settings reset supplies a second product-level cleanup gate.

- [ ] **Step 9: Commit device UI integration**

```powershell
git add -- main/ui/include/ui_runtime.h main/app_main.c main/application/app_settings.c main/ui/pages/ui_settings_page.c main/ui/README.md
git commit -m "feat(web-file): 添加设备端文件管理入口"
```

---

### Task 9: Enable UTF-8 FatFs and complete verification

**Files:**
- Modify: `sdkconfig.defaults`
- Modify: `sdkconfig.ci`
- Modify: `sdkconfig`
- Modify: `README.md`
- Modify: `components/services/web_file_service/README.md`
- Modify: `main/application/README.md`
- Modify: `main/presentation/README.md`
- Modify: `main/ui/README.md`

**Interfaces:**
- Consumes: all previous tasks.
- Produces: consistent UTF-8 firmware configuration, final architecture/user documentation, static and hardware verification record.

- [ ] **Step 1: Make all tracked SDK configurations explicit**

In `sdkconfig.defaults` and `sdkconfig.ci` add:

```text
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y
# CONFIG_FATFS_API_ENCODING_ANSI_OEM is not set
# CONFIG_HTTPD_WS_SUPPORT is not set
CONFIG_HTTPD_MAX_URI_LEN=2048
```

In current `sdkconfig`, replace:

```text
CONFIG_FATFS_API_ENCODING_ANSI_OEM=y
# CONFIG_FATFS_API_ENCODING_UTF_8 is not set
```

with:

```text
# CONFIG_FATFS_API_ENCODING_ANSI_OEM is not set
CONFIG_FATFS_API_ENCODING_UTF_8=y
```

Keep `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255`, `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y`, and WebSocket disabled. Do not change the existing sector size or codepage choice unless the unified build regenerates it from Kconfig.

The 2048-byte URI bound accommodates a 512-byte logical path after worst-case percent encoding plus the `/api/file?path=` prefix. Request handlers must not place a 2048-byte query array on the HTTPD Task stack; they allocate the exact query length from bounded internal heap and free it in cleanup.

- [ ] **Step 2: Complete reader-facing documentation**

Update root and layer READMEs with this exact flow:

```text
设备设置页选择“网页文件管理”
  → app_web_file 检查 /sdcard
  → app_network 授予 APP_NETWORK_LEASE_WEB_FILE
  → web_file_service 恢复事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → handler 串行浏览、下载或事务上传
  → 设备返回时 Service 安全停止后释放网络租约
```

State explicitly that configuration editing, delete/rename/mkdir, WebDAV and WebSocket remain outside this phase.

- [ ] **Step 3: Run the approved non-compiling verification**

Run:

```powershell
python .\components\services\web_file_service\scripts\test_build_html.py -v
rg -n "^CONFIG_FATFS_API_ENCODING_UTF_8=y$" sdkconfig sdkconfig.defaults sdkconfig.ci
rg -n "^# CONFIG_HTTPD_WS_SUPPORT is not set$" sdkconfig sdkconfig.defaults sdkconfig.ci
rg -n "^CONFIG_HTTPD_MAX_URI_LEN=2048$" sdkconfig sdkconfig.defaults sdkconfig.ci
rg -n "heap_caps_malloc\\(32U \\* 1024U, MALLOC_CAP_SPIRAM \\| MALLOC_CAP_8BIT\\)" .\components\services\web_file_service
rg -n "WebSocket|WebDAV|multipart/form-data|jszip" .\components\services\web_file_service\web .\components\services\web_file_service\src
git diff --check
git status --short
```

Expected:

- Python generator tests pass;
- UTF-8 is present in all three configurations;
- WebSocket is disabled in all three configurations;
- the HTTPD URI bound is 2048 in all three configurations;
- the exact PSRAM allocation appears;
- the browser/source scan finds no prohibited transport or dependency;
- diff whitespace check emits no output;
- status shows only task files plus the pre-existing unrelated worktree changes.

- [ ] **Step 4: Perform the compile gate only with explicit authorization**

If and only if the user explicitly requests compilation, run:

```powershell
& .\dm.ps1 build
```

Expected: unified ESP-IDF build succeeds for the configured `esp32s3` target. If the script, fixed toolchain or environment fails, stop immediately and report the exact output; do not invoke a lower-level build command.

- [ ] **Step 5: Execute the hardware acceptance checklist**

On a device with an SD card and existing Wi-Fi:

1. enter Settings → Web File Management and observe storage/network/start phases;
2. verify the shown URL uses the current STA IPv4 and the code has six digits including possible leading zeros;
3. log in from one browser and confirm a second fresh browser is rejected;
4. browse root, normal hidden entries, Chinese directories, spaces, `%`, `#`, and multi-byte boundary names;
5. confirm `.deskmate-web` is absent and direct encoded access receives `400` or `404`;
6. download representative text/image/binary files and compare sizes and hashes;
7. upload 0-byte, 32 KiB, non-block-aligned and 500 MiB files and compare hashes;
8. confirm 500 MiB + 1 returns `413`, insufficient space returns `507`, and unconfirmed overwrite returns `409`;
9. confirm an approved overwrite preserves the old file until the new file is durable;
10. interrupt Wi-Fi/power at each journal phase and verify deterministic recovery;
11. verify upload progress, UI responsiveness, one-transfer busy response and ten-minute idle expiration;
12. verify the active lease blocks light sleep, Portal, voice lease and OTA/sync;
13. leave the device child page, observe closing state, and verify HTTPD Task, sockets, file handles, PSRAM buffer, secrets and lease are released;
14. reconnect Wi-Fi while Service remains enabled and verify the device URL updates without changing the access code.

Record heap/PSRAM free size and historical minimum during the 500 MiB upload.

- [ ] **Step 6: Commit configuration and final documentation**

```powershell
git add -- sdkconfig sdkconfig.defaults sdkconfig.ci README.md components/services/web_file_service/README.md main/application/README.md main/presentation/README.md main/ui/README.md
git commit -m "docs(web-file): 完成文件管理配置与验证说明"
```

---

## Final Review Gate

- [ ] Confirm `git log --oneline -9` contains one focused commit per task and every description is Chinese.
- [ ] Confirm `git diff dev~9..HEAD --name-only` contains no pre-existing unrelated file except files explicitly listed by this plan.
- [ ] Confirm public headers contain Chinese Doxygen `@brief`, parameter directions and returns.
- [ ] Confirm no access code, token, full file content or private path is written to logs.
- [ ] Confirm every handler authenticates before filesystem metadata access and releases handler/transfer state through one cleanup path.
- [ ] Confirm every upload error leaves either the original target intact or an unambiguous recovery journal.
- [ ] Confirm Service stop does not release the Web network lease until HTTPD, handlers, file handles and the PSRAM buffer are gone.
- [ ] Confirm no direct `idf.py`, `cmake`, `ninja`, Bash, WebDAV or WebSocket command/dependency was introduced.
