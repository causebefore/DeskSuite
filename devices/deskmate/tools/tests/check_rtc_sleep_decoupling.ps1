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
        $failures.Add("仍存在禁止的 RTC 睡眠耦合: $RelativePath / $Pattern")
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
Assert-NotContains 'components\services\rtc_service\include\rtc_service.h' `
    'RTC_SERVICE_STATE_PAUSED|pause_interrupt_consumption|resume_interrupt_consumption'
Assert-NotContains 'components\services\rtc_service\src\rtc_service_task.c' `
    'RTC_SERVICE_STATE_PAUSED|s_pause_requested|s_consumption_mutex'

Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_PIN_BTN_LEFT'
Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_PIN_BTN_RIGHT'
Assert-Contains 'components\bsp\src\bsp_power.c' 'CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED'
Assert-Contains 'components\bsp\src\bsp_power.c' 'BOARD_RTC_PIN_INT'
Assert-Contains 'components\bsp\src\bsp_power.c' '\.rtc_alarm'
Assert-Contains 'components\bsp\src\bsp_power.c' 'rtc_gpio_pulldown_dis'
Assert-Contains 'components\bsp\src\bsp_power.c' 'rtc_gpio_pullup_en'
Assert-Contains 'components\bsp\src\bsp_power.c' `
    'esp_sleep_pd_config\(ESP_PD_DOMAIN_RTC_PERIPH,\s*ESP_PD_OPTION_ON\)'
Assert-Contains 'components\bsp\src\bsp_power.c' `
    'esp_sleep_pd_config\(ESP_PD_DOMAIN_RTC_PERIPH,\s*ESP_PD_OPTION_AUTO\)'
Assert-Contains 'components\bsp\src\bsp_power.c' 'RTC 域内部上拉已保持'
Assert-NotContains 'components\bsp\src\bsp_power.c' 'gpio_get_level'
Assert-Contains 'components\bsp\src\bsp_power.c' 'esp_sleep_enable_timer_wakeup'
Assert-Contains 'components\bsp\src\bsp_power.c' `
    '#ifndef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED[\s\S]*esp_sleep_enable_timer_wakeup'
Assert-Contains 'components\bsp\include\bsp.h' 'bool\s+rtc_alarm'
Assert-Contains 'components\device\device_power\include\device_power.h' 'bool\s+rtc_alarm'
Assert-Contains 'components\device\device_power\src\device_power.c' '\.rtc_alarm'
Assert-Contains 'main\application\app_power.h' 'APP_POWER_WAKEUP_RTC_ALARM'
Assert-Contains 'main\application\app_power_task.c' 'RTC INT 唤醒已刷新屏幕'
Assert-Contains 'main\application\app_power_task.c' 'rtc_service_request_check'
Assert-Contains 'main\application\app_power_task.c' 'rtc_service_get_status_copy'
Assert-Contains 'main\application\app_power_task.c' 'confirm_rtc_alarm_consumed\(rtc_alarm_count_before_sleep\)'
Assert-Contains 'main\application\app_power_task.c' 'RTC INT 唤醒的告警消费已确认'
Assert-Contains 'main\application\app_power_task.c' `
    'sleep_error\s*==\s*ESP_ERR_INVALID_STATE\s*\?\s*ESP_ERR_INVALID_RESPONSE'
Assert-Contains 'main\application\app_power_task.c' `
    'RTC INT 测试模式下设备拒绝进入轻睡眠，停止自动重试'
Assert-NotContains 'main\application\app_power_task.c' 'gpio_get_level|device_rtc_read_alarm_flag'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_RTC_INT_WAKE_TEST_ENABLED'
Assert-Contains 'main\Kconfig.projbuild' `
    'DESKMATE_RTC_INT_WAKE_TEST_ENABLED[\s\S]{0,160}default y'
Assert-Contains 'main\Kconfig.projbuild' `
    'DESKMATE_RTC_INT_WAKE_TEST_ENABLED[\s\S]{0,700}RTC_PERIPH'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED=y'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC=60'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC=60'
Assert-Contains 'main\app_main.c' 'rtc_service_start'
Assert-Contains 'main\application\app_key.c' 'app_power_notify_activity'
Assert-Contains 'components\services\rtc_service\src\rtc_service_task.c' `
    'RTC_SERVICE_STATE_RUNNING\s*&&\s*s_state\s*!=\s*RTC_SERVICE_STATE_STOPPING'
Assert-Contains 'components\services\rtc_service\include\rtc_service.h' `
    '调用方可再次调用本函数继续等待'

if ($failures.Count -gt 0) {
    Write-Host 'RTC INT 独占维护唤醒测试契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'RTC INT 独占维护唤醒测试契约检查通过。' -ForegroundColor Green
