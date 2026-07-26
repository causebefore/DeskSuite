# RTC INT 与轻睡眠解耦 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除每 30 秒 RTC 闹钟测试，使 GPIO15 不再阻止或唤醒轻睡眠，同时保留清醒期间的 RTC Service 告警消费。

**Architecture:** Application 不再为轻睡眠暂停 RTC Service；Device/BSP 电源能力只暴露和配置左右按键唤醒。RTC Service 简化为单 Task 串行消费器，继续在清醒期间处理 GPIO ISR 和 AF。

**Tech Stack:** ESP-IDF 6.0、C、FreeRTOS、PowerShell 静态契约检查。

## Global Constraints

- 轻睡眠无活动窗口保持 `DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC=60`。
- EXT1 唤醒源只允许 GPIO18 和 GPIO0，GPIO15 不参与电平检查、唤醒掩码或唤醒结果。
- 保留 PCF85063 I2C、GPIO ISR、AF 清除和清醒期 `rtc_service` 事件链路。
- 不混入 `AGENTS.md`、`transport_http.cpp`、`main.c` 和 `bsp_power.c` 中已有 UART FLUSH 改动。
- 用户未明确要求编译，因此不运行构建；只运行静态契约检查和 `git diff --check`。

---

### Task 1: 建立 RTC 睡眠解耦回归契约

**Files:**
- Create: `tools/tests/check_rtc_sleep_decoupling.ps1`

**Interfaces:**
- Consumes: 仓库根目录及现有 C/Kconfig 文件。
- Produces: 无参数 PowerShell 检查脚本；满足契约返回 0，发现耦合返回 1。

- [x] **Step 1: 写入当前必然失败的静态契约检查**

脚本必须检查以下条件：

```powershell
Assert-FileMissing 'main\application\app_rtc_alarm.c'
Assert-FileMissing 'main\application\app_rtc_alarm.h'
Assert-NotContains 'main\CMakeLists.txt' 'app_rtc_alarm'
Assert-NotContains 'main\Kconfig.projbuild' 'DESKMATE_RTC_ALARM_TEST_ENABLED'
Assert-NotContains 'main\app_main.c' 'app_rtc_alarm'
Assert-NotContains 'main\application\app_power_task.c' 'rtc_service_pause_interrupt_consumption|rtc_service_resume_interrupt_consumption'
Assert-NotContains 'components\bsp\src\bsp_power.c' 'BOARD_RTC_PIN_INT|rtc_interrupt'
Assert-NotContains 'components\bsp\include\bsp.h' 'bool\s+rtc_interrupt'
Assert-NotContains 'components\device\device_power\include\device_power.h' 'bool\s+rtc_interrupt'
Assert-NotContains 'components\device\device_power\src\device_power.c' '\.rtc_interrupt'
Assert-NotContains 'components\services\rtc_service\include\rtc_service.h' 'RTC_SERVICE_STATE_PAUSED|pause_interrupt_consumption|resume_interrupt_consumption'
Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_PIN_BTN_LEFT'
Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_PIN_BTN_RIGHT'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC=60'
Assert-Contains 'main\app_main.c' 'rtc_service_start'
```

- [x] **Step 2: 运行检查并确认 RED**

Run:

```powershell
& .\tools\tests\check_rtc_sleep_decoupling.ps1
```

Expected: exit code 1，报告 `app_rtc_alarm`、RTC pause/resume、GPIO15 唤醒掩码和 `rtc_interrupt` 字段仍存在。

### Task 2: 删除临时闹钟测试和 Application 睡眠耦合

**Files:**
- Delete: `main/application/app_rtc_alarm.c`
- Delete: `main/application/app_rtc_alarm.h`
- Modify: `main/CMakeLists.txt`
- Modify: `main/Kconfig.projbuild`
- Modify: `main/app_main.c`
- Modify: `main/application/app_power_task.c`
- Modify: `main/application/README.md`

**Interfaces:**
- Consumes: `rtc_service_start()`、`rtc_service_stop()` 和普通告警事件回调。
- Produces: 不再依赖 RTC pause/resume 的 `run_light_sleep_transaction()`。

- [x] **Step 1: 删除 30 秒测试**

删除两个 `app_rtc_alarm` 文件，并移除 CMake 源文件、Kconfig 测试项、`app_main` 包含与启动/重排调用。保留告警事件中的：

```c
const esp_err_t activity_error = app_power_notify_activity();
```

- [x] **Step 2: 删除睡眠事务中的 RTC 屏障**

