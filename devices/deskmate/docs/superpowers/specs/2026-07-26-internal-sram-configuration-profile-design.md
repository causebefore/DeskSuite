# 内部 SRAM 低性能配置档设计

## 1. 目标

在保留现有功能、PSRAM 必需契约和 OTA 事务实现不变的前提下，采用“性能换内部 SRAM”的
配置档，减少静态 DRAM、IRAM、Cache 占用以及联网、TLS 运行时的内部堆峰值。

本规格是配置专项任务。`firmware_ota` 的按需 Task 改造继续保留在
`2026-07-26-internal-sram-external-bss-ota-on-demand-design.md`，本阶段不修改 OTA 源码、
公共 API、生命周期或 Task。

## 2. 官方依据与项目边界

依据 ESP-IDF v6.0.1 ESP32-S3 配置契约：

- `.data`、`.bss` 和 IRAM 都会减少可用内部堆。
- 缩小 Data Cache 会把相应片上 SRAM 归还给堆。
- Wi-Fi、SPI、Event 和 Heap 的 IRAM 选项以内部内存换取吞吐或 Cache 关闭期间的执行能力。
- Wi-Fi 静态 RX 缓冲每个约占 1.6 KiB；降低 RX/TX 缓冲数量会降低突发吞吐。
- Mbed TLS 动态缓冲在需要时分配 TX/RX 空间，数据处理完成后释放。
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 控制普通 `malloc()` 优先使用内部 RAM 的尺寸阈值。

项目边界：

- 显示 SPI 初始化没有设置 `ESP_INTR_FLAG_IRAM`，不依赖 Flash 写入期间继续发送。
- 项目源码没有从 IRAM ISR 调用 `esp_event_post()`，也没有使用 SPI Slave。
- 显示、语音的 DMA 缓冲仍显式申请内部 DMA 内存。
- UI Task 在字体分区 mmap/munmap 的 Cache 冻结路径运行，其栈必须保留在内部 SRAM。
- HTTPS/OTA Client 均以单次请求或事务为生命周期，不复用已释放握手配置做 TLS 重协商。

## 3. 配置变更

所有配置同时写入 `sdkconfig.defaults` 和当前 `sdkconfig`。

### 3.1 静态内存与普通分配

```text
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
```

外部 BSS 由 ESP-IDF 自动迁移 `lwip`、`net80211`、`pp` 等已声明段，不手工给项目对象增加
`EXT_RAM_BSS_ATTR`。普通 `malloc()` 中 513 字节及以上的分配优先使用 PSRAM；显式
`MALLOC_CAP_INTERNAL`、`MALLOC_CAP_DMA` 的申请不受此阈值影响。

### 3.2 编译与 Cache

```text
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
# CONFIG_COMPILER_OPTIMIZATION_DEBUG is not set
CONFIG_ESP32S3_DATA_CACHE_16KB=y
# CONFIG_ESP32S3_DATA_CACHE_32KB is not set
```

使用 GCC `-Os` 缩小代码，代价是调试地址与源码行的对应不如 `-Og` 直观。Instruction Cache
已经是最小 16 KiB，保持不变。Data Cache 从 32 KiB 降到 16 KiB，额外 16 KiB 片上 SRAM
由启动阶段归还堆；访问 PSRAM、Flash 的延迟和显示、网络吞吐可能下降。

### 3.3 Wi-Fi 缓冲

```text
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=4
CONFIG_ESP_WIFI_RX_BA_WIN=4
```

RX BA Window 与静态 RX 数量保持一致，满足驱动约束。该档适用于 DeskMate 的低并发、
低吞吐 HTTP/JSON 与语音交互，不以 iperf 吞吐为目标。

ESP-IDF v6.0.1 中 `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER` 与当前
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` 存在 Kconfig 硬互斥。为了优先把 Wi-Fi/LwIP
可外置对象放入 PSRAM，本设计保留 PSRAM 优先分配和静态 TX 类型，只把静态 TX 数量从 8
降到 4。不得为了启用动态 TX 而关闭 PSRAM 优先分配。

### 3.4 IRAM 性能选项

```text
# CONFIG_ESP_WIFI_IRAM_OPT is not set
# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set
# CONFIG_SPI_MASTER_ISR_IN_IRAM is not set
# CONFIG_SPI_SLAVE_ISR_IN_IRAM is not set
# CONFIG_GDMA_CTRL_FUNC_IN_IRAM is not set
# CONFIG_ESP_EVENT_POST_FROM_IRAM_ISR is not set
CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y
# CONFIG_LIBC_LOCKS_PLACE_IN_IRAM is not set
```

关闭后：

- Wi-Fi 常用及 RX 路径从 IRAM 返回 Flash，吞吐下降。
- SPI 中断在 Flash 写入期间不再运行，OTA 写入时显示刷新可短暂停顿。
- SPI ISR 不再选择 GDMA 控制函数进入 IRAM；GDMA ISR 本身仍保留默认 IRAM 配置。
- `esp_event_post()`、Heap 与 libc lock 路径不再保证 Cache 关闭期间可调用。

项目当前 ISR 契约不依赖上述能力。若以后新增 IRAM ISR，必须禁止它调用 Event、Heap 和 libc
lock API，或回退对应配置。

`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y` 与
`CONFIG_SPI_MASTER_ISR_IN_IRAM=y` 互斥，因此必须作为同一配置组变更。

### 3.5 TLS 动态缓冲

```text
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
```

TLS TX/RX 缓冲按需创建并在使用后释放；握手完成后释放证书和配置数据。当前 HTTP/OTA
每次创建新 Client，后续连接会重新注册证书或证书包。若未来复用同一个 SSL 对象重握手，
必须重新注册相应配置。

## 4. 明确保留

- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`
- 16 KiB Instruction Cache
- 现有 CPU 频率、双核模式与日志级别
- 所有 Task 栈大小和栈内外部 RAM 属性
- Flash ROM 实现选择
- OTA Task 和 OTA 生命周期

## 5. 验证

新增 PowerShell 结构回归检查，验证：

- `sdkconfig.defaults` 与 `sdkconfig` 的显式配置一致。
- 外部 BSS、`-Os`、16 KiB Data Cache、低 Wi-Fi 缓冲、TLS 动态缓冲均已启用。
- Wi-Fi/SPI/Event/libc IRAM 配置均关闭，Heap 放入 Flash。
- PSRAM 优先分配和 32 KiB 内部 DMA 保留池未被误改。
- OTA 源文件不在本任务改动列表中。

按照仓库规则，本任务未收到编译授权，不执行构建。后续显式运行 `.\dm.ps1 build` 后，应检查：

- Kconfig 不存在依赖冲突。
- map 中 `.ext_ram.bss` 增加、内部 `.dram0.bss` 和 `.iram0.text` 下降。
- 启动、联网、HTTP/HTTPS、语音、显示刷新、Light-sleep 和 OTA 均可运行。
- 比较联网待机时内部堆总空闲、历史最低空闲和最大连续块。

## 6. 回退顺序

若实机出现问题，按最小影响顺序独立回退：

1. 显示卡顿或语音不连续：Data Cache 恢复 32 KiB。
2. Wi-Fi 丢包、连接不稳定：RX/TX 缓冲与 BA Window 恢复原值。
3. OTA 写入期间显示异常：恢复 SPI Master ISR IRAM。
4. HTTPS 重连失败：关闭 Mbed TLS 握手后配置释放，保留动态 TX/RX。
5. 出现 Cache 关闭期断言：恢复对应 Event、Heap 或 libc IRAM 配置并定位调用者。
