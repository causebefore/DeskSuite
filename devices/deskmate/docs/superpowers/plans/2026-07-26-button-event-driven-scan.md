# 按键事件触发扫描 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 `button_service` 清醒空闲时每 10 ms 的永久周期唤醒，同时完整保留左右键短按、长按、消抖、双键并发事件和 Light Sleep 按键唤醒。

**Architecture:** BSP 用 GPIO 双边沿 ISR 只上报“按键活动”；Device 继续拥有双键状态机并返回最多两个事件及是否仍需推进；Button Service 用单个 one-shot `esp_timer` 在交互窗口内每 10 ms 扫描，稳定后停表。Light Sleep 返回后由 `app_power` 按值提交 EXT1 唤醒按键事实，补齐睡眠期间不会重放的 GPIO 边沿。

**Tech Stack:** ESP-IDF 6.0.1、C、FreeRTOS 临界区/信号量、GPIO ISR、one-shot `esp_timer`、PowerShell 静态契约检查。

## Global Constraints

- 只修改按键扫描调度及 Light Sleep 按键事实衔接；环境采样继续保持电池 2000 ms、温湿度 30000 ms 周期。
- OTA Timer、Firmware OTA 初始化/启动和 Network Task 生命周期本次不修改。
- `button_service` 不创建独立 Task，因此本次只减少空闲 CPU 唤醒，不宣称减少 Task 栈。
- `button_service_event_cb_t` 继续在 ESP Timer Task 上下文同步执行；GPIO ISR 不调用 Application、不打普通日志。
- Light Sleep 阶段 1 继续让 Button Service 保持 RUNNING，不在睡眠前 `stop()`、唤醒后 `start()`。
- 不修改已有用户改动：`.vscode/settings.json`、`AGENTS.md`、`components/bsp/src/bsp_power.c`、`components/communication/transport/src/transport_http.cpp`、`main/main.c`、`sd卡路径.md`。
- 用户未明确要求编译，因此不运行构建；只运行静态契约检查、`git diff --check` 和 Git 范围核查。
- 每个公共 API 添加中文 Doxygen；ISR 上下文、借用期限、同步停止语义和失败状态必须写清楚。

---

### Task 1: 建立按键按需扫描回归契约

**Files:**

- Create: `tools/tests/check_button_event_driven.ps1`

**Interfaces:**

- Consumes: 仓库根目录下按键 BSP、Device、Service、Application、环境和 OTA 源码。
- Produces: `-Section All|Core|Wake|Regression|Docs` 静态检查；默认 `All`。
- Exit contract: 全部断言满足返回 0；任何断言失败返回 1 并逐项打印中文原因。

- [ ] **Step 1: 写通用断言和分段入口**

脚本开头写入：

```powershell
param(
    [ValidateSet('All', 'Core', 'Wake', 'Regression', 'Docs')]
    [string]$Section = 'All'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Read-RepoFile {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("缺少待检查文件: $RelativePath")
        return ''
    }
    return Get-Content -Raw -LiteralPath $path
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ((Read-RepoFile $RelativePath) -notmatch $Pattern) {
        $failures.Add($Message)
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ((Read-RepoFile $RelativePath) -match $Pattern) {
        $failures.Add($Message)
    }
}

function Test-Section {
    param([Parameter(Mandatory = $true)][string]$Name)

    return $Section -eq 'All' -or $Section -eq $Name
}
```

- [ ] **Step 2: 写 Core、Wake、Regression 和 Docs 契约**

`Core` 必须覆盖永久周期删除、BSP 双边沿、Device 双事件结果和 Service 同步停止：

