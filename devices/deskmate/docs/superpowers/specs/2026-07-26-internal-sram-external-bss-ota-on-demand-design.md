# 内部 SRAM：外部 BSS 与 OTA 按需 Task 设计

## 1. 目标

在不改变 OTA 公共 API、不降低 Wi-Fi/显示性能配置、不盲目缩减 Task 栈的前提下，减少
DeskMate 正常空闲和联网待机阶段的内部 SRAM 占用：

1. 启用 ESP-IDF 的外部 BSS 支持，把框架明确支持外置的静态 BSS 放入 PSRAM。
2. 把 `firmware_ota` 的 10 KiB Task 栈从“组件启动后常驻”改为“检查或安装事务期间按需存在”。

## 2. 依据与当前基线

本设计依据：

- [ESP-IDF v6.0.1 ESP32-S3 内存优化指南](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0.1/esp32s3/api-guides/performance/ram-usage.html)
- `docs/architecture/data_flow.md`
- `docs/architecture/api_conventions.md`
- `components/communication/tools/firmware_ota/README.md`

官方指南强调：

- 静态 `.data`、`.bss` 会直接减少运行时可用堆。
- Task 栈通常从堆分配，减少不必要的 Task 数量和生命周期可以显著节省 RAM。
- Task 栈只能在覆盖高负载路径并取得高水位数据后谨慎缩减。
- ESP32-S3 的静态 IRAM 会挤占可用于堆的 DRAM；关闭 Wi-Fi、SPI 等 IRAM 优化会影响性能，
  需要单独评估。

当前 `build/esp32.map` 的只读基线表明：

- `lwip`、`libnet80211`、`libpp` 中可由 ESP-IDF 外置的 BSS 约为 12.5 KiB。
- `firmware_ota` 在 `app_network_init()` 阶段启动常驻 Task，配置栈为 10240 字节；Task
  绝大多数时间阻塞等待命令。

## 3. 范围

### 3.1 本次包含

- 在 `sdkconfig.defaults` 和当前 `sdkconfig` 中启用
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`。
- 保留 `firmware_ota_init()`、`firmware_ota_start()`、`firmware_ota_stop()`、
  `firmware_ota_deinit()` 以及两个异步 `request` API。
- `firmware_ota_start()` 只进入可接受事务的 `IDLE` 状态，不创建 Task。
- `firmware_ota_request_check()` 和 `firmware_ota_request_install()` 成功提交时，各创建一个
  一次性 OTA Task。
- 一次性 Task 完成状态收敛、执行回调并释放自身栈。
- 检查成功得到的 `pending_target` 在检查 Task 退出后继续由 OTA Runtime 持有。
- 更新 `firmware_ota/README.md` 的生命周期和 Task 契约。
- 增加 PowerShell 结构回归检查，覆盖配置和一次性 Task 的关键不变量。

### 3.2 本次不包含

- 不给项目业务快照增加 `EXT_RAM_BSS_ATTR`。
- 不修改 `CONFIG_COMPILER_OPTIMIZATION_DEBUG`。
- 不缩小 Instruction/Data Cache。
- 不关闭 `CONFIG_ESP_WIFI_IRAM_OPT`、`CONFIG_ESP_WIFI_RX_IRAM_OPT`、
  `CONFIG_SPI_MASTER_ISR_IN_IRAM`。
- 不降低 Wi-Fi/LwIP 缓冲区数量。
- 不调整已有 Task 栈大小，不把 OTA Task 栈迁往 PSRAM。
- 不改变 OTA 检查、安装、摘要校验、启动分区切换和强制重启语义。

这些项目要么需要高水位数据，要么会影响网络、显示、Flash 写入或实时音频性能，应独立测量
和提交。

## 4. 外部 BSS 设计

同时修改：

```text
sdkconfig.defaults
sdkconfig
```

设置：

```text
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

ESP-IDF v6.0.1 的 Kconfig 契约会自动把 `lwip`、`net80211`、`pp` 等已声明的 BSS 链接到
`.ext_ram.bss`。PSRAM 初始化失败时固件不能继续启动，因为外部 BSS 已成为程序正确运行的必要
内存；当前工程已经启用并强制初始化 PSRAM，不允许忽略 PSRAM 缺失，因此契约一致。

本次不手工标注项目状态对象，避免把可能参与 Cache 禁用路径的数据误放入 PSRAM。后续如需继续
迁移，必须逐个证明对象不会被 ISR、Flash 写入或 Cache 禁用路径访问。

## 5. OTA 一次性 Task 生命周期

### 5.1 生命周期

