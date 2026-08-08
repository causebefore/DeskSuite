# PhotoPainter Web Console Files Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 DeskMate 已有的共享 Files-only Web Console 安全接入 PhotoPainter，只开放 `/sdcard/user`，并由现有电源状态机独占编排网络、SD、休眠和照片恢复。

**Architecture:** `photo_playback_app` 只把右键三秒长按转换成异步请求；`power_management_app` 的唯一 Task 等待产品状态收敛并启停 Web Console。共享 Service 继续只拥有认证、HTTP 和 Files，PhotoPainter 通过带代次的 SD 文件系统租约保证挂载稳定，并通过产品配置把浏览器根限制为 `/sdcard/user`。

**Tech Stack:** ESP-IDF 5.x、C/C++、FreeRTOS、FATFS、ESP HTTP Server、共享 `web_console_service`、Python `unittest`、PowerShell `ds.ps1`。

## Global Constraints

- 只启用 `CONFIG_WEB_CONSOLE_FILES=y`；`SETTINGS`、`STATUS`、`ACTIONS` 均为 `n`。
- 浏览器物理根固定为 `/sdcard/user`，事务目录固定为 `.photopainter-web`。
- 单文件上传上限固定为 500 MiB，提交后最少保留 8 MiB 可用空间。
- HTTP 端口固定为 80，`CONFIG_HTTPD_MAX_URI_LEN=2048`。
- 右键短按继续翻到下一张；只有右键持续至少 3 秒并结束才提交 Web Console 请求。
- 会话从成功进入 `RUNNING` 起最多运行 10 分钟；左键可提前退出。
- 停止顺序必须是 Console stop/deinit → SD 租约 release → network stop → 照片恢复/休眠。
- 访问码只在本地屏幕显示，禁止写日志、持久化或发送到 Hub。
- 不实现图片导入、PPF2 转换、Wi-Fi 设置、Portal 合并、OTA 发布、烧录或设备安装。
- 所有固件编译只能从仓库根执行 `& .\ds.ps1 build photopainter`。
- 保留工作区现有 Hub 修改；每次只暂存当前任务文件和代码块。

---

## File Structure

### Shared Web Console

- Modify: `shared/components/services/web_console_service/web/modules/files.js` — 把产品特定的“SD 卡”客户文案改为“设备存储”。
- Modify: `shared/components/services/web_console_service/scripts/test_build_html.py` — 验证 Files-only 组合包含通用文案且不包含旧文案。

### PhotoPainter storage

- Modify: `devices/photopainter/components/device/device_sd/include/device_sd.h` — 增加容量快照类型和同步读取 API。
- Modify: `devices/photopainter/components/device/device_sd/src/device_sd.c` — 在既有存储互斥量内读取 FATFS 总容量与可用空间。
- Modify: `devices/photopainter/components/device/device_sd/CMakeLists.txt` — 增加 FATFS 私有依赖。
- Modify: `devices/photopainter/components/services/sd_card_service/include/sd_card_service.h` — 增加文件系统租约、卡变化事件和回调 API。
- Modify: `devices/photopainter/components/services/sd_card_service/src/sd_card_service.c` — 实现租约代次、操作串行、延后卸载和事件发布。
- Modify: `devices/photopainter/components/services/sd_card_service/src/sd_card_service_task.c` — 保持 ISR 只唤醒 Task，由 Task 执行收敛和事件发布。
- Modify: `devices/photopainter/components/services/sd_card_service/src/sd_card_service_internal.h` — 补齐私有收敛契约。

### PhotoPainter display recovery and button intent

- Modify: `devices/photopainter/components/sys/include/system_storage.h` — 将 OTA 专用恢复标记公共 API 泛化为临时页面恢复标记。
- Modify: `devices/photopainter/components/sys/system_storage.c` — 保留兼容 NVS key，修改公共语义和日志。
- Modify: `devices/photopainter/components/application/bootstart_app/bootstart_app.c` — 启动时按通用标记恢复活动照片。
- Modify: `devices/photopainter/components/application/photo_playback_app/include/photo_playback_app.h` — 增加 Web Console 请求回调类型和 setter。
- Modify: `devices/photopainter/components/application/photo_playback_app/src/photo_playback_app_internal.hpp` — 增加请求通知位、右键按下时间和回调借用字段。
- Modify: `devices/photopainter/components/application/photo_playback_app/src/photo_playback_app.cpp` — 管理新回调生命周期。
- Modify: `devices/photopainter/components/application/photo_playback_app/src/photo_playback_app_task.cpp` — 实现右键三秒判定和请求分发。

