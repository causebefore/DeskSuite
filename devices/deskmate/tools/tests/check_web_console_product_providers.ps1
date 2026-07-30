$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()
$cache = @{}

function Read-RepoFile {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    if ($cache.ContainsKey($RelativePath)) {
        return $cache[$RelativePath]
    }
    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("缺少待检查文件: $RelativePath")
        $cache[$RelativePath] = ''
        return ''
    }
    $cache[$RelativePath] = Get-Content -Raw -LiteralPath $path
    return $cache[$RelativePath]
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

function Assert-MatchCount {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$ExpectedCount,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $count = [regex]::Matches((Read-RepoFile $RelativePath), $Pattern).Count
    if ($count -ne $ExpectedCount) {
        $failures.Add("$Message（期望 $ExpectedCount，实际 $count）")
    }
}

function Assert-InOrder {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string[]]$Patterns,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $content = Read-RepoFile $RelativePath
    $offset = 0
    foreach ($pattern in $Patterns) {
        $match = [regex]::Match($content, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if (-not $match.Success -or $match.Index -lt $offset) {
            $failures.Add($Message)
            return
        }
        $offset = $match.Index + $match.Length
        $content = $content.Substring($offset)
        $offset = 0
    }
}

$ownerHeader = 'main\application\app_pomodoro.h'
$ownerInternal = 'main\application\app_pomodoro_internal.h'
$ownerSource = 'main\application\app_pomodoro.c'
$ownerTask = 'main\application\app_pomodoro_task.c'
$provider = 'main\application\app_web_console_provider.c'

Assert-Contains $ownerHeader 'uint64_t\s+settings_version' `
    'Pomodoro 快照未声明独立 64 位设置版本'
Assert-Contains $ownerHeader 'uint64_t\s+expected_version[\s\S]{0,120}app_pomodoro_settings_update_t' `
    'Pomodoro 设置更新未携带期望版本'
foreach ($state in @('PENDING', 'SUCCEEDED', 'FAILED')) {
    Assert-Contains $ownerHeader "APP_POMODORO_SETTINGS_UPDATE_STATE_$state" `
        "Pomodoro 设置结果缺少 $state 状态"
}
Assert-Contains $ownerHeader 'app_pomodoro_validate_settings_update\s*\(' `
    'Pomodoro Owner 未暴露无副作用设置校验'
Assert-Contains $ownerHeader 'app_pomodoro_request_update_settings_copy\s*\(' `
    'Pomodoro Owner 未暴露版本化异步设置入口'
Assert-Contains $ownerHeader 'app_pomodoro_get_settings_update_result_copy\s*\(' `
    'Pomodoro Owner 未提供按请求 ID 查询结果的入口'

Assert-Contains $ownerInternal 'uint32_t\s+next_reconcile_request_id' `
    'Pomodoro 睡眠补算请求 ID 已丢失'
Assert-Contains $ownerInternal 'uint64_t\s+next_settings_request_id' `
    'Pomodoro 设置请求 ID 未与睡眠补算 ID 分离'
Assert-Contains $ownerInternal 'uint64_t\s+settings_request_id' `
    'Pomodoro 设置命令未携带独立请求 ID'
Assert-Contains $ownerSource 'next_settings_request_id\s*==\s*UINT64_MAX' `
    'Pomodoro 设置请求 ID 未在最大值处拒绝回绕'
Assert-Contains $ownerSource 'latest_settings_update_result\.state[\s\S]{0,120}APP_POMODORO_SETTINGS_UPDATE_STATE_PENDING' `
    'Pomodoro Owner 未实现单 pending 设置请求门'
Assert-Contains $ownerSource 'xQueueSend\s*\([^,]+,\s*&command,\s*0\)' `
    'Pomodoro 设置请求未使用零等待复制入队'
Assert-Contains $ownerTask 'expected_version\s*!=\s*state->snapshot\.settings_version' `
    'Pomodoro Task 执行点未重检设置版本'
Assert-Contains $ownerTask 'run_state\s*!=\s*APP_POMODORO_RUN_STATE_IDLE' `
    'Pomodoro Task 执行点未重检 IDLE'
Assert-InOrder $ownerTask @(
    'snapshot\.settings\s*=\s*update->settings',
    'snapshot\.settings_version\+\+',
    'xSemaphoreGive\s*\(\s*g_app_pomodoro_runtime\.state_lock\s*\)',
    'pomodoro_store_save_settings_copy\s*\(',
    'xSemaphoreTake\s*\(\s*g_app_pomodoro_runtime\.state_lock'
) 'Pomodoro 设置未按“内存与版本 → 解锁 → NVS → 重新加锁”顺序执行'
Assert-Contains $ownerTask 'APP_POMODORO_SETTINGS_UPDATE_STATE_FAILED[\s\S]{0,160}error' `
    'Pomodoro NVS 失败未形成带真实错误的 FAILED 终态'

foreach ($fieldId in @(
    'focus_minutes',
    'short_break_minutes',
    'long_break_minutes',
    'long_break_interval'
)) {
    Assert-Contains $provider "\.id\s*=\s*`"$fieldId`"" `
        "DeskMate Settings Provider 缺少字段 $fieldId"
}
Assert-MatchCount $provider 'WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY' 4 `
    '四项番茄钟设置未全部声明 IDLE_ONLY'
Assert-Contains $provider '\.minimum\s*=\s*5[\s\S]{0,100}\.maximum\s*=\s*90[\s\S]{0,100}\.step\s*=\s*5U' `
    '专注时长范围或步长不符合契约'