```powershell
if (Test-Section 'Core') {
    Assert-NotContains 'components\services\button_service\src\button_service.c' `
        'esp_timer_start_periodic\s*\(' 'Button Service 仍在启动永久周期 Timer'
    Assert-Contains 'components\services\button_service\src\button_service.c' `
        'esp_timer_start_once\s*\(' 'Button Service 未使用 one-shot Timer'
    Assert-Contains 'components\bsp\src\bsp_button.c' `
        'GPIO_INTR_ANYEDGE' '按键 GPIO 未配置双边沿'
    Assert-Contains 'components\bsp\src\bsp_button.c' `
        'gpio_isr_handler_add\s*\(' '按键 GPIO ISR 未注册'
    Assert-Contains 'components\bsp\src\bsp_button.c' `
        'gpio_isr_handler_remove\s*\(' '按键 GPIO ISR 未成对注销'
    Assert-Contains 'components\device\device_button\include\device_button.h' `
        'DEVICE_BUTTON_MAX_EVENTS\s+2U' 'Device 未声明双事件上限'
    Assert-Contains 'components\device\device_button\include\device_button.h' `
        'device_button_scan_result_t' 'Device 未提供有界扫描结果'
    Assert-Contains 'components\device\device_button\include\device_button.h' `
        'follow_up_required' 'Device 未返回继续推进事实'
    Assert-Contains 'components\device\device_button\include\device_button.h' `
        'device_button_set_activity_callback_borrow' 'Device 未提供活动回调入口'
    Assert-Contains 'components\services\button_service\include\button_service.h' `
        'button_service_stop\s*\(\s*uint32_t\s+timeout_ms\s*\)' `
        'Button Service 未提供带超时的同步停止'
}
```

`Wake`、`Regression` 和 `Docs` 写入：

```powershell
if (Test-Section 'Wake') {
    Assert-Contains 'components\services\button_service\include\button_service.h' `
        'button_service_wakeup_info_t' 'Button Service 未声明轻睡眠唤醒事实'
    Assert-Contains 'components\services\button_service\include\button_service.h' `
        'button_service_request_light_sleep_wakeup_copy' `
        'Button Service 未提供轻睡眠唤醒事实入口'
    Assert-Contains 'main\application\app_power_task.c' `
        'button_service_request_light_sleep_wakeup_copy\s*\(' `
        'App Power 未把 EXT1 按键事实提交给 Button Service'
}

if (Test-Section 'Regression') {
    Assert-Contains 'main\application\app_environment_task.c' `
        'ENVIRONMENT_BATTERY_SAMPLE_PERIOD_MS\s+2000U' '电池采样周期不再是 2000 ms'
    Assert-Contains 'main\application\app_environment_task.c' `
        'ENVIRONMENT_SENSOR_SAMPLE_PERIOD_MS\s+30000U' '温湿度采样周期不再是 30000 ms'
    Assert-Contains 'main\application\app_network_task.c' `
        'esp_timer_create\s*\(\s*&ota_args\s*,\s*&s_ota_timer\s*\)' 'OTA Timer 生命周期被改变'
    Assert-Contains 'main\application\app_network_task.c' `
        'firmware_ota_init\s*\(' 'Firmware OTA 初始化入口被改变'
    Assert-Contains 'main\application\app_network_task.c' `
        'firmware_ota_start\s*\(' 'Firmware OTA 启动入口被改变'
}