### PhotoPainter product assembly and orchestration

- Modify: `devices/photopainter/CMakeLists.txt` — 加入共享 `web_console_service` 组件目录。
- Modify: `devices/photopainter/sdkconfig.defaults` — 固定 Files-only 裁剪和 URI 长度。
- Modify: `devices/photopainter/components/application/power_management_app/CMakeLists.txt` — 加入新源文件及 `web_console_service` 私有依赖。
- Create: `devices/photopainter/components/application/power_management_app/src/power_management_app_web_console.hpp` — 声明产品会话资源和技术启停接口。
- Create: `devices/photopainter/components/application/power_management_app/src/power_management_app_web_console.cpp` — 装配用户根、容量回调、网络和共享 Service。
- Modify: `devices/photopainter/components/application/power_management_app/include/power_management_app.h` — 增加 Web Console 公共状态阶段。
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app_internal.hpp` — 增加通知位和通用模态入口。
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app.cpp` — 注册请求、SD 事件和网络变化回调。
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app_task.cpp` — 把维护会话并入现有唯一 Task 状态机。

### Tests and docs

- Create: `devices/photopainter/tests/test_web_console_contract.py` — 只验证可由源码和构建配置证明的产品契约，不冒充实机测试。
- Modify: `shared/components/services/web_console_service/README.md`
- Modify: `devices/photopainter/README.md`
- Modify: `devices/photopainter/components/services/sd_card_service/README.md`
- Modify: `devices/photopainter/components/application/photo_playback_app/README.md`
- Modify: `devices/photopainter/components/application/power_management_app/README.md`
- Modify: `devices/photopainter/components/application/bootstart_app/README.md`

---

### Task 1: Generalize shared Files customer copy

**Files:**
- Modify: `shared/components/services/web_console_service/scripts/test_build_html.py:291-346`
- Modify: `shared/components/services/web_console_service/web/modules/files.js:503,545`

**Interfaces:**
- Consumes: `build_html.assemble_html(INDEX_TEMPLATE, ("files",))`
- Produces: Files 页面统一使用“设备存储”，不再包含“SD 卡”客户文案。

- [ ] **Step 1: Write the failing test**

在 `test_files_module_keeps_existing_feature_contracts()` 增加：

```python
self.assertIn("当前文件存在于设备存储。", html)
self.assertIn("当前文件在设备存储中不存在，请重新选择。", html)
self.assertNotIn("SD 卡", html)
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
py shared/components/services/web_console_service/scripts/test_build_html.py -v
```

Expected: FAIL，缺少“设备存储”并仍包含“SD 卡”。

- [ ] **Step 3: Implement the minimal copy change**

在 `files.js` 中只替换两处客户文案：

```javascript
setStatus("当前文件存在于设备存储。", "available");
```

```javascript
exists ? "当前文件存在于设备存储。" : "当前文件在设备存储中不存在，请重新选择。"
```

- [ ] **Step 4: Run the full shared asset test**

Run the same Python command. Expected: all tests PASS。

- [ ] **Step 5: Commit**

```powershell
git add -- shared/components/services/web_console_service/web/modules/files.js shared/components/services/web_console_service/scripts/test_build_html.py
git commit -m "fix(web-console): 通用化文件存储文案"
```

### Task 2: Add bounded SD capacity snapshots

**Files:**
- Create: `devices/photopainter/tests/test_web_console_contract.py`
- Modify: `devices/photopainter/components/device/device_sd/include/device_sd.h`
- Modify: `devices/photopainter/components/device/device_sd/src/device_sd.c`
- Modify: `devices/photopainter/components/device/device_sd/CMakeLists.txt`

**Interfaces:**
- Produces:

```c
typedef struct
{
    uint64_t total_bytes;
    uint64_t free_bytes;
} device_sd_capacity_snapshot_t;

