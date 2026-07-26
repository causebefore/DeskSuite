# Internal SRAM Low-Performance Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apply the approved ESP32-S3 low-performance configuration profile without changing the OTA Task implementation.

**Architecture:** Keep all memory policy in `sdkconfig.defaults` and mirror the effective values in the checked-in `sdkconfig`. Add one PowerShell contract check that parses both files semantically, so enabled, disabled, numeric, and mutually exclusive options cannot silently drift.

**Tech Stack:** ESP-IDF 6.0.1 Kconfig, ESP32-S3 PSRAM, PowerShell 7, Git.

> **Execution override (2026-07-26):** The user explicitly requested direct configuration changes
> without TDD. Task 1 and the test-script parts of Tasks 2–3 are therefore superseded. Implementation
> directly updates `sdkconfig.defaults` and `sdkconfig`, then validates the effective values with a
> read-only inline PowerShell assertion; no `tools/tests/check_internal_sram_config.ps1` is created.

## Global Constraints

- Work on branch `dev`.
- Do not modify `firmware_ota` source, public API, lifecycle, or Task.
- Keep `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`.
- Keep `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`.
- Keep the 16 KiB Instruction Cache and all existing Task stack sizes.
- Dynamic Wi-Fi TX is unavailable while PSRAM-first Wi-Fi/LwIP allocation is enabled; retain static TX and reduce its count to four.
- Do not invoke `idf.py`, CMake, or Ninja.
- The user did not authorize a build, so do not run `.\dm.ps1 build`.
- Use PowerShell only.
- Preserve all unrelated tracked and untracked worktree changes.

---

### Task 1: Add the configuration contract check

**Files:**
- Create: `tools/tests/check_internal_sram_config.ps1`
- Test: `tools/tests/check_internal_sram_config.ps1`

**Interfaces:**
- Consumes: ESP-IDF-style `CONFIG_NAME=value` and `# CONFIG_NAME is not set` lines.
- Produces: process exit code `0` only when both configuration files contain every approved value exactly once.

- [ ] **Step 1: Write the failing contract check**

```powershell
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Assert-ConfigValue {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Expected
    )

    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("缺少配置文件: $RelativePath")
        return
    }

    $escapedName = [regex]::Escape($Name)
    $matches = @(Get-Content -LiteralPath $path | Where-Object {
        $_ -match "^$escapedName=(.*)$" -or $_ -eq "# $Name is not set"
    })
    if ($matches.Count -ne 1) {
        $failures.Add("$RelativePath 中 $Name 应且只能出现一次，实际为 $($matches.Count) 次")
        return
    }

    $actual = if ($matches[0] -eq "# $Name is not set") {
        'n'
    } else {
        $matches[0].Substring($Name.Length + 1)
    }
    if ($actual -ne $Expected) {
        $failures.Add("$RelativePath 中 $Name 期望为 $Expected，实际为 $actual")
    }
}

$expected = [ordered]@{
    CONFIG_COMPILER_OPTIMIZATION_DEBUG = 'n'
    CONFIG_COMPILER_OPTIMIZATION_SIZE = 'y'
    CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY = 'y'
    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL = '512'
    CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP = 'y'
    CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL = '32768'
    CONFIG_ESP32S3_DATA_CACHE_16KB = 'y'
    CONFIG_ESP32S3_DATA_CACHE_32KB = 'n'
    CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM = '4'
    CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM = '16'
    CONFIG_ESP_WIFI_STATIC_TX_BUFFER = 'y'
    CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER = 'n'
    CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM = '4'
    CONFIG_ESP_WIFI_RX_BA_WIN = '4'
    CONFIG_ESP_WIFI_IRAM_OPT = 'n'
    CONFIG_ESP_WIFI_RX_IRAM_OPT = 'n'
    CONFIG_SPI_MASTER_ISR_IN_IRAM = 'n'
    CONFIG_SPI_SLAVE_ISR_IN_IRAM = 'n'
    CONFIG_GDMA_CTRL_FUNC_IN_IRAM = 'n'
    CONFIG_ESP_EVENT_POST_FROM_IRAM_ISR = 'n'
    CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH = 'y'
    CONFIG_LIBC_LOCKS_PLACE_IN_IRAM = 'n'
    CONFIG_MBEDTLS_DYNAMIC_BUFFER = 'y'
    CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA = 'y'
    CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT = 'y'
}

foreach ($relativePath in @('sdkconfig.defaults', 'sdkconfig')) {
    foreach ($entry in $expected.GetEnumerator()) {
        Assert-ConfigValue $relativePath $entry.Key $entry.Value
    }
}

if ($failures.Count -gt 0) {
    Write-Host '内部 SRAM 配置契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host '内部 SRAM 配置契约检查通过。' -ForegroundColor Green
```