if (Test-Section 'Docs') {
    Assert-Contains 'components\services\button_service\README.md' `
        'one-shot|单次' 'Button Service README 未说明 one-shot 调度'
    Assert-Contains 'main\application\README.md' `
        '按键.*边沿|边沿.*按键' 'Application README 未说明按键边沿链路'
    Assert-Contains 'docs\低功耗流程.md' `
        'button_service_request_light_sleep_wakeup_copy' `
        '低功耗文档未说明 EXT1 按键事实桥接'
}
```

脚本结尾统一输出：

```powershell
if ($failures.Count -gt 0) {
    Write-Host '按键事件触发扫描契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "按键事件触发扫描契约检查通过：$Section" -ForegroundColor Green
```

- [ ] **Step 3: 运行检查并确认 RED**

Run:

```powershell
& .\tools\tests\check_button_event_driven.ps1
```

Expected: exit code 1，至少报告 `esp_timer_start_periodic()`、缺少 `GPIO_INTR_ANYEDGE`、缺少
`device_button_scan_result_t` 和缺少 Light Sleep 唤醒事实入口。

不要暂存脚本；等最终所有分段变绿后与文档一起提交，避免仓库中间提交包含默认失败的检查。

---

### Task 2: 实现 BSP → Device → Service 的按需扫描核心

**Files:**

- Modify: `components/bsp/include/bsp.h:21-27, 132-145`
- Modify: `components/bsp/src/bsp_button.c:1-65`
- Modify: `components/device/device_button/include/device_button.h:17-65`
- Modify: `components/device/device_button/src/device_button.c:10-137`
- Modify: `components/services/button_service/include/button_service.h:1-58`
- Modify: `components/services/button_service/src/button_service.c:1-149`
- Modify: `components/services/button_service/CMakeLists.txt:10-13`

**Interfaces:**

- BSP produces: ISR 上下文的无参数活动事实，不暴露 GPIO 编号。
- Device produces: 最多两个稳定产品事件和 `follow_up_required`。
- Service produces: 空闲停表、活动窗口 10 ms one-shot 推进、同步超时停止。
- Service consumes: `device_button` 已由 Composition Root 初始化，事件回调为长期借用。

- [ ] **Step 1: 在 BSP 公共头声明活动回调**

在 `bsp_button_id_t` 后增加：

```c
/** @brief GPIO ISR 上下文中的按键电平活动回调 */
typedef void (*bsp_button_activity_callback_t)(void *context);
```

在现有 `bsp_button_get_level()` 后增加：

```c
/**
 * @brief 注册或清除长期借用的按键双边沿活动回调
 *
 * 非 NULL 回调运行在 GPIO ISR 上下文，只允许执行 ISR-safe 的有界操作。
 * 传入 NULL 时先关闭左右键中断，再清除借用的回调和上下文。
 *
 * @param[in] callback 活动回调；NULL 表示清除
 * @param[in] context 长期借用的回调上下文；清除时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE BSP 尚未初始化；或 GPIO 驱动错误码
 */
esp_err_t bsp_button_set_activity_callback_borrow(
    bsp_button_activity_callback_t callback,
    void *context);
```

- [ ] **Step 2: 在 BSP 注册双边沿 ISR，并保证逆序清理**

`bsp_button.c` 增加：

```c
static portMUX_TYPE                    s_callback_lock = portMUX_INITIALIZER_UNLOCKED;
static bsp_button_activity_callback_t s_activity_callback;
static void                          *s_activity_context;
static bool                           s_handlers_registered;

static void IRAM_ATTR button_gpio_isr(void *arg)
{
    (void) arg;
    bsp_button_activity_callback_t callback;
    void *context;

    taskENTER_CRITICAL_ISR(&s_callback_lock);
    callback = s_activity_callback;
    context  = s_activity_context;
    taskEXIT_CRITICAL_ISR(&s_callback_lock);
    if (callback != NULL)
    {
        callback(context);
    }
}
```

初始化顺序固定为：

1. `gpio_config()` 仍以 `GPIO_INTR_DISABLE` 配置左右键；
2. `gpio_install_isr_service(0)`，`ESP_ERR_INVALID_STATE` 视为共享 ISR Service 已存在；
3. 左键和右键分别 `gpio_isr_handler_add()`；
4. 任一失败时移除已添加 handler 并复位 GPIO；
5. handler 注册后仍保持中断关闭，直到非 NULL 活动回调注册。

回调注册时先保存回调，再对两键设置 `GPIO_INTR_ANYEDGE` 并使能中断；清除时按“禁用两键中断
→ 临界区清空回调”的顺序执行。`deinit()` 调用清除入口、成对移除两个 handler、复位 GPIO，
但不卸载共享 `gpio_isr_service`。

- [ ] **Step 3: 扩展 Device 扫描契约**

在 `device_button.h` 增加：

```c
#define DEVICE_BUTTON_MAX_EVENTS 2U

/** @brief GPIO ISR 上下文中的按键活动回调 */
typedef void (*device_button_activity_callback_t)(void *context);

/** @brief 一轮双按键扫描的有界结果 */
typedef struct
{
    device_button_event_t events[DEVICE_BUTTON_MAX_EVENTS]; /**< 本轮稳定事件 */
    uint8_t               event_count;                       /**< 有效事件数量 */
    bool                  follow_up_required;                /**< 是否仍需定时推进 */
} device_button_scan_result_t;
```

把扫描声明改为：

```c
/**
 * @brief 推进左右键状态机并复制本轮完整结果
 * @param[in] now_ms 当前单调时间，单位毫秒
 * @param[out] out_result 本轮最多两个事件和继续推进事实
 * @return ESP_OK 已完成扫描；其他值表示尚未初始化、参数错误或 BSP 读取失败
 */
esp_err_t device_button_scan(uint32_t now_ms, device_button_scan_result_t *out_result);

/**
 * @brief 注册或清除长期借用的按键活动回调
 * @param[in] callback GPIO ISR 上下文的活动回调；NULL 表示清除
 * @param[in] context 长期借用的上下文；清除时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
 */
esp_err_t device_button_set_activity_callback_borrow(
    device_button_activity_callback_t callback,
    void *context);
```

`device_button_scan()` 先将输出清零，再分别推进左右键；每个非 NONE 事件按键序写入数组。扫描
两个键后按以下函数汇总继续推进事实：

```c
static bool button_follow_up_required(const device_button_state_t *state)
{
    return state->candidate_pressed != state->stable_pressed
           || (state->stable_pressed && !state->long_sent);
}
```

活动回调 API 只做已初始化校验并转发到
`bsp_button_set_activity_callback_borrow()`。`device_button_deinit()` 必须先清除活动回调，
再释放 BSP；清除失败时不得静默释放。

- [ ] **Step 4: 将 Button Service 改为 one-shot 生命周期**

`button_service.h` 保留 `scan_period_ms` 配置字段，但注释改为“活动窗口状态机推进间隔”。更新：

```c
/** @brief 启动边沿监听并安排一次初始扫描 */
esp_err_t button_service_start(void);

/**
 * @brief 同步停止边沿监听和扫描
 *
 * ESP_OK 返回后保证没有 GPIO 活动回调、扫描回调或上层事件回调仍在执行。
 * 超时时保持 STOPPING，调用方可再次调用 stop() 收敛；start() 和 deinit() 会被拒绝。
 *
 * @param[in] timeout_ms 等待在途回调退出的超时，必须大于 0
 * @return ESP_OK 已完全停止；ESP_ERR_TIMEOUT 尚有在途回调；或状态/底层错误码
 */
esp_err_t button_service_stop(uint32_t timeout_ms);
```

`button_service.c` 使用以下私有状态，不使用不存在的 `esp_timer_stop_blocking()`：

```c
typedef enum
{
    BUTTON_SERVICE_STATE_UNINITIALIZED = 0,
    BUTTON_SERVICE_STATE_INITIALIZED,
    BUTTON_SERVICE_STATE_RUNNING,
    BUTTON_SERVICE_STATE_STOPPING,
} button_service_state_t;

typedef struct
{
    bool      edge_pending;
    bool      timer_armed;
    uint32_t  activity_inflight;
    uint32_t  schedule_inflight;
    uint32_t  timer_inflight;
    esp_err_t schedule_error;
} button_service_runtime_t;

static portMUX_TYPE            s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t       s_quiesced_signal;
static button_service_state_t  s_state;
static button_service_runtime_t s_runtime;
```

在 `button_service_init()` 中先创建二值 `s_quiesced_signal`，再创建
`ESP_TIMER_TASK` one-shot Timer；失败时逆序释放。`components/services/button_service/CMakeLists.txt`
的 `PRIV_REQUIRES` 增加 `freertos`。

实现两个调度入口：

```c
static esp_err_t schedule_scan_from_task(void);
static void IRAM_ATTR button_service_activity_isr(void *context);
```

两者都在临界区内只在 RUNNING 且 `timer_armed == false` 时把 `timer_armed` 置 true，再调用
ESP-IDF 6.0.1 的 `esp_timer_start_once(s_scan_timer, s_scan_period_ms * 1000ULL)`。每次在锁外执行
该调用前增加 `schedule_inflight`，返回后再减少，使 `stop()` 不可能越过尚未真正启动的 Timer。
ISR 路径：

- 先增加 `activity_inflight`，锁存 `edge_pending`；
- 不记录普通日志；
- 调度失败时恢复 `timer_armed` 并锁存 `schedule_error`；
- 退出前减少 `activity_inflight`，若 STOPPING 则用 ISR-safe semaphore give 唤醒停止方。

`start()` 顺序固定为：

1. 只接受 INITIALIZED；
2. 注册 `device_button_set_activity_callback_borrow(button_service_activity_isr, NULL)`；
3. 进入 RUNNING；
4. 安排一次初始 one-shot 扫描；
5. 调度失败时进入 STOPPING，注销活动回调、停止 Timer、等待在途回调并回滚到 INITIALIZED。

- [ ] **Step 5: 实现 Timer 回调的有限推进和双事件转发**

Timer 回调进入时在锁内：

- `timer_armed = false`；
- `timer_inflight++`；
- 若不是 RUNNING，直接走统一退出；
- 消费 `edge_pending`，复制并清除 `schedule_error`。

锁外调用：

```c
device_button_scan_result_t result = { 0 };
const esp_err_t error = device_button_scan(now_ms, &result);
```

成功时逐个调用现有 `button_service_event_cb_t`。扫描结束后，仅当下列任一条件为真才再次
`esp_timer_start_once()`：

```c
result.follow_up_required
|| edge_arrived_during_scan
|| scan_failed
```

其中 `scan_failed` 保持“首次失败记录、恢复后记录一次”的现有语义；错误时继续有限 one-shot
重试，成功且稳定时停表。回调统一退出时减少 `timer_inflight`，STOPPING 时 give
`s_quiesced_signal`。

边沿竞态必须按 `timer_armed` 和 `edge_pending` 收敛：Timer 回调采样期间 ISR 若已安排下一次
Timer，回调结束不得重复安排；若 ISR 只锁存了边沿，回调结束必须再安排一轮。

- [ ] **Step 6: 实现同步 `stop(timeout_ms)` 和安全 `deinit()`**

`stop()` 只接受 RUNNING 或 STOPPING，且 `timeout_ms > 0`：

1. 首次调用在锁内切到 STOPPING；
2. 调用 `device_button_set_activity_callback_borrow(NULL, NULL)`，阻止新业务入口；
3. 循环检查 `activity_inflight == 0 && schedule_inflight == 0`，未收敛时用剩余超时等待
   `s_quiesced_signal`；
4. 所有 ISR 调度调用退出后调用 `esp_timer_stop()`；`ESP_ERR_INVALID_STATE` 表示 Timer 已自然到期，
   可继续收敛；
5. 循环等待 `timer_inflight == 0`；该计数覆盖 Device 扫描和上层事件回调；
6. 成功后清空运行时锁存，切回 INITIALIZED。

超时返回 `ESP_ERR_TIMEOUT` 并保持 STOPPING；重复 `stop()` 从步骤 2 继续。`deinit()` 只接受
INITIALIZED，先删除 Timer，再删除 `s_quiesced_signal` 并清空借用回调和配置。

- [ ] **Step 7: 运行 Core 检查并确认 GREEN**

Run:

```powershell
& .\tools\tests\check_button_event_driven.ps1 -Section Core
git diff --check
```

Expected: 两条命令 exit code 0。默认 `All` 此时仍可因 Wake/Docs 未完成而失败。

- [ ] **Step 8: 核查并提交核心实现**

Run:

```powershell
git diff -- components/bsp/include/bsp.h components/bsp/src/bsp_button.c
git diff -- components/device/device_button/include/device_button.h components/device/device_button/src/device_button.c
git diff -- components/services/button_service
git status --short
git add -- components/bsp/include/bsp.h components/bsp/src/bsp_button.c
git add -- components/device/device_button/include/device_button.h components/device/device_button/src/device_button.c
git add -- components/services/button_service/include/button_service.h
git add -- components/services/button_service/src/button_service.c
git add -- components/services/button_service/CMakeLists.txt
git commit -m "refactor(button): 改为边沿触发按需扫描"
```

不要暂存 `tools/tests/check_button_event_driven.ps1`，也不要暂存 Global Constraints 中列出的用户
文件。

---

### Task 3: 接入 Light Sleep 按键唤醒事实

**Files:**

- Modify: `components/services/button_service/include/button_service.h`
- Modify: `components/services/button_service/src/button_service.c`
- Modify: `main/application/app_power_task.c:1-19, 299-367`

**Interfaces:**

- Input: `device_power_enter_light_sleep()` 返回的 `left_button`、`right_button` 按值快照。
- Output: 已接受的异步扫描请求；最终仍通过既有 `button_service_event_cb_t` 产生短按或长按。
- Failure: 唤醒事实提交失败记录为恢复错误并使 `app_power` 进入现有 BLOCKED 路径。

- [ ] **Step 1: 声明 Service 唤醒事实 API**

在 `button_service.h` 增加：

```c
#include <stdbool.h>

/** @brief 一次 Light Sleep 返回时锁存的按键唤醒事实 */
typedef struct
{
    bool left_button;  /**< EXT1 表明左键曾拉低 */
    bool right_button; /**< EXT1 表明右键曾拉低 */
} button_service_wakeup_info_t;

/**
 * @brief 按值提交 Light Sleep 按键唤醒事实并安排扫描
 *
 * ESP_OK 只表示请求已复制；最终产品事件仍通过事件回调返回。
 *
 * @param[in] wakeup 至少包含一个按键位的唤醒事实
 * @return ESP_OK 已接受；ESP_ERR_INVALID_ARG 无按键事实；ESP_ERR_INVALID_STATE Service
 *         未运行；或 Timer 调度错误码
 */
esp_err_t button_service_request_light_sleep_wakeup_copy(
    const button_service_wakeup_info_t *wakeup);
```

- [ ] **Step 2: 在 Service 中锁存并收敛唤醒按键**

给运行时状态增加：

```c
uint8_t wake_pending_mask;
```

私有位定义为：

```c
#define BUTTON_WAKE_LEFT  (1U << 0)
#define BUTTON_WAKE_RIGHT (1U << 1)
```

请求 API 校验非 NULL、至少一个位、RUNNING；在锁内 OR 到 `wake_pending_mask`，再调用普通上下文
one-shot 调度入口。调度失败时保留已复制的 pending 事实并返回原始错误码；`app_power` 会因此
进入 BLOCKED，后续边沿、重复请求或显式停止仍可安全收敛，不允许为了回滚而丢失真实唤醒。

Timer 扫描成功后：

1. Device 产生某键短按或长按时，清除对应 `wake_pending_mask`；
2. 调用 `device_button_get_pressed_state_copy()` 读取当前物理状态；
3. pending 键仍按下时保留 pending，并按 Device 的 `follow_up_required` 继续推进；
4. pending 键已释放且本轮没有该键 Device 事件时，合成该键 SHORT 事件并清除 pending；
5. ISR 与唤醒请求同时到达只合并 Timer 请求，同一键最多产生一个产品事件；
6. 只要 `wake_pending_mask != 0` 就继续 one-shot，读取失败时保留 pending 并重试。

Service 用最多两个事件的本地数组统一转发 Device 事件和合成短按；每个实体键一轮最多占一个槽，
不得覆盖另一键事件。

- [ ] **Step 3: 在 App Power 恢复 UI 后提交按键事实**

`app_power_task.c` 增加：

```c
#include "button_service.h"
```

在 `resume_ui_runtime()` 成功、`ui_stopped = false` 后，且进入现有
`if (wakeup_is_button(wakeup_source))` 分支时，先提交：

```c
const button_service_wakeup_info_t button_wakeup = {
    .left_button  = wakeup.left_button,
    .right_button = wakeup.right_button,
};
const esp_err_t button_error =
    button_service_request_light_sleep_wakeup_copy(&button_wakeup);
if (button_error != ESP_OK)
{
    keep_recovery_error(button_error);
    return button_error;
}
```

只有提交成功后才增加 `s_success_count` 并清空错误。这样快速释放的唤醒按键会在 UI 已恢复后重放
短按；持续按住的唤醒键继续通过原状态机判断短按/长按。

不要修改 `components/bsp/src/bsp_power.c`：它已经正确返回 EXT1 左右键按值快照，且该文件包含
用户未提交改动。

- [ ] **Step 4: 运行 Wake 和 Regression 检查**

Run:

```powershell
& .\tools\tests\check_button_event_driven.ps1 -Section Wake
& .\tools\tests\check_button_event_driven.ps1 -Section Regression
git diff --check
```

Expected: 三条命令 exit code 0；环境 2000/30000 ms 与 OTA 现有生命周期保持不变。

- [ ] **Step 5: 核查并提交唤醒桥接**

Run:

```powershell
git diff -- components/services/button_service/include/button_service.h
git diff -- components/services/button_service/src/button_service.c
git diff -- main/application/app_power_task.c
git status --short
git add -- components/services/button_service/include/button_service.h
git add -- components/services/button_service/src/button_service.c
git add -- main/application/app_power_task.c
git commit -m "feat(power): 衔接轻睡眠按键唤醒事实"
```

---

### Task 4: 同步 Service、Application 与低功耗文档

**Files:**

- Modify: `components/services/button_service/README.md`
- Modify: `main/application/README.md`
- Modify: `docs/低功耗流程.md`
- Add and stage: `tools/tests/check_button_event_driven.ps1`

**Interfaces:**

- Documents: 说明职责、上下文、生命周期、错误恢复、Light Sleep 阶段 1 边界和验收门槛。
- Regression script: 默认 `All` 一次覆盖 Core/Wake/Regression/Docs。

- [ ] **Step 1: 更新 Button Service README**

文档必须明确：

- 清醒空闲时没有 10 ms 周期 Timer；
- GPIO 任一边沿只触发一个扫描窗口；
- 去抖或长按未收敛时每 10 ms one-shot 推进，稳定后停表；
- Service 没有独立 Task，事件回调仍运行在 ESP Timer Task；
- `button_service_stop(timeout_ms)` 的 STOPPING、重复停止和成功后无在途回调保证；
- Light Sleep 唤醒事实 API 的按值复制、快速释放短按重放和双键去重规则。

- [ ] **Step 2: 更新 Application README**

在 Task/事件流和低功耗编排章节说明：

```text
GPIO 双边沿 → BSP ISR → Device 活动事实 → Button Service 临时 one-shot 扫描
→ Device 稳定事件 → app_key → 默认事件循环
```

以及：

```text
EXT1 左右键掩码 → app_power 恢复 UI
→ button_service_request_light_sleep_wakeup_copy()
→ 原按键状态机或快速释放短按重放
```

环境 Application Task 仍同时拥有 2000 ms 电池与 30000 ms 温湿度截止时间；OTA 本次不变。

- [ ] **Step 3: 更新低功耗流程**

`docs/低功耗流程.md` 必须明确：

- 阶段 1 不停止 Button Service；
- 睡眠期间不能依赖 GPIO ISR 边沿在唤醒后重放；
- `app_power` 在 UI 恢复后调用
  `button_service_request_light_sleep_wakeup_copy()`；
- 提交失败属于恢复错误，进入 BLOCKED，禁止继续自动睡眠；
- 睡前 stop/醒后 start 仍留到阶段 1 完成 100 次实机验收后的独立阶段。

- [ ] **Step 4: 运行完整静态验证**

Run:

```powershell
& .\tools\tests\check_button_event_driven.ps1
git diff --check
```

Expected: 两条命令 exit code 0。

- [ ] **Step 5: 核查测试与文档并提交**

Run:

```powershell
git diff -- tools/tests/check_button_event_driven.ps1
git diff -- components/services/button_service/README.md
git diff -- main/application/README.md
git diff -- docs/低功耗流程.md
git status --short
git add -- tools/tests/check_button_event_driven.ps1
git add -- components/services/button_service/README.md
git add -- main/application/README.md
git add -- docs/低功耗流程.md
git commit -m "docs(button): 更新按需扫描与唤醒契约"
```

---

### Task 5: 完成范围与行为核查

**Files:**

- Verify only; no new files expected.

**Interfaces:**

- Produces: 静态契约、格式、提交范围和人工验收清单的最终证据。

- [ ] **Step 1: 重跑全部自动检查**

Run:

```powershell
& .\tools\tests\check_button_event_driven.ps1
git diff --check
git status --short --branch
```

Expected:

- 静态检查通过；
- `git diff --check` 无输出；
- 工作区只剩 Global Constraints 中列出的用户原有修改和未跟踪文件；
- 本任务产生的实现、测试和文档均已提交。

- [ ] **Step 2: 核对提交**

Run:

```powershell
git log -3 --oneline
git show --stat --oneline HEAD~2..HEAD
```

Expected: 按职责看到核心扫描、Light Sleep 桥接、测试/文档提交，没有环境、OTA、HTTP、UART 或
用户文件混入。

- [ ] **Step 3: 交付未自动执行的验证**

明确告知用户本轮未编译、未刷写。等待用户明确要求后，编译只能运行：

```powershell
& .\dm.ps1 build
```

实机验收按设计文档执行：

- 左右短按各 50 次；
- 左右长按各 50 次，释放不追加短按；
- 左右键同时按下/释放时两侧事件都存在；
- 无按键 60 秒时按钮 Timer 触发计数不增长；
- Light Sleep 左右键各唤醒 50 次，无丢失、重复或新 BLOCKED。

## Plan Self-Review

- 设计覆盖：永久 10 ms 唤醒、双键事件、消抖/长按、Light Sleep 快速释放、同步停止、文档和
  实机门槛均映射到具体 Task。
- 分层覆盖：GPIO/ISR 只在 BSP；状态机只在 Device；Timer 与并发收敛只在 Service；睡眠事实
  解释只在 Application。
- 并发覆盖：ISR/Timer 并发、Timer 回调期间新边沿、停止期间迟到 ISR、在途上层回调和重复
  `stop()` 均有明确状态或计数。
- API 一致性：BSP/Device 活动回调签名一致；Device 固定容量 2；Service 停止参数和 Light Sleep
  输入都按批准设计声明。
- 范围覆盖：环境和 OTA 由 Regression 检查锁定；`bsp_power.c` 明确只读不改。
- 占位扫描：未发现待补内容标记、伪文件名或未定义占位接口。
- 验证边界：没有未经授权的构建命令；自动检查与未执行的编译/实机验收清晰区分。
