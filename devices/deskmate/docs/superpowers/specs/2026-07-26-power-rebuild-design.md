# DeskMate 低功耗流程重建设计

> 状态：已确认设计。本文定义从双按键最小 Light-sleep 闭环开始，逐步加入运行期组件
> 停止与恢复的目标方案。实现以当前 `dev` 已完成的 RTC INT 解耦为基线。

## 1. 背景与目标

现有低功耗流程在一个 Application 事务中同时处理网络、环境采样、UI、按键、硬件唤醒配置、
活动取消和失败回滚。各组件的停止终态、迟到操作和恢复顺序相互影响，串口又可能在 Light-sleep
返回后失去输出，导致故障只能表现为“没有后续日志”，无法确认最后成功步骤。

本次重建目标：

- 保留最后一次用户活动后 60 秒自动进入 Light-sleep 的产品策略；
- 正式唤醒源只包含左键 GPIO18 和右键 GPIO0，RTC INT 与内部 Timer 均不参与；
- 第一版不停止任何外设或 Task，先建立可重复的双按键睡眠/唤醒闭环；
- 使用 SD 卡上的追加式 JSONL 记录取代串口作为主要诊断依据；
- 第一版连续完成 100 次人工按键唤醒后，才允许加入第一个生命周期参与者；
- 后续每次只加入一个参与者，并以独立 commit 和同等验证门槛建立可回退基线；
- 每个参与者根据资源所有权选择 `stop`、`suspend` 或 `deinit`，不机械反初始化全部组件。

本次不追求确定的休眠电流目标。当前没有实机电流测量条件，无法用未经测量的功耗收益为复杂
停机流程辩护。

## 2. 非目标

- 不恢复 RTC INT 轻睡眠唤醒或 RTC Service 暂停屏障；
- 不添加 ESP32 内部 Timer 诊断唤醒；
- 不使用 Deep-sleep、ESP-IDF automatic Light-sleep 或 Tickless Idle；
- 不创建可在运行期注册的通用“低功耗参与者框架”；
- 不在第一版停止网络、环境、UI、按键、音频、存储或其他外设；
- 不以 UART 是否恢复作为睡眠闭环的验收条件；
- 不在缺少功耗数据时提前加入音频、SD 卡或其他外设的低功耗生命周期。

## 3. 架构边界

重建后的职责划分为：

```text
app_power
    负责 60 秒活动窗口、产品阻止条件、阶段顺序、取消、恢复和最终状态
        ↓
各参与组件自己的生命周期 API
    只停止或恢复自身拥有的 Task、Timer、会话和硬件资源
        ↓
device_power → BSP
    在一次同步调用内检查按键、配置 EXT1、Light-sleep、锁存来源并清理配置
```

### 3.1 Application

`app_power` 是唯一电源 Application Task，拥有：

- 最后活动单调时间和活动代次；
- 睡眠循环编号 `cycle_id`；
- 当前主状态与步骤；
- 已完成停机步骤集合；
- 最近唤醒来源、成功次数、主错误和恢复错误；
- 暂时阻止睡眠的产品状态快照。

`app_power` 不访问其他组件的 Task、Queue、Timer、GPIO 或私有句柄。它只调用稳定公共 API，
并根据公共契约确认组件是否到达明确终态。

睡眠准备不得调用 `app_settings_reset()` 或类似 API 隐式清除设置会话、OTA 目标及其他产品
状态。这些状态要么作为只读睡眠阻止条件，要么由其所有者提供正式暂停契约。

### 3.2 Device 与 BSP

Device 对上提供不暴露 GPIO 和板型的同步 Light-sleep 能力。BSP 拥有 GPIO18、GPIO0、
EXT1 配置以及 ESP-IDF 睡眠调用。

Device/BSP 不创建电源 Task，不决定 60 秒窗口、重试、产品降级或组件停机顺序。RTC GPIO、
RTC 告警标志和 `rtc_service` 均不进入这条调用链。

### 3.3 SD 诊断

SD 跟踪是 `main/application/` 内的私有诊断模块，不创建新 ESP-IDF 组件或 Task。所有文件
操作只由 `app_power` Task 串行执行。SD 卡和文件系统在本方案中始终保持初始化，不作为低功耗
参与者，否则会破坏主要观测链。

## 4. 主状态与单轮记录

主状态固定为：