esp_err_t device_sd_read_capacity_snapshot(device_sd_capacity_snapshot_t *out_snapshot);
```

- [ ] **Step 1: Write the failing source contract**

创建 `test_web_console_contract.py`，使用 `unittest` 和仓库根相对路径读取源码：

```python
class DeviceSdCapacityContractTest(unittest.TestCase):
    def test_capacity_snapshot_is_owned_by_device_sd(self):
        header = read("devices/photopainter/components/device/device_sd/include/device_sd.h")
        source = read("devices/photopainter/components/device/device_sd/src/device_sd.c")
        cmake = read("devices/photopainter/components/device/device_sd/CMakeLists.txt")
        self.assertIn("device_sd_capacity_snapshot_t", header)
        self.assertIn("device_sd_read_capacity_snapshot", header)
        self.assertIn("esp_vfs_fat_info", source)
        self.assertIn("fatfs", cmake)
```

Helper `read()` 必须由 `Path(__file__).resolve().parents[3]` 定位仓库根，不依赖当前目录。

- [ ] **Step 2: Run and verify RED**

```powershell
py devices/photopainter/tests/test_web_console_contract.py -v
```

Expected: FAIL，容量类型和 API 尚不存在。

- [ ] **Step 3: Implement the device API**

`device_sd_read_capacity_snapshot()` 必须：

1. 拒绝空输出和未初始化状态；
2. 取得现有 `s_storage_mutex`；
3. 拒绝未挂载状态；
4. 调用 `esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total_bytes, &free_bytes)`；
5. 校验 `free_bytes <= total_bytes` 后完整填写输出；
6. 所有返回路径释放互斥量。

`CMakeLists.txt` 的 `PRIV_REQUIRES` 增加 `fatfs`。

- [ ] **Step 4: Run the source contract**

Run the same Python command. Expected: PASS。

- [ ] **Step 5: Commit**

```powershell
git add -- devices/photopainter/tests/test_web_console_contract.py devices/photopainter/components/device/device_sd
git commit -m "feat(photopainter): 增加SD容量快照"
```

### Task 3: Add a generation-safe SD filesystem lease

**Files:**
- Modify: `devices/photopainter/tests/test_web_console_contract.py`
- Modify: `devices/photopainter/components/services/sd_card_service/include/sd_card_service.h`
- Modify: `devices/photopainter/components/services/sd_card_service/src/sd_card_service.c`
- Modify: `devices/photopainter/components/services/sd_card_service/src/sd_card_service_task.c`
- Modify: `devices/photopainter/components/services/sd_card_service/src/sd_card_service_internal.h`

**Interfaces:**
- Produces:

```c
typedef struct
{
    uint32_t generation;
} sd_card_service_filesystem_lease_t;

typedef struct
{
    bool card_present;
    bool mounted;
    esp_err_t reconcile_error;
} sd_card_service_card_event_t;

typedef void (*sd_card_service_card_event_cb_t)(
    const sd_card_service_card_event_t *event, void *context);

esp_err_t sd_card_service_set_card_event_callback_borrow(
    sd_card_service_card_event_cb_t callback, void *context);
esp_err_t sd_card_service_acquire_filesystem_lease(
    sd_card_service_filesystem_lease_t *out_lease);
esp_err_t sd_card_service_release_filesystem_lease(
    sd_card_service_filesystem_lease_t lease);