从 `run_light_sleep_transaction()` 删除：

```c
bool rtc_consumption_paused = false;
rtc_service_pause_interrupt_consumption(...);
rtc_service_resume_interrupt_consumption();
```

按键扫描停止后直接调用 `device_power_prepare_light_sleep()`；恢复路径只取消唤醒配置并恢复运行期组件。

### Task 3: 简化 RTC Service 生命周期

**Files:**
- Modify: `components/services/rtc_service/include/rtc_service.h`
- Modify: `components/services/rtc_service/src/rtc_service_task.c`
- Modify: `components/services/rtc_service/README.md`

**Interfaces:**
- Consumes: Device RTC ISR 回调、AF 读取和清除 API。
- Produces: `rtc_service_init/start/request_check/stop/get_status_copy/deinit`，不再提供 pause/resume。

- [x] **Step 1: 删除暂停公共契约**

从头文件删除：

```c
RTC_SERVICE_STATE_PAUSED
rtc_service_pause_interrupt_consumption(uint32_t timeout_ms)
rtc_service_resume_interrupt_consumption(void)
```

- [x] **Step 2: 删除仅供暂停使用的内部状态**

删除 `s_pause_requested`、`s_consumption_mutex`、`interrupt_consumption_allowed()` 和 pause/resume 实现。`consume_alarm_interrupt()` 由唯一 Service Task 直接串行执行；ISR 回调只在 `RTC_SERVICE_STATE_RUNNING` 时通知 Task；`stop()` 只接受 RUNNING。

- [x] **Step 3: 同步 Service README**

删除暂停屏障、轻睡眠保留低电平和 PAUSED 状态说明；保留启动主动检查、AF 消费、重试和回调契约。

### Task 4: 从 Device/BSP 电源能力移除 GPIO15

**Files:**
- Modify: `components/bsp/include/bsp.h`
- Modify: `components/bsp/src/bsp_power.c`
- Modify: `components/device/device_power/include/device_power.h`
- Modify: `components/device/device_power/src/device_power.c`

**Interfaces:**
- Consumes: 左右按键板级 GPIO。
- Produces: 只含 `left_button`、`right_button` 的 BSP/Device 唤醒快照。

- [x] **Step 1: 收窄唤醒结构**

从 `bsp_power_wakeup_info_t` 和 `device_power_wakeup_info_t` 删除：

```c
bool rtc_interrupt;
```

同步删除 Device 适配赋值。

- [x] **Step 2: 收窄 BSP EXT1 配置**

将掩码改为：

```c
#define BSP_POWER_WAKEUP_MASK \
    ((1ULL << BOARD_PIN_BTN_LEFT) | (1ULL << BOARD_PIN_BTN_RIGHT))
```

删除 RTC 引脚静态断言、进入前电平检查、RTC 唤醒结果和 RTC 日志参数。保留现有：

```c
(void) esp_sleep_set_console_uart_handling_mode(ESP_SLEEP_ALWAYS_FLUSH_UART);
```

### Task 5: 同步架构文档并完成 GREEN 验证

**Files:**
- Modify: `docs/低功耗流程.md`
- Modify: `components/services/README.md`（仅在现有描述涉及睡眠耦合时调整）

**Interfaces:**
- Consumes: Task 2—4 的最终行为。
- Produces: 与代码一致的按键-only 轻睡眠说明。

- [x] **Step 1: 更新文档**

文档必须明确：

- 轻睡眠只由 GPIO18、GPIO0 唤醒；
- RTC INT 不检查、不阻止、不唤醒；
- RTC Service 只保证清醒期中断消费；
- 60 秒无活动窗口不变；
- 删除每 30 秒测试和相关 Kconfig 表项。

- [x] **Step 2: 运行契约检查并确认 GREEN**

Run:

```powershell
& .\tools\tests\check_rtc_sleep_decoupling.ps1
```

Expected: exit code 0，输出“RTC INT 与轻睡眠解耦契约检查通过”。

- [x] **Step 3: 运行静态格式与范围检查**

Run:

```powershell
git diff --check
git status --short
```

Expected: `git diff --check` exit code 0；状态中只新增本任务文件，既有四个无关修改保持未暂存。

- [x] **Step 4: 精确暂存并提交**

`bsp_power.c` 必须只暂存 RTC 解耦相关行，不能暂存用户已有 UART FLUSH hunk。其余任务文件正常暂存。

Commit:

```powershell
git commit -m "refactor(power): 移除RTC中断与轻睡眠耦合"
```
