$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    return Join-Path $repoRoot $RelativePath
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
        $failures.Add("缺少当前低功耗阶段必需能力: $RelativePath / $Pattern")
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
        $failures.Add("仍存在当前低功耗阶段禁止内容: $RelativePath / $Pattern")
    }
}

function Assert-FileMissing {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    if (Test-Path -LiteralPath (Resolve-RepoPath $RelativePath)) {
        $failures.Add("仍存在应删除文件: $RelativePath")
    }
}

$bspHeader = 'components\bsp\include\bsp.h'
$bspSource = 'components\bsp\src\bsp_power.c'
$deviceHeader = 'components\device\device_power\include\device_power.h'
$deviceSource = 'components\device\device_power\src\device_power.c'
$powerHeader = 'main\application\app_power.h'
$powerTask = 'main\application\app_power_task.c'
$networkTask = 'main\application\app_network_task.c'
$settingsHeader = 'components\sys\include\settings_store.h'
$uiHeader = 'main\ui\include\ui_runtime.h'
$uiTask = 'main\ui\core\ui_task.c'
$lvglHeader = 'components\graphics\ui_platform\include\ui_platform_lvgl.h'
$lvglSource = 'components\graphics\ui_platform\lvgl\lvgl_runtime.c'
$displayHeader = 'components\device\device_display\include\device_display.h'
$displayBspSource = 'components\bsp\src\bsp_display.c'
$appVoiceHeader = 'main\application\app_voice.h'

Assert-Contains $bspHeader 'bsp_power_enter_light_sleep'
Assert-Contains $bspHeader 'bsp_display_stop'
Assert-Contains $bspHeader 'bsp_display_start'
Assert-Contains $displayBspSource 'ST7305_CMD_HIGH_POWER_MODE\s+0x38U'
Assert-Contains $displayBspSource 'ST7305_CMD_LOW_POWER_MODE\s+0x39U'
Assert-Contains $displayBspSource 'ST7305_HPM_TO_LPM_VOLTAGE_SETTLE_MS\s+20U'
Assert-Contains $displayBspSource 'ST7305_HPM_TO_LPM_MODE_SETTLE_MS\s+100U'
Assert-Contains $displayBspSource 'ST7305_LPM_TO_HPM_MODE_SETTLE_MS\s+300U'
Assert-Contains $displayBspSource 'ST7305_LPM_TO_HPM_VOLTAGE_SETTLE_MS\s+20U'
Assert-Contains $displayBspSource `
    '(?s)lcd_switch_to_low_power_mode\s*\(void\).*?ST7305_CMD_HIGH_POWER_MODE.*?lcd_write_source_voltage_levels.*?lcd_select_source_voltage_set.*?ST7305_HPM_TO_LPM_VOLTAGE_SETTLE_MS.*?ST7305_CMD_LOW_POWER_MODE.*?ST7305_HPM_TO_LPM_MODE_SETTLE_MS'
Assert-Contains $displayBspSource `
    '(?s)lcd_switch_to_high_power_mode\s*\(void\).*?ST7305_CMD_LOW_POWER_MODE.*?ST7305_CMD_HIGH_POWER_MODE.*?ST7305_LPM_TO_HPM_MODE_SETTLE_MS.*?lcd_write_source_voltage_levels.*?lcd_select_source_voltage_set.*?ST7305_LPM_TO_HPM_VOLTAGE_SETTLE_MS'
Assert-Contains $displayBspSource `
    '(?s)bsp_display_stop\s*\(uint32_t\s+timeout_ms\).*?bsp_display_wait_flush_done.*?gpio_intr_disable.*?lcd_switch_to_low_power_mode.*?lcd_set_io_hold\(true\)'
Assert-Contains $displayBspSource `
    '(?s)bsp_display_start\s*\(void\).*?lcd_set_io_hold\(false\).*?lcd_switch_to_high_power_mode.*?gpio_intr_enable.*?set_display_accepting_frames\(true\)'