```

- [ ] **Step 1: Extend the failing contract**

断言公共头包含上述类型和三个 API；实现同时包含：

```python
self.assertIn("s_operation_mutex", source)
self.assertIn("filesystem_lease_generation", source)
self.assertIn("filesystem_lease_active", source)
self.assertIn("removal_pending", source)
```

并断言 `sd_card_service_stop()` 在活动租约下返回 `ESP_ERR_INVALID_STATE`。

- [ ] **Step 2: Run and verify RED**

Run the product contract test. Expected: FAIL，租约尚不存在。

- [ ] **Step 3: Implement serialized lease ownership**

实现规则：

- `start()` 创建 `s_operation_mutex` 后再执行首次收敛和启动监测 Task；失败完整回滚。
- `reconcile_card()` 在操作互斥量内读取卡状态和执行 mount/unmount。
- 无卡且租约活动时只设置 `removal_pending=true`，不调用 `device_sd_unmount()`。
- 每次物理插卡事实变化后，在锁外调用借用事件回调；回调只得到按值构造的不可变事件。
- `acquire` 只接受 Service 已运行、卡存在、已挂载且无活动租约；递增非零 generation。
- `release` 必须匹配 generation；清除租约后立即重新执行一次收敛，使待卸载事实完成。
- `stop` 在租约活动时拒绝；成功停止时清除回调、删除操作互斥量和运行状态。
- callback/context 只在短临界区复制，绝不在锁内回调产品代码。

- [ ] **Step 4: Run the product contract**

Expected: PASS。

- [ ] **Step 5: Commit**

```powershell
git add -- devices/photopainter/tests/test_web_console_contract.py devices/photopainter/components/services/sd_card_service
git commit -m "feat(photopainter): 增加SD文件系统租约"
```

### Task 4: Generalize the retained display-restore marker

**Files:**
- Modify: `devices/photopainter/tests/test_web_console_contract.py`
- Modify: `devices/photopainter/components/sys/include/system_storage.h`
- Modify: `devices/photopainter/components/sys/system_storage.c`
- Modify: `devices/photopainter/components/application/bootstart_app/bootstart_app.c`
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app_task.cpp`

**Interfaces:**
- Produces:

```c
esp_err_t system_storage_set_display_restore_pending(bool pending);
esp_err_t system_storage_get_display_restore_pending(bool *out_pending);
```

- [ ] **Step 1: Write the failing rename contract**

断言新公共 API 出现在头文件和三个调用方中，并断言项目源码不再引用：

```text
system_storage_set_ota_display_restore_pending
system_storage_get_ota_display_restore_pending
```

- [ ] **Step 2: Run and verify RED**

Expected: FAIL，当前接口仍为 OTA 专用命名。

- [ ] **Step 3: Rename without a forwarding wrapper**

- 保留 NVS key 字符串 `ota_disp_rstr`，以兼容已部署设备；只把常量名、公共 API、注释和日志泛化。
- 不保留旧 API 宏或包装函数，因为仓库内没有外部 ABI 消费者。
- `bootstart_app` 读取异常时继续保守恢复活动照片；第一张物理显示成功后才清除标记。
- OTA 页面继续使用同一通用标记，行为不变。

- [ ] **Step 4: Run the contract**

Expected: PASS。

- [ ] **Step 5: Commit**

```powershell
git add -- devices/photopainter/tests/test_web_console_contract.py devices/photopainter/components/sys/include/system_storage.h devices/photopainter/components/sys/system_storage.c devices/photopainter/components/application/bootstart_app/bootstart_app.c devices/photopainter/components/application/power_management_app/src/power_management_app_task.cpp
git commit -m "refactor(photopainter): 泛化临时页面恢复标记"
```

### Task 5: Turn right-button hold into a Web Console request

**Files:**
- Modify: `devices/photopainter/tests/test_web_console_contract.py`
- Modify: `devices/photopainter/components/application/photo_playback_app/include/photo_playback_app.h`
- Modify: `devices/photopainter/components/application/photo_playback_app/src/photo_playback_app_internal.hpp`
- Modify: `devices/photopainter/components/application/photo_playback_app/src/photo_playback_app.cpp`
- Modify: `devices/photopainter/components/application/photo_playback_app/src/photo_playback_app_task.cpp`

**Interfaces:**
- Produces:

```c
typedef esp_err_t (*photo_playback_app_web_console_request_cb_t)(void *context);

esp_err_t photo_playback_app_set_web_console_request_callback_borrow(
    photo_playback_app_web_console_request_cb_t callback, void *context);
```

- [ ] **Step 1: Write the failing button contract**

断言：

