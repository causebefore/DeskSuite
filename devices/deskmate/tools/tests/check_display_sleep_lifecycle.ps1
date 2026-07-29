$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sourcePath = Join-Path $repoRoot 'components\bsp\src\bsp_display.c'
$failures = [System.Collections.Generic.List[string]]::new()

if (-not (Test-Path -LiteralPath $sourcePath)) {
    Write-Host '缺少显示 BSP 源文件。' -ForegroundColor Red
    exit 1
}

$source = Get-Content -Raw -LiteralPath $sourcePath

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Text -notmatch $Pattern) {
        $failures.Add($Message)
    }
}

function Get-Section {
    param(
        [Parameter(Mandatory = $true)][string]$Start,
        [Parameter(Mandatory = $true)][string]$End
    )

    $startIndex = $source.IndexOf($Start, [System.StringComparison]::Ordinal)
    $endIndex = $source.IndexOf($End, $startIndex + $Start.Length, [System.StringComparison]::Ordinal)
    if ($startIndex -lt 0 -or $endIndex -lt 0) {
        $failures.Add("无法定位源码区段: $Start -> $End")
        return ''
    }
    return $source.Substring($startIndex, $endIndex - $startIndex)
}

function Assert-Ordered {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string[]]$Tokens,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $cursor = 0
    foreach ($token in $Tokens) {
        $index = $Text.IndexOf($token, $cursor, [System.StringComparison]::Ordinal)
        if ($index -lt 0) {
            $failures.Add("$Message，缺少或顺序错误: $token")
            return
        }
        $cursor = $index + $token.Length
    }
}

Assert-Contains $source '#define\s+ST7305_CMD_SLEEP_IN\s+0x10U' 'ST7305 Sleep-In 指令必须为 0x10。'
Assert-Contains $source '#define\s+ST7305_CMD_SLEEP_OUT\s+0x11U' 'ST7305 Sleep-Out 指令必须为 0x11。'
Assert-Contains $source '#define\s+ST7305_SLEEP_IN_SETTLE_US\s+\(5U \* 1000U\)' 'Sleep-In 后必须等待至少 5 ms。'
Assert-Contains $source '#define\s+ST7305_SLEEP_OUT_SETTLE_US\s+\(120U \* 1000U\)' 'Sleep-Out 后必须按参考流程等待 120 ms。'
Assert-Contains $source '#define\s+ST7305_SLEEP_OUT_TO_IN_US\s+\(100U \* 1000U\)' 'Sleep-Out 到下一次 Sleep-In 必须间隔至少 100 ms。'

$enterSleep = Get-Section 'static esp_err_t lcd_enter_sleep(void)' 'static esp_err_t lcd_exit_sleep(void)'
$exitSleep = Get-Section 'static esp_err_t lcd_exit_sleep(void)' '/**'
$stop = Get-Section 'esp_err_t bsp_display_stop(uint32_t timeout_ms)' 'esp_err_t bsp_display_start(void)'
$start = Get-Section 'esp_err_t bsp_display_start(void)' 'uint32_t bsp_display_get_flush_fps(void)'

Assert-Ordered $enterSleep @(
    'ST7305_SLEEP_OUT_TO_IN_US',
    'lcd_cmd(ST7305_CMD_SLEEP_IN)',
    'esp_rom_delay_us(ST7305_SLEEP_IN_SETTLE_US)',
    's_controller_sleeping = true'
) 'Sleep-In 时序不符合数据手册约束'

Assert-Ordered $exitSleep @(
    'lcd_cmd(ST7305_CMD_SLEEP_OUT)',
    's_last_sleep_out_us = esp_timer_get_time()',
    'esp_rom_delay_us(ST7305_SLEEP_OUT_SETTLE_US)',
    's_controller_sleeping = false'
) 'Sleep-Out 时序不符合数据手册约束'

Assert-Ordered $stop @(
    'bsp_display_wait_flush_done',
    'gpio_intr_disable',
    'lcd_enter_sleep',
    'lcd_set_io_hold(true)'
) '显示停止顺序必须先静止 DMA、关闭 TE、休眠控制器，再保持 GPIO'

Assert-Ordered $start @(
    'lcd_set_io_hold(false)',
    'lcd_exit_sleep',
    'gpio_intr_enable',
    'set_display_accepting_frames(true)'
) '显示恢复顺序必须先解除 GPIO 保持、唤醒控制器、恢复 TE，再接受新帧'

if ($failures.Count -gt 0) {
    Write-Host 'ST7305 休眠生命周期契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'ST7305 休眠生命周期契约检查通过。' -ForegroundColor Green