```text
STOPPED → AWAKE → PREPARING → SLEEPING → RESUMING → AWAKE
                              ↘
                               BLOCKED
```

每轮尝试创建局部阶段记录，至少包含：

- `cycle_id`；
- 本轮开始时的活动代次；
- 当前步骤；
- 已确认完成的停机步骤集合；
- 唤醒结果；
- 主操作错误；
- 恢复错误。

参与者及顺序使用显式枚举和静态代码表达，不提供运行期注册表或通用回调接口。局部阶段记录
取代现有分散的全局停机布尔变量。

一个步骤只有在明确到达目标终态后，才能加入已完成集合。API 返回超时而组件无法确认处于
运行或停止状态时，不得猜测其资源所有权。

## 5. 活动窗口与睡眠阻止条件

`app_power_notify_activity()` 只执行以下操作：

1. 递增活动代次；
2. 使用 `esp_timer_get_time()` 更新最后活动时间；
3. 通知电源 Task 重新计算 60 秒截止时间。

60 秒窗口使用单调时间，不依赖 RTC 或系统墙上时钟。

语音、音频、OTA 和实时网络租约等明确的产品事务仍可暂时阻止睡眠，但检查必须是只读的。
`app_power` 保存并记录阻止原因掩码，不通过检查函数改变其他组件状态。窗口已经结束但阻止条件
仍存在时，Application 按配置的重试间隔重新检查；条件解除后可以立即继续准备，不重新虚构一次
用户活动。

准备阶段在每个有界步骤返回后重新检查活动代次。若出现新活动，则停止继续准备，并恢复本轮
已经完成的步骤。同步生命周期调用不被强制中断；活动发生在调用期间时，等待调用返回并确认
终态后再取消。

## 6. 双按键同步睡眠事务

删除现有 Device 的 `prepare/start/cancel` 三段式契约，改为单个同步调用：

```c
esp_err_t device_power_enter_light_sleep(
    device_power_wakeup_info_t *out_wakeup);
```

`device_power_wakeup_info_t` 只表达：

- 左键唤醒；
- 右键唤醒；
- 左右键同时命中。

同步调用的数据流为：

```text
检查左右键均为释放高电平
    → 配置 GPIO18、GPIO0 EXT1 ANY_LOW
    → 调用 esp_light_sleep_start()
    → 锁存 EXT1 唤醒状态
    → 清理本轮 EXT1 配置
    → 返回 Device 唤醒事实
```

函数必须遵守：

- `out_wakeup == NULL` 返回 `ESP_ERR_INVALID_ARG`；
- 任意按键未释放返回 `ESP_ERR_INVALID_STATE`，不进入睡眠；
- 配置、睡眠入口或清理失败保留原始 `esp_err_t`；
- 返回前必须尽力清理本轮已经建立的临时唤醒配置；
- 只有返回 `ESP_OK` 时 `out_wakeup` 有效；
- 不在公共 API 中暴露 GPIO、EXT1 掩码或 ESP-IDF 唤醒枚举；
- 不读取 RTC INT，不调用 RTC Device 或 RTC Service。

若睡眠主操作成功但临时唤醒配置清理失败，函数返回清理错误。Application 将其视为状态无法可靠
复用并进入 `BLOCKED`，不能继续下一轮睡眠。

## 7. SD JSONL 诊断

### 7.1 启用与失败策略

新增 `CONFIG_DESKMATE_POWER_VALIDATION_MODE`，Kconfig 默认关闭。当前低功耗重建分支在
`sdkconfig.defaults` 中显式开启，完成硬件验收并进入正式固件前再关闭。

- 验证模式开启时，SD 跟踪强制启用并作为进入睡眠的必要条件；
- 正式模式默认不写 SD 跟踪，避免长期写卡、改变时序和增加功耗；
- 验证模式下挂载、创建目录、打开、写入、`fflush`、`fsync` 或关闭失败，均禁止进入下一轮
  睡眠并进入 `BLOCKED`；
- 诊断失败只阻止自动睡眠，不要求重启设备，也不应停止其他正常产品能力。

### 7.2 文件与写入模型

诊断文件为：

```text
/sdcard/diagnostics/power_cycles.jsonl
```

实际挂载根使用项目已有文件系统常量组装，不在多个模块重复硬编码。文件采用追加式 JSON
Lines；每一行是完整、独立、有换行结尾的 JSON 对象，不维护需要整体重写的 JSON 数组。