- 公共 callback/setter 存在；
- `PHOTO_PLAYBACK_WEB_CONSOLE_HOLD_US` 等于 `3000000LL`；
- Runtime 有独立 `right_press_started_at_us`；
- 右键 `CLICK` 仍调用 `photo_playback_app_request_next()`；
- 模态分支在长按判断前返回，保证维护会话消费所有长按。

- [ ] **Step 2: Run and verify RED**

Expected: FAIL，新请求契约尚不存在。

- [ ] **Step 3: Implement the request path**

- 为 Photo Task 增加 `PHOTO_PLAYBACK_NOTIFY_WEB_CONSOLE_REQUEST`。
- 非模态右键 `PRESS` 记录单调时间，`RELEASE` 清零，`LONG_PRESS_END` 只有达到三秒才通知 Photo Task。
- Photo Task 在普通 Task 上下文复制 callback/context、锁外快速调用并记录拒绝日志。
- `begin_modal`、`end_modal`、task start 和 deinit 同时清零左右键按下时间。
- init/deinit 管理 callback/context；setter 允许传 `NULL` 清除。
- 不把网络、SD、HTTP 或 Power Application 头文件加入 Photo Application。

- [ ] **Step 4: Run the product contract**

Expected: PASS。

- [ ] **Step 5: Commit**

```powershell
git add -- devices/photopainter/tests/test_web_console_contract.py devices/photopainter/components/application/photo_playback_app
git commit -m "feat(photopainter): 增加文件管理长按请求"
```

### Task 6: Assemble the Files-only product session

**Files:**
- Modify: `devices/photopainter/tests/test_web_console_contract.py`
- Modify: `devices/photopainter/CMakeLists.txt`
- Modify: `devices/photopainter/sdkconfig.defaults`
- Modify: `devices/photopainter/components/application/power_management_app/CMakeLists.txt`
- Create: `devices/photopainter/components/application/power_management_app/src/power_management_app_web_console.hpp`
- Create: `devices/photopainter/components/application/power_management_app/src/power_management_app_web_console.cpp`

**Interfaces:**
- Produces:

```cpp
struct PowerWebConsoleSession
{
    bool service_initialized = false;
    bool service_running = false;
    bool network_held = false;
    bool filesystem_lease_held = false;
    sd_card_service_filesystem_lease_t filesystem_lease = {};
};

struct PowerWebConsoleAccess
{
    char ip[16];
    char access_code[7];
};

esp_err_t power_management_web_console_start(
    PowerWebConsoleSession *session, PowerWebConsoleAccess *out_access);
esp_err_t power_management_web_console_stop(PowerWebConsoleSession *session);
bool power_management_web_console_has_resources(const PowerWebConsoleSession &session);
```

- [ ] **Step 1: Write failing assembly contracts**

验证：

- 根 CMake 包含 `../../shared/components/services/web_console_service`；
- defaults 包含 Files `y`，Settings/Status/Actions `n` 和 URI 2048；
- Helper 常量精确包含 `/sdcard/user`、`.photopainter-web`、`500ULL * 1024ULL * 1024ULL`、
  `8ULL * 1024ULL * 1024ULL` 和端口 `80U`；
- Power CMake 编译新源并依赖 `web_console_service`。

- [ ] **Step 2: Run and verify RED**

Expected: FAIL，产品装配尚不存在。

- [ ] **Step 3: Implement start and stop helpers**

`start()` 固定顺序：

```text
acquire filesystem lease
→ device_sd_make_directory("user")
→ network_manager_start()
→ bounded wait for NETWORK_STATE_ONLINE
→ web_console_service_init_borrow(files-only config)
→ web_console_service_start()
→ network_manager_get_diagnostics_copy() + web_console_service_get_status_copy()
→ copy IP/access code to out_access
```

容量 adapter 把 `device_sd_capacity_snapshot_t` 按值映射到
`web_console_files_capacity_t`。任何启动失败保留 `session` 中尚未清理的真实所有权，调用方随后用
同一个 `stop()` 收敛。

`stop()` 固定顺序：

```text
web_console_service_stop(5000)
→ web_console_service_deinit()
→ sd_card_service_release_filesystem_lease(generation)
→ network_manager_stop()
```