- [ ] **Step 2: Run the check and verify the old configuration fails**

Run:

```powershell
& .\tools\tests\check_internal_sram_config.ps1
```

Expected: exit code `1`, reporting missing or mismatched external BSS, `-Os`, Data Cache, Wi-Fi, IRAM, Heap, and TLS values.

### Task 2: Apply the low-performance memory profile

**Files:**
- Modify: `sdkconfig.defaults`
- Modify: `sdkconfig`
- Test: `tools/tests/check_internal_sram_config.ps1`

**Interfaces:**
- Consumes: the exact expected-value map from Task 1.
- Produces: matching default and current ESP-IDF configurations.

- [ ] **Step 1: Update `sdkconfig.defaults`**

Add or replace the following semantic values:

```text
CONFIG_COMPILER_OPTIMIZATION_DEBUG=n
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
CONFIG_ESP32S3_DATA_CACHE_16KB=y
CONFIG_ESP32S3_DATA_CACHE_32KB=n
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=n
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=4
CONFIG_ESP_WIFI_RX_BA_WIN=4
CONFIG_ESP_WIFI_IRAM_OPT=n
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
CONFIG_SPI_MASTER_ISR_IN_IRAM=n
CONFIG_SPI_SLAVE_ISR_IN_IRAM=n
CONFIG_GDMA_CTRL_FUNC_IN_IRAM=n
CONFIG_ESP_EVENT_POST_FROM_IRAM_ISR=n
CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y
CONFIG_LIBC_LOCKS_PLACE_IN_IRAM=n
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
```

Keep these values unchanged:

```text
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
```

- [ ] **Step 2: Mirror the same effective values in `sdkconfig`**

Use generated-file syntax for disabled values:

```text
# CONFIG_NAME is not set
```

Also align the compiler compatibility aliases:

```text
# CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG is not set
# CONFIG_COMPILER_OPTIMIZATION_DEFAULT is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_RELEASE=y
```

- [ ] **Step 3: Run the contract check**

Run:

```powershell
& .\tools\tests\check_internal_sram_config.ps1
```

Expected: exit code `0` and `内部 SRAM 配置契约检查通过。`

### Task 3: Verify scope and commit

**Files:**
- Verify: `sdkconfig.defaults`
- Verify: `sdkconfig`
- Verify: `tools/tests/check_internal_sram_config.ps1`

**Interfaces:**
- Consumes: completed configuration-only diff.
- Produces: one independently revertible configuration commit.

- [ ] **Step 1: Check whitespace and changed-file scope**

Run:

```powershell
git diff --check
git status --short
git diff --name-only
```

Expected:

- `git diff --check` exits `0`.
- Task changes are limited to `sdkconfig.defaults`, `sdkconfig`, and `tools/tests/check_internal_sram_config.ps1`.
- Existing unrelated changes remain unstaged.
- No path under `components/communication/tools/firmware_ota/` appears.

- [ ] **Step 2: Re-run the contract check**

Run:

```powershell
& .\tools\tests\check_internal_sram_config.ps1
```

Expected: exit code `0`.

- [ ] **Step 3: Stage only task files and inspect the index**

Run:

```powershell
git add -- sdkconfig.defaults sdkconfig tools/tests/check_internal_sram_config.ps1
git diff --cached --name-only
git diff --cached --check
```

Expected: exactly the three task files are staged and the cached whitespace check exits `0`.

- [ ] **Step 4: Commit**

Run:

```powershell
git commit -m "perf(memory): 应用低性能内部SRAM配置"
```

Expected: commit succeeds on `dev`; unrelated worktree changes remain uncommitted.