诊断模块使用固定上限缓冲区生成单行 JSON，不为每条事件进行无界动态分配，不记录密码、
Token、Authorization 或其他凭据。

验证模式中每条记录完成：

```text
追加完整 JSON 行 → fflush → fsync
```

写入 `sleep_enter` 并完成持久化后关闭文件，再调用 Device 睡眠事务。Light-sleep 返回后首先以
追加模式重新打开文件并写入 `wake_return`，之后才恢复其他参与者。这样不跨睡眠长期保留 C
文件流状态。

### 7.3 事件与字段

至少记录以下事件：

- `boot`；
- `awake_window_started`；
- `sleep_deferred`；
- `cycle_started`；
- `step_started`；
- `step_completed`；
- `preparation_cancelled`；
- `sleep_enter`；
- `wake_return`；
- `restore_started`；
- `restore_completed`；
- `cycle_completed`；
- `cycle_failed`；
- `blocked`。

每条记录至少包含：

| 字段 | 语义 |
| --- | --- |
| `schema_version` | 当前固定为 `1` |
| `boot_id` | 本次启动生成的随机十六进制标识 |
| `sequence` | 本次启动内严格递增的诊断记录序号 |
| `monotonic_us` | `esp_timer_get_time()` 单调时间 |
| `cycle_id` | 睡眠尝试编号；启动事件可为 `0` |
| `event` | 事件名称 |
| `state` | 主状态名称 |
| `step` | 当前步骤名称 |
| `activity_generation` | 当前活动代次 |
| `blockers` | 产品阻止原因位组合及可读名称 |
| `wake_source` | `NONE`、`LEFT_BUTTON`、`RIGHT_BUTTON`、`BOTH_BUTTONS` 或 `UNKNOWN` |
| `success_count` | 已成功睡眠并唤醒的累计次数 |
| `primary_error_code/name` | 主操作 `esp_err_t` 数值与名称 |
| `recovery_error_code/name` | 恢复 `esp_err_t` 数值与名称 |

`boot` 事件额外记录 `esp_reset_reason()` 的数值和名称。如果文件中某轮只有持久化完成的
`sleep_enter`，下一条却是新 `boot_id`，即可判定设备在睡眠调用期间发生复位。

## 8. 失败、恢复与 BLOCKED

错误分为三类：

1. **暂时拒绝**：产品事务活跃、按键尚未释放或准备期间出现活动。恢复已完成步骤后回到
   `AWAKE`，等待窗口或重试，不进入 `BLOCKED`。
2. **主操作失败且终态明确**：例如参与者拒绝停止且保证仍在运行。恢复其他已完成步骤并记录
   本轮失败；恢复完整时可以按产品重试策略继续。
3. **终态不确定或恢复失败**：生命周期超时后无法确认资源所有权、Device 临时唤醒配置无法
   清理，或任一已停止参与者无法恢复。尽力恢复全部已完成步骤后进入 `BLOCKED`。

恢复遍历全部已完成步骤。某一步失败时保留第一个恢复错误，但继续尝试其余步骤，最大限度恢复
设备可用能力。`BLOCKED` 状态禁止新的自动睡眠，不自动复位设备。

`app_power_get_status_copy()` 至少暴露：

- 主状态；
- 当前步骤；
- `cycle_id`；
- 活动代次；
- 阻止原因；
- 最近唤醒来源；
- 成功次数；
- 主错误；
- 恢复错误。

## 9. 分阶段实施

### 阶段 1：双按键最小闭环

保留 60 秒活动窗口和只读产品阻止检查，不停止任何外设或 Task：

```text
60 秒无活动且无产品阻止条件
    → 持久化 sleep_enter
    → Device 双按键同步睡眠事务
    → 持久化 wake_return
    → cycle_completed
    → 重新开始 60 秒窗口
```

阶段 1 完成并通过 100 次验收前，不得加入任何运行期参与者。

### 阶段 2：按键扫描

加入 `button_service` 的协作停止与恢复：

- 停止扫描 Timer 后，物理 GPIO 仍由 BSP 作为 Light-sleep 唤醒源；
- 唤醒后恢复扫描；
- UI 此时仍保持运行，便于单独验证唤醒按键事件衔接；
- 已释放按键和持续按住按键不得产生重复产品动作。

### 阶段 3：UI Runtime

