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

if (Test-Section 'Wake') {
    Assert-Contains 'components\services\button_service\include\button_service.h' `
        'button_service_wakeup_snapshot_t' 'Button Service 未声明轻睡眠唤醒事实'
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

if ($failures.Count -gt 0) {
    Write-Host '按键事件触发扫描契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "按键事件触发扫描契约检查通过：$Section" -ForegroundColor Green