`ESP_ERR_INVALID_STATE` 只在状态快照证明资源已经停止时归一为成功；超时或清理失败保留对应 bool，
让下一次调用继续收敛。函数结束前覆盖临时访问码副本。

- [ ] **Step 4: Run product contracts**

Expected: PASS。

- [ ] **Step 5: Commit**

```powershell
git add -- devices/photopainter/tests/test_web_console_contract.py devices/photopainter/CMakeLists.txt devices/photopainter/sdkconfig.defaults devices/photopainter/components/application/power_management_app
git commit -m "feat(photopainter): 装配文件管理会话"
```

### Task 7: Integrate the session into the power state machine

**Files:**
- Modify: `devices/photopainter/tests/test_web_console_contract.py`
- Modify: `devices/photopainter/components/application/power_management_app/include/power_management_app.h`
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app_internal.hpp`
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app.cpp`
- Modify: `devices/photopainter/components/application/power_management_app/src/power_management_app_task.cpp`

**Interfaces:**
- Consumes: Task 3 lease/event APIs、Task 5 request callback、Task 6 session helper。
- Produces: 公共状态：

```c
POWER_MANAGEMENT_APP_STATE_WEB_CONSOLE_PENDING
POWER_MANAGEMENT_APP_STATE_WEB_CONSOLE_STARTING
POWER_MANAGEMENT_APP_STATE_WEB_CONSOLE_RUNNING
POWER_MANAGEMENT_APP_STATE_WEB_CONSOLE_STOPPING
POWER_MANAGEMENT_APP_STATE_WEB_CONSOLE_RESULT_PAGE
```

- [ ] **Step 1: Write failing orchestration contracts**

验证：

- Power start 注册 `photo_playback_app_set_web_console_request_callback_borrow` 和
  `sd_card_service_set_card_event_callback_borrow`；clear path 对称解除。
- 通知位包括 Web Console 请求、SD 变化和 Network 变化。
- 主循环包含 10 分钟运行期限、1 秒停止重试和 30 秒错误结果页期限。
- helper stop 成功前不存在 `device_power_start_deep_sleep()` 可达的 Web Console 分支。
- access code 显示后调用 `explicit_bzero` 或等价 volatile 覆盖函数。

- [ ] **Step 2: Run and verify RED**

Expected: FAIL，Power 状态机尚未接入。

- [ ] **Step 3: Add callbacks and mutual exclusion**

- Web 请求回调只通知 Power Task。
- SD 事件回调只复制 `card_present` 事实并通知 Power Task。
- Network 回调继续释放 OTA 等待信号量，并额外通知 Power Task 重新读取状态。
- OTA、Web Console、手动休眠和内容刷新必须互斥；任一交互活跃时拒绝另一个。
- 将 OTA 专用 `begin_ota_modal_capture` 重命名为通用模态捕获入口，不保留包装层。

- [ ] **Step 4: Add the Web Console phases**

内部使用：

```cpp
enum class PowerWebConsoleInteraction : uint8_t
{
    Inactive = 0,
    Pending,
    Starting,
    Running,
    Stopping,
    ResultPage,
    Blocked,
};
```

状态流：

1. 请求到达后进入 `Pending`，关闭普通 awake deadline，启用模态按键捕获。
2. 复用 OTA 的内容/显示收敛门控；就绪后停止空闲 content、设置通用恢复标记并显示启动页。
3. 调用 Task 6 helper；成功后显示 IP/验证码、覆盖访问码副本并启动 10 分钟期限。
4. 左键或期限到达进入 `Stopping`；断网、拔卡或启动失败记录客户错误原因后进入同一路径。
5. helper stop 失败时每秒重试，保持唤醒、模态和全部所有权。
6. 正常手动退出恢复照片并重开 180 秒窗口；正常超时恢复照片后按原绝对计划睡眠。
7. 启动失败或断网显示 30 秒非秘密结果页，随后恢复照片；左键可提前关闭结果页。
8. 拔卡先显示不含验证码的错误页并保持恢复标记，随后走失败退避深睡；下一次启动由
   `display_collection_service_init()` 从当前卡重新恢复，禁止复用旧内存路径。

- [ ] **Step 5: Run product contracts**