在按键扫描停止后停止 UI Runtime：

- UI 必须到达明确 `STOPPED` 终态；
- 唤醒后先恢复 UI，再恢复按键扫描；
- 验证显示重建、当前页面呈现和连续循环无黑屏。

### 阶段 4：环境采样

在按键扫描前停止环境产品采样 Task：

- 等待进行中的 I²C 采样事务达到明确终态；
- Device 与 `environment_service` 默认保持初始化；
- 唤醒后恢复采样调度并确认快照继续更新。

### 阶段 5：网络

最后加入网络暂停与恢复，因为它同时涉及产品命令、Timer、Wi-Fi、Portal、OTA 和迟到回执：

- 暂停 API 必须返回明确的已暂停终态；
- 超时后无法确认网络状态时禁止睡眠；
- 唤醒恢复只要求状态所有权一致，不要求调用返回时已经成功联网。

最终停机顺序：

```text
网络 → 环境采样 → 按键扫描 → UI → Light-sleep
```

最终恢复顺序：

```text
UI → 按键扫描 → 环境采样 → 网络
```

音频、SD 卡、存储和其他外设不进入上述阶段。后续只有在发现它们影响稳定性或取得功耗测量
证据后，才分别设计和验证。

## 10. 验证标准

### 10.1 每阶段硬件门槛

每个阶段都必须完成连续 100 次人工按键睡眠/唤醒：

- 左键单独唤醒 50 次；
- 右键单独唤醒 50 次；
- 整轮保持同一个 `boot_id`；
- `cycle_id` 连续，无缺号或重复；
- 每轮包含顺序正确的 `sleep_enter → wake_return → cycle_completed`；
- `wake_source` 与实际按键一致；
- 最终 `success_count` 为 100；
- `primary_error` 与 `recovery_error` 均为 `ESP_OK`；
- 不出现 `TIMER`、`RTC_INT` 或 `UNKNOWN` 唤醒来源；
- JSONL 每一行均可独立解析；
- UART 是否恢复不影响验收结论。

左右键同时按下产生 `BOTH_BUTTONS` 时记录事实，但该轮不计入左右键各 50 次的单键验收。

### 10.2 状态机测试

通过私有可替换步骤函数测试：

- 活动代次变化后取消准备；
- 只恢复已经确认完成的步骤；
- 恢复顺序与阶段记录一致；
- 暂时忙或按键未释放不会进入 `BLOCKED`；
- 不确定终态进入 `BLOCKED`；
- 某一步恢复失败后仍继续尝试其他恢复步骤；
- SD 必要记录失败时不会调用 Device 睡眠入口；
- Device 返回清理错误时禁止下一轮睡眠。

测试替换只作为编译期私有测试接缝，不形成产品运行期参与者注册框架。

### 10.3 JSONL 校验工具

增加只读 PowerShell 工具：

```text
tools/validate_power_trace.ps1
```

工具解析 JSONL 并检查：

- JSON 语法与 `schema_version`；
- `boot_id` 是否变化；
- `sequence` 与 `cycle_id` 连续性；
- 每轮事件顺序；
- 唤醒来源及左右键计数；
- 成功计数；
- 主错误、恢复错误和 `BLOCKED`；
- `sleep_enter` 后直接出现新 `boot` 的疑似睡眠期复位。

校验工具不得修改 SD 记录。

## 11. 文档、提交与隔离

实现位于 `refactor/power-rebuild` 分支及独立 worktree，以当前 `dev` 的已提交 HEAD 为基线。
原工作区已有未提交修改不会带入新分支。

按以下可独立回滚职责拆分 commit：

1. 设计文档；
2. 双按键原子 Device/BSP 睡眠 API 与 SD 诊断；
3. 双按键最小 Application 闭环；
4. 按键扫描参与者；
5. UI Runtime 参与者；
6. 环境采样参与者；
7. 网络参与者。

每个阶段同步更新受影响的公共 Doxygen、Application/UI/Service README 和
`docs/低功耗流程.md`。提交不得混入原工作区的 UART workaround 或其他未知修改。

仓库默认不主动编译。用户明确要求编译时，只能在仓库根目录执行：

```powershell
& .\dm.ps1 build
```

若统一脚本或固定 ESP-IDF 环境报错，应立即停止并报告，不得绕过脚本调用 `idf.py`、CMake 或
Ninja。