Assert-NotContains $displayBspSource 'ST7305_CMD_SLEEP_IN|lcd_cmd\(0x10U?\)'
Assert-Contains $bspSource 'esp_sleep_enable_ext1_wakeup_io'
Assert-Contains $bspSource 'esp_sleep_enable_timer_wakeup'
Assert-Contains $bspSource 'esp_sleep_disable_ext1_wakeup_io'
Assert-Contains $bspSource 'esp_sleep_disable_wakeup_source\(ESP_SLEEP_WAKEUP_TIMER\)'
Assert-Contains $bspHeader 'bool\s+timer'
Assert-NotContains $bspHeader 'bool\s+rtc_timer'
Assert-Contains $deviceHeader 'device_power_enter_light_sleep'
Assert-Contains $deviceHeader 'bool\s+timer'
Assert-NotContains $deviceHeader 'bool\s+rtc_timer'
Assert-Contains $deviceSource 'bsp_power_enter_light_sleep'
Assert-Contains $lvglHeader 'ui_platform_lvgl_stop'
Assert-Contains $lvglHeader 'ui_platform_lvgl_start'
Assert-Contains $lvglSource 'lv_refr_now'
Assert-Contains $lvglSource 'device_display_wait_flush_done'
Assert-Contains $uiHeader 'ui_runtime_stop'
Assert-Contains $uiHeader 'ui_runtime_start'
Assert-Contains $uiTask 'UI_TASK_CONTROL_DEINIT'
Assert-Contains $powerTask 'device_power_enter_light_sleep'
Assert-Contains $powerHeader 'APP_POWER_STEP_VOICE_STOP'
Assert-Contains $powerHeader 'APP_POWER_STEP_VOICE_START'
Assert-Contains $powerHeader 'APP_POWER_BLOCKER_AUDIO_PROCESSOR'
Assert-Contains $powerTask 'app_voice_stop'
Assert-Contains $powerTask 'app_voice_start'
Assert-Contains $powerTask 'app_voice_get_status_copy'
Assert-Contains $appVoiceHeader 'app_voice_reconcile_network_lease\s*\(\s*uint32_t\s+timeout_ms\s*\)'
Assert-Contains $powerTask 'app_voice_reconcile_network_lease\s*\(\s*APP_POWER_VOICE_LIFECYCLE_TIMEOUT_MS\s*\)'
Assert-Contains $powerTask 'APP_POWER_STEP_UI_STOP'
Assert-Contains $powerTask 'APP_POWER_STEP_UI_START'
Assert-Contains $powerTask 'APP_POWER_WAKEUP_TIMER'
Assert-NotContains $powerTask 'APP_POWER_WAKEUP_RTC_TIMER'
Assert-Contains $powerTask 'timer_refresh_count'
Assert-NotContains $powerTask 'rtc_timer_refresh_count'
Assert-Contains $powerTask 'ui_runtime_stop'
Assert-Contains $powerTask 'ui_runtime_start'
Assert-Contains $powerTask 'app_network_suspend_for_power_save'
Assert-Contains $powerTask 'app_network_resume_from_power_save'
Assert-Contains $powerTask 'app_network_sync_for_power_save'
Assert-Contains $powerTask 'app_network_get_next_dashboard_sync_at_utc'
Assert-Contains $powerHeader 'APP_POWER_STATE_OFFLINE_DISPLAY'
Assert-Contains $powerTask 'app_pomodoro_requires_live_display'
Assert-Contains $powerTask 'run_offline_display_session'
Assert-Contains $powerTask 'Timer 计划唤醒='
Assert-Contains $networkTask 's_next_refresh_at_utc\s*=\s*dashboard\.next_refresh_at_utc'
Assert-Contains $networkTask 'esp_timer_start_once\(s_dashboard_timer'
Assert-Contains $networkTask 'CONFIG_DESKMATE_DASHBOARD_FAILURE_RETRY_SEC'
Assert-NotContains $networkTask 'esp_timer_start_periodic\(s_dashboard_timer'
Assert-NotContains $settingsHeader 'refresh_seconds'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC=60'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC=60'
Assert-NotContains 'sdkconfig.defaults' 'CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_PM_ENABLE=y'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_PM_DFS_INIT_AUTO=y'
Assert-Contains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC'
Assert-NotContains 'main\Kconfig.projbuild' 'DESKMATE_RTC_INT_WAKE_TEST_ENABLED'

Assert-FileMissing 'main\application\app_power_trace.c'
Assert-FileMissing 'main\application\app_power_trace.h'
Assert-FileMissing 'tools\validate_power_trace.ps1'
Assert-FileMissing 'tools\tests\fixtures\power_trace_valid.jsonl'
Assert-FileMissing 'tools\tests\check_rtc_sleep_decoupling.ps1'

foreach ($relativePath in @($bspHeader, $bspSource, $deviceHeader, $deviceSource, $powerTask)) {
    Assert-NotContains $relativePath `
        'power_(prepare|cancel|start)_light_sleep'
}

Assert-NotContains $bspHeader 'bool\s+rtc_interrupt'
Assert-NotContains $powerTask `
    'app_environment_deinit|button_service_stop|app_settings_reset|app_power_trace|power_cycles\.jsonl'
Assert-NotContains 'main\Kconfig.projbuild' 'DESKMATE_POWER_VALIDATION_MODE'
Assert-NotContains 'sdkconfig.defaults' 'CONFIG_DESKMATE_POWER_VALIDATION_MODE'
Assert-NotContains 'main\Kconfig.projbuild' 'DESKMATE_LIGHT_SLEEP_PREPARE_TIMEOUT_MS'
Assert-NotContains 'sdkconfig.defaults' 'CONFIG_DESKMATE_LIGHT_SLEEP_PREPARE_TIMEOUT_MS'

if ($failures.Count -gt 0) {
    Write-Host '按键、Timer 与网络维护低功耗契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host '按键、Timer 与网络维护低功耗契约检查通过。' -ForegroundColor Green