Expected: PASS。

- [ ] **Step 6: Commit**

```powershell
git add -- devices/photopainter/tests/test_web_console_contract.py devices/photopainter/components/application/power_management_app devices/photopainter/components/application/photo_playback_app
git commit -m "feat(photopainter): 编排文件管理维护会话"
```

### Task 8: Documentation, static verification, and firmware build

**Files:**
- Modify: `shared/components/services/web_console_service/README.md`
- Modify: `devices/photopainter/README.md`
- Modify: `devices/photopainter/components/services/sd_card_service/README.md`
- Modify: `devices/photopainter/components/application/photo_playback_app/README.md`
- Modify: `devices/photopainter/components/application/power_management_app/README.md`
- Modify: `devices/photopainter/components/application/bootstart_app/README.md`
- Modify: `devices/photopainter/tests/test_web_console_contract.py`

**Interfaces:**
- Consumes: Tasks 1–7 的最终 API 和行为。
- Produces: 可审查的当前事实、自动化结果和固件构建证据。

- [ ] **Step 1: Add failing documentation assertions**

产品契约测试断言文档包含：

```text
右键长按三秒
/sdcard/user
500 MiB
8 MiB
十分钟
不自动加入照片集合
```

并断言旧的“OTA 状态画面恢复标记”专用表述已泛化。

- [ ] **Step 2: Run and verify RED**

Expected: FAIL，文档尚未同步。

- [ ] **Step 3: Update factual docs**

- PhotoPainter 根 README 增加进入方法、访问方式、文件范围和不自动导入边界。
- Photo Playback README 更新完整按键表。
- Power README 记录新状态、独占资源、十分钟期限和停止顺序。
- SD README 记录租约代次、拔卡延后卸载和 callback 上下文。
- Bootstart README 把恢复标记描述为任意临时模态页。
- Shared README 把 Files 存储描述保持产品无关。

- [ ] **Step 4: Run all host-side checks**

```powershell
py shared/components/services/web_console_service/scripts/test_build_html.py -v
py devices/photopainter/tests/test_web_console_contract.py -v
py -m pytest build_tools/tests -q
git diff --check
```

Expected: all PASS；`git diff --check` 退出 0。现有 Hub 改动若产生单独提示，只报告且不修改。

- [ ] **Step 5: Build the authorized firmware**

```powershell
& .\ds.ps1 build photopainter
```

Expected: build exits 0。随后核对：

```powershell
Select-String -LiteralPath 'devices\photopainter\sdkconfig' -Pattern 'CONFIG_WEB_CONSOLE_(FILES|SETTINGS|STATUS|ACTIONS)|CONFIG_HTTPD_MAX_URI_LEN'
Select-String -LiteralPath 'devices\photopainter\build\compile_commands.json' -Pattern 'web_console_service_(read|transfer|mutation)|web_console_provider'
Select-String -LiteralPath 'devices\photopainter\build\PhotoPainter_Device.map' -Pattern 'web_console_service|web_console_provider'
```

证明 Files 源进入构建，Provider/Settings/Status 未进入；记录 app 二进制大小和剩余分区空间。若
构建失败，按 `superpowers:systematic-debugging` 查根因，禁止绕过 `ds.ps1`。

- [ ] **Step 6: Commit docs and final verification contract**

```powershell
git add -- shared/components/services/web_console_service/README.md devices/photopainter/README.md devices/photopainter/components/services/sd_card_service/README.md devices/photopainter/components/application/photo_playback_app/README.md devices/photopainter/components/application/power_management_app/README.md devices/photopainter/components/application/bootstart_app/README.md devices/photopainter/tests/test_web_console_contract.py
git commit -m "docs(photopainter): 记录文件管理运行边界"
```

- [ ] **Step 7: Final scope audit**

```powershell
git log --oneline fcb0fa6..HEAD
git status --short
git diff fcb0fa6..HEAD --stat
```

只把 PhotoPainter、共享 Files 文案、测试和文档列为本任务成果；Hub 现有未提交内容必须继续保持
未暂存。最终报告分别列出：自动化、固件构建、OTA 发布、设备安装、实体功能验收五种状态。