Assert-Contains $provider '\.minimum\s*=\s*1[\s\S]{0,100}\.maximum\s*=\s*30[\s\S]{0,100}\.step\s*=\s*1U' `
    '短休时长范围或步长不符合契约'
Assert-Contains $provider '\.minimum\s*=\s*5[\s\S]{0,100}\.maximum\s*=\s*60[\s\S]{0,100}\.step\s*=\s*5U' `
    '长休时长范围或步长不符合契约'
Assert-Contains $provider '\.minimum\s*=\s*2[\s\S]{0,100}\.maximum\s*=\s*8[\s\S]{0,100}\.step\s*=\s*1U' `
    '长休间隔范围或步长不符合契约'

foreach ($fieldId in @(
    'firmware_version',
    'build_time',
    'uptime_sec',
    'sram_free_kb',
    'psram_free_kb',
    'cpu_mhz',
    'reset_reason'
)) {
    Assert-Contains $provider "\.id\s*=\s*`"$fieldId`"" `
        "DeskMate Status Provider 缺少字段 $fieldId"
}
Assert-MatchCount $provider '\.access\s*=\s*SYSTEM_STATUS_READ_ONLY' 7 `
    '七项系统状态未全部声明只读'
Assert-MatchCount $provider '\.effect\s*=\s*WEB_CONSOLE_FIELD_EFFECT_NONE' 7 `
    '七项系统状态未全部声明无副作用'
Assert-MatchCount $provider 'system_info_get_snapshot_copy\s*\(' 1 `
    'System Status 回调每次应只读取一份系统快照'

Assert-Contains $provider '#include\s+"app_pomodoro\.h"' `
    '产品 Provider 未通过 Pomodoro 公共 API 适配设置'
Assert-Contains $provider '#include\s+"system_info\.h"' `
    '产品 Provider 未通过 System 公共 API 适配状态'
Assert-NotContains $provider 'app_pomodoro_internal|g_app_pomodoro_runtime|pomodoro_store|nvs_|httpd_|xTaskCreate|xQueueCreate|esp_timer_create' `
    '产品 Provider 越过公共 API 或拥有了 Task、Queue、Timer、NVS、HTTPD 状态'
