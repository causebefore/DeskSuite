$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    return Join-Path $repoRoot $RelativePath
}

function Assert-FileMissing {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    if (Test-Path -LiteralPath (Resolve-RepoPath $RelativePath)) {
        $failures.Add("仍存在应删除文件: $RelativePath")
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $path = Resolve-RepoPath $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("缺少待检查文件: $RelativePath")
        return
    }
    if ((Get-Content -Raw -LiteralPath $path) -match $Pattern) {
        $failures.Add("仍存在 RTC 睡眠耦合: $RelativePath / $Pattern")
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $path = Resolve-RepoPath $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("缺少待检查文件: $RelativePath")
        return
    }
    if ((Get-Content -Raw -LiteralPath $path) -notmatch $Pattern) {
        $failures.Add("缺少必须保留的能力: $RelativePath / $Pattern")
    }
}

Assert-FileMissing 'main\application\app_rtc_alarm.c'
Assert-FileMissing 'main\application\app_rtc_alarm.h'
Assert-NotContains 'main\CMakeLists.txt' 'app_rtc_alarm'
Assert-NotContains 'main\Kconfig.projbuild' 'DESKMATE_RTC_ALARM_TEST_ENABLED'
Assert-NotContains 'main\app_main.c' 'app_rtc_alarm'
Assert-NotContains 'main\application\app_power_task.c' `
    'rtc_service_pause_interrupt_consumption|rtc_service_resume_interrupt_consumption'
Assert-NotContains 'main\app_main.c' 'app_power_notify_activity'
Assert-NotContains 'components\bsp\src\bsp_power.c' 'BOARD_RTC_PIN_INT|rtc_interrupt'
Assert-NotContains 'components\bsp\include\bsp.h' 'bool\s+rtc_interrupt'
Assert-NotContains 'components\device\device_power\include\device_power.h' 'bool\s+rtc_interrupt'
Assert-NotContains 'components\device\device_power\src\device_power.c' '\.rtc_interrupt'
Assert-NotContains 'components\services\rtc_service\include\rtc_service.h' `
    'RTC_SERVICE_STATE_PAUSED|pause_interrupt_consumption|resume_interrupt_consumption'
Assert-NotContains 'components\services\rtc_service\src\rtc_service_task.c' `
    'RTC_SERVICE_STATE_PAUSED|s_pause_requested|s_consumption_mutex'

Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_PIN_BTN_LEFT'
Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_PIN_BTN_RIGHT'
Assert-Contains 'components\bsp\src\bsp_power.c' 'esp_sleep_enable_timer_wakeup'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC=60'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC=60'
Assert-Contains 'main\app_main.c' 'rtc_service_start'
Assert-Contains 'main\application\app_key.c' 'app_power_notify_activity'

if ($failures.Count -gt 0) {
    Write-Host 'RTC INT 与轻睡眠解耦契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'RTC INT 与轻睡眠解耦契约检查通过。' -ForegroundColor Green