```text
UNINITIALIZED
    │ init
    ▼
STOPPED（无 Task）
    │ start
    ▼
IDLE（无 Task）
    │ request_check
    ▼
CHECKING（一次性 Task）
    ├── 失败/无更新 ──→ IDLE（Task 退出）
    └── 有更新 ──────→ UPDATE_AVAILABLE（Task 退出，保留 pending_target）
                              │ request_install
                              ▼
                         DOWNLOADING（一次性 Task）
                              ├── 失败 ──→ IDLE（清除目标，Task 退出）
                              └── 成功 ──→ AWAITING_RESTART → esp_restart()
```

`start()` 的含义保持为启动组件生命周期，不再等同于创建后台 Task。异步 `request` API 只有在
一次性 Task 创建成功后才返回 `ESP_OK`，因此返回值仍表示命令已经成功提交。

### 5.2 运行资源

初始化阶段只长期持有：

- 状态互斥量；
- Task 停止信号量；
- 有界 Runtime 状态和 `pending_target`。

不再创建命令队列。一次性 Task 的命令通过创建参数传入，且同一时刻状态机只允许一个检查或
安装事务，不需要额外排队。

### 5.3 创建失败回滚

- 检查 Task 创建失败：`CHECKING → IDLE`，返回 `ESP_ERR_NO_MEM`。
- 安装 Task 创建失败：`DOWNLOADING → UPDATE_AVAILABLE`，保留 `pending_target`，返回
  `ESP_ERR_NO_MEM`。

Task 创建和状态预占必须在状态互斥量保护下完成，避免 `stop()` 在“状态已改变但 Task 尚未
登记”的窗口观察到不一致状态。新 Task 即使抢占运行，也只能在取得同一互斥量后完成状态收敛。

### 5.4 Task 完成

一次性 Task：

1. 执行一个检查或安装事务。
2. 在内部锁内更新 OTA 状态并复制回调。
3. 在锁外执行完成回调。
4. 若没有停止请求，清空 Task 句柄并释放自身。
5. 若已有停止请求，清除待安装目标并进入 `STOPPED`，再通知同步等待者并释放自身。

检查 Task 退出不会清除有效的 `pending_target`。安装事务一旦成功仍按现有原子契约立即重启。

## 6. 停止与并发语义

`firmware_ota_stop()` 保持同步：

- `IDLE` 或 `UPDATE_AVAILABLE` 且没有 Task：直接清除待安装目标并进入 `STOPPED`。
- `CHECKING` 或 `DOWNLOADING` 且 Task 正在运行：设置 `stopping=true`，等待当前事务和完成回调
  自然结束；不取消 HTTP、Flash 写入或镜像校验。
- 等待超时：保留运行资源和停止状态，返回 `ESP_ERR_TIMEOUT`，不得调用 `deinit()` 释放资源。
- `AWAITING_RESTART`：继续拒绝停止。

回调仍在 OTA Task 上下文、内部锁之外执行，且不得重入 OTA 控制 API。Task 退出前发送停止
信号；`deinit()` 只在 `STOPPED`、无 Task、无回调运行时释放互斥量和信号量。

## 7. 验证设计

### 7.1 自动结构回归

新增 `tools/tests/check_internal_sram_optimization.ps1`，验证：

- `sdkconfig.defaults` 与 `sdkconfig` 都启用外部 BSS。
- `firmware_ota_start()` 不创建 Task。
- `firmware_ota_request_check()` 与 `firmware_ota_request_install()` 通过同一个事务启动辅助函数
  创建一次性 Task。
- `firmware_ota_task()` 不再使用永久命令接收循环。
- Runtime 和初始化路径不再持有 OTA 命令队列。
- README 明确记录“每个事务按需创建并退出”的契约。

测试遵循仓库现有 `tools/tests/*.ps1` 结构检查方式：先在旧实现上运行并确认因常驻 Task/命令队列
而失败，再实施最小改动使其通过。

### 7.2 后续构建与实机验收

本任务未收到编译授权，不执行构建。后续显式执行 `.\dm.ps1 build` 后应检查：

- map 中出现 `.ext_ram.bss`，内部 `.dram0.bss` 相对基线下降。
- 启动后 OTA 处于 `IDLE` 时不存在 `firmware_ota` Task。
- 检查期间创建 `firmware_ota` Task，完成回调后 Task 消失。
- 有更新时 Task 消失但 `UPDATE_AVAILABLE` 和目标仍保留。
- 安装时重新创建 Task；失败后释放，成功后完成校验并重启。
- 检查/安装过程中请求休眠时，`stop()` 等待事务自然结束并最终进入 `STOPPED`。
- 内部堆的总空闲、历史最低空闲和最大连续块均记录用于与旧固件比较。

## 8. 验收标准

- 公共头文件和调用方无需修改。
- 空闲 OTA 生命周期不再持有 10240 字节 Task 栈。
- 外部 BSS 配置在默认配置和当前配置中一致生效。
- 检查结果可跨 Task 生命周期保留到安装或丢弃。
- Task 创建失败、事务失败和同步停止均保持明确、可恢复状态。
- 自动结构回归脚本通过。
- README 与实现描述一致。