Assert-NotContains $provider '\.id\s*=\s*"(ssid|password|token|ota|dashboard)' `
    '产品 Provider 暴露了凭据、OTA、Dashboard 或无线网络敏感字段'

Assert-Contains 'main\application\app_web_console.cpp' `
    'app_web_console_provider_get_settings_borrow\s*\(&settings_provider_count\)' `
    '网页控制台未显式装配 DeskMate Settings Provider'
Assert-Contains 'main\application\app_web_console.cpp' `
    'app_web_console_provider_get_status_borrow\s*\(&status_provider_count\)' `
    '网页控制台未显式装配 DeskMate Status Provider'
Assert-Contains 'main\CMakeLists.txt' 'application/app_web_console_provider\.c' `
    'DeskMate main 组件未编译产品 Provider'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_WEB_CONSOLE_SETTINGS=y' `
    'DeskMate 默认构建未启用 Settings 模块'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_WEB_CONSOLE_STATUS=y' `
    'DeskMate 默认构建未启用 Status 模块'

Assert-Contains 'main\presentation\pomodoro_presenter.h' 'uint64_t\s+settings_version' `
    'Pomodoro View Model 未携带设置版本'
Assert-Contains 'main\ui\include\ui_runtime.h' 'uint64_t\s+expected_version' `
    '本机番茄钟设置意图未携带期望版本'
Assert-Contains 'main\ui\pages\ui_settings_page.c' '\.expected_version\s*=\s*view\.settings_version' `
    '本机设置草稿未保存开始编辑时的版本'
Assert-Contains 'main\ui\pages\ui_settings_page.c' `
    'intent_result\.request_id' `
    '本机设置页未保留 Owner 返回的异步设置请求 ID'
Assert-Contains 'main\ui\pages\ui_settings_page.c' `
    'view\.settings_update_request_id\s*==\s*s_view\.pomodoro_settings_request_id' `
    '本机设置页未按请求 ID 匹配异步终态'
Assert-Contains 'main\ui\pages\ui_settings_page.c' `
    'POMODORO_VIEW_SETTINGS_UPDATE_FAILED[\s\S]{0,180}view\.settings_update_error' `
    '本机设置页未呈现匹配请求的 FAILED 终态'
Assert-Contains 'main\ui\core\ui_main.c' `
    'current_page\s*==\s*PRESENTATION_PAGE_POMODORO[\s\S]{0,120}current_page\s*==\s*PRESENTATION_PAGE_SETTINGS' `
    'Pomodoro 终态事件未在设置顶层页自动刷新当前子页'
Assert-Contains 'main\presentation\pomodoro_presenter.h' `
    'settings_update_request_id[\s\S]{0,180}settings_update_error' `
    'Pomodoro View Model 未携带设置请求终态'
Assert-Contains $ownerTask `
    'latest_settings_request_id[\s\S]{0,260}latest_settings_update_result' `
    'Pomodoro Task 未把最新请求 ID 与结果按同一快照推送'
Assert-Contains 'main\application\app_web_console.cpp' `
    'callback_error\s*!=\s*ESP_OK[\s\S]{0,420}s_initialized\s*=\s*false' `
    '网页控制台网络回调注册失败后未恢复未初始化状态'
Assert-InOrder 'main\app_main.c' @(
    'error\s*=\s*app_pomodoro_init\s*\(\s*\)',
    'error\s*=\s*app_web_console_init\s*\(\s*\)'
) 'Composition Root 未先初始化 Pomodoro Owner 再初始化网页控制台'
Assert-Contains 'main\app_main.c' `
    'rollback_web_console_service_init\s*\(\s*\);\s*if\s*\(\s*pomodoro_initialized\s*\)[\s\S]{0,180}app_pomodoro_deinit\s*\(\s*\)' `
    '初始化失败时未先回滚网页控制台再反初始化 Pomodoro Owner'

if ($failures.Count -gt 0) {
    Write-Host 'DeskMate 网页控制台产品 Provider 契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'DeskMate 网页控制台产品 Provider 契约检查通过。' -ForegroundColor Green
