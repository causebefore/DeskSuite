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

function Assert-TextContains {
    param(
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Content -notmatch $Pattern) {
        $failures.Add($Message)
    }
}

function Assert-TextNotContains {
    param(
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Content -match $Pattern) {
        $failures.Add($Message)
    }
}

function Assert-TextMatchCount {
    param(
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$ExpectedCount,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $count = [regex]::Matches($Content, $Pattern).Count
    if ($count -ne $ExpectedCount) {
        $failures.Add("$Message（期望 $ExpectedCount，实际 $count）")
    }
}

function Assert-TextInOrder {
    param(
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string[]]$Patterns,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $remaining = $Content
    foreach ($pattern in $Patterns) {
        $match = [regex]::Match($remaining, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if (-not $match.Success) {
            $failures.Add($Message)
            return
        }
        $remaining = $remaining.Substring($match.Index + $match.Length)
    }
}

function Assert-TextNotContainsBefore {
    param(
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$BoundaryPattern,
        [Parameter(Mandatory = $true)][string]$ForbiddenPattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $boundary = [regex]::Match($Content, $BoundaryPattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $boundary.Success) {
        $failures.Add($Message)
        return
    }
    if ($Content.Substring(0, $boundary.Index) -match $ForbiddenPattern) {
        $failures.Add($Message)
    }
}

function Read-CFunction {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    $content = Read-RepoFile $RelativePath
    $signature = [regex]::Match(
        $content,
        "(?s)static\s+void\s+$FunctionName\s*\([^)]*\)\s*\{"
    )
    if (-not $signature.Success) {
        $failures.Add("缺少待检查函数: $FunctionName")
        return ''
    }
    $openBrace = $content.IndexOf('{', $signature.Index)
    $depth = 0
    $inString = $false
    $inCharacter = $false
    $inLineComment = $false
    $inBlockComment = $false
    for ($index = $openBrace; $index -lt $content.Length; ++$index) {
        $character = $content[$index]
        $next = if ($index + 1 -lt $content.Length) { $content[$index + 1] } else { [char]0 }
        if ($inLineComment) {
            if ($character -eq "`n") { $inLineComment = $false }
            continue
        }
        if ($inBlockComment) {
            if ($character -eq '*' -and $next -eq '/') {
                ++$index
                $inBlockComment = $false
            }
            continue
        }
        if ($inString) {
            if ($character -eq '\') { ++$index; continue }
            if ($character -eq '"') { $inString = $false }
            continue
        }
        if ($inCharacter) {
            if ($character -eq '\') { ++$index; continue }
            if ($character -eq "'") { $inCharacter = $false }
            continue
        }
        if ($character -eq '/' -and $next -eq '/') {
            ++$index
            $inLineComment = $true
            continue
        }
        if ($character -eq '/' -and $next -eq '*') {
            ++$index
            $inBlockComment = $true
            continue
        }
        if ($character -eq '"') {
            $inString = $true
            continue
        }
        if ($character -eq "'") {
            $inCharacter = $true
            continue
        }
        if ($character -eq '{') {
            ++$depth
        }
        elseif ($character -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $content.Substring($signature.Index, $index - $signature.Index + 1)
            }
        }
    }
    $failures.Add("函数大括号未闭合: $FunctionName")
    return ''
}

$ownerHeader = 'main\application\app_pomodoro.h'
$ownerInternal = 'main\application\app_pomodoro_internal.h'
$ownerSource = 'main\application\app_pomodoro.c'
$pomodoroTask = 'main\application\app_pomodoro_task.c'
$provider = 'main\application\app_web_console_provider.c'
$storeHeader = 'components\data\pomodoro_store\include\pomodoro_store.h'
$storeSource = 'components\data\pomodoro_store\src\pomodoro_store.c'
$sharedProviderHeader = '..\..\shared\components\services\web_console_service\include\web_console_provider.h'
$sharedProviderRegistry = '..\..\shared\components\services\web_console_service\src\providers\web_console_provider_registry.cpp'
$sharedProviderHttp = '..\..\shared\components\services\web_console_service\src\providers\web_console_provider_http.cpp'

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
Assert-Contains $pomodoroTask 'expected_version\s*!=\s*state->snapshot\.settings_version' `
    'Pomodoro Task 执行点未重检设置版本'
Assert-Contains $pomodoroTask 'run_state\s*!=\s*APP_POMODORO_RUN_STATE_IDLE' `
    'Pomodoro Task 执行点未重检 IDLE'
$pomodoroUpdate = Read-CFunction $pomodoroTask 'update_settings_locked'
Assert-TextNotContainsBefore $pomodoroUpdate `
    'pomodoro_store_save_settings_copy\s*\(' `
    'state->snapshot\.(?:settings|settings_version|settings_saved|phase_duration_seconds|remaining_seconds|generation)\s*=(?!=)|state->snapshot\.settings_version\+\+|APP_POMODORO_SETTINGS_UPDATE_STATE_SUCCEEDED' `
    '番茄钟在持久化前不得改变设置快照、阶段派生字段、代次或完成成功结果'
Assert-TextInOrder $pomodoroUpdate @(
    'const\s+app_pomodoro_settings_t\s+candidate\s*=\s*update->settings',
    'pomodoro_store_settings_t\s+stored\s*=\s*\{[\s\S]{0,320}candidate\.focus_minutes[\s\S]{0,160}candidate\.short_break_minutes[\s\S]{0,160}candidate\.long_break_minutes[\s\S]{0,160}candidate\.long_break_interval',
    'memcpy\s*\(\s*stored\.completion_audio_path\s*,\s*candidate\.completion_audio_path',
    'xSemaphoreGive\s*\(\s*g_app_pomodoro_runtime\.state_lock\s*\)',
    'pomodoro_store_save_settings_copy\s*\(',
    'xSemaphoreTake\s*\(\s*g_app_pomodoro_runtime\.state_lock\s*,\s*portMAX_DELAY\s*\)',
    'if\s*\(\s*error\s*==\s*ESP_OK\s*\)',
    'state->snapshot\.settings\s*=\s*candidate',
    'state->snapshot\.settings_version\+\+',
    'state->snapshot\.settings_saved\s*=\s*true',
    'finish_settings_update_locked\s*\(\s*command->settings_request_id'
) '番茄钟必须在同一路径中先锁外持久化候选，成功后才锁内发布设置、版本与成功结果'
Assert-TextContains $pomodoroUpdate `
    'if\s*\(\s*error\s*==\s*ESP_OK\s*\)\s*\{[\s\S]{0,640}state->snapshot\.settings\s*=\s*candidate[\s\S]{0,240}state->snapshot\.settings_version\+\+[\s\S]{0,240}state->snapshot\.phase_duration_seconds\s*=[\s\S]{0,240}state->snapshot\.remaining_seconds\s*=[\s\S]{0,240}state->snapshot\.settings_saved\s*=\s*true[\s\S]{0,240}state->snapshot\.last_error\s*=\s*ESP_OK[\s\S]{0,240}state->snapshot\.generation\s*=' `
    '番茄钟只可在持久化成功分支公开候选设置、版本、阶段派生字段、已保存事实与新代次'
Assert-TextMatchCount $pomodoroUpdate 'state->snapshot\.settings\s*=\s*candidate' 1 `
    '番茄钟候选设置只能在持久化成功分支发布一次'
Assert-TextMatchCount $pomodoroUpdate 'state->snapshot\.settings_version\+\+' 1 `
    '番茄钟设置版本只能在持久化成功分支递增一次'
Assert-TextMatchCount $pomodoroUpdate 'state->snapshot\.phase_duration_seconds\s*=' 1 `
    '番茄钟阶段总时长只能在持久化成功分支更新一次'
Assert-TextMatchCount $pomodoroUpdate 'state->snapshot\.remaining_seconds\s*=' 1 `
    '番茄钟剩余时长只能在持久化成功分支更新一次'
Assert-TextMatchCount $pomodoroUpdate 'state->snapshot\.generation\s*=' 1 `
    '番茄钟代次只能在持久化成功分支更新一次'
Assert-TextNotContains $pomodoroUpdate 'state->snapshot\.settings\s*=\s*update->settings' `
    '番茄钟不得在持久化前直接公开 update 设置'
Assert-TextNotContains $pomodoroUpdate 'state->snapshot\.settings_saved\s*=\s*false' `
    '番茄钟持久化失败不得覆盖旧的已保存事实'
Assert-TextContains $pomodoroUpdate `
    'else\s*\{\s*state->snapshot\.last_error\s*=\s*error\s*;\s*\}\s*finish_settings_update_locked\s*\(\s*command->settings_request_id' `
    '番茄钟持久化失败只可更新 last_error 并收敛原请求终态'
Assert-TextContains $pomodoroUpdate `
    'finish_settings_update_locked\s*\(\s*command->settings_request_id\s*,\s*error\s*==\s*ESP_OK\s*\?\s*APP_POMODORO_SETTINGS_UPDATE_STATE_SUCCEEDED\s*:\s*APP_POMODORO_SETTINGS_UPDATE_STATE_FAILED\s*,\s*error\s*\)' `
    'Pomodoro 持久化失败路径未在同一事务中完成 FAILED 结果'

foreach ($fieldId in @(
    'focus_minutes',
    'short_break_minutes',
    'long_break_minutes',
    'long_break_interval',
    'completion_audio_path'
)) {
    Assert-Contains $provider "\.id\s*=\s*`"$fieldId`"" `
        "DeskMate Settings Provider 缺少字段 $fieldId"
}
Assert-MatchCount $provider 'WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY' 5 `
    '五项番茄钟设置未全部声明 IDLE_ONLY'
Assert-Contains $provider '\.id\s*=\s*"focus_minutes"[\s\S]{0,320}\.minimum\s*=\s*5[\s\S]{0,100}\.maximum\s*=\s*180[\s\S]{0,100}\.step\s*=\s*1U' `
    '专注时长范围或步长不符合契约'
Assert-Contains $provider '\.id\s*=\s*"short_break_minutes"[\s\S]{0,320}\.minimum\s*=\s*5[\s\S]{0,100}\.maximum\s*=\s*180[\s\S]{0,100}\.step\s*=\s*1U' `
    '短休时长范围或步长不符合契约'
Assert-Contains $provider '\.id\s*=\s*"long_break_minutes"[\s\S]{0,320}\.minimum\s*=\s*5[\s\S]{0,100}\.maximum\s*=\s*180[\s\S]{0,100}\.step\s*=\s*1U' `
    '长休时长范围或步长不符合契约'
Assert-Contains $provider '\.id\s*=\s*"long_break_interval"[\s\S]{0,320}\.minimum\s*=\s*2[\s\S]{0,100}\.maximum\s*=\s*12[\s\S]{0,100}\.step\s*=\s*1U' `
    '长休间隔范围或步长不符合契约'
Assert-Contains $provider `
    'completion_audio_path[\s\S]{0,260}WEB_CONSOLE_FIELD_TYPE_STRING[\s\S]{0,260}\.file_suffix\s*=\s*"\.mp3"' `
    '完成音乐未声明为 Files 支持的 MP3 字符串字段'
Assert-Contains $provider `
    'POMODORO_FIELD_COMPLETION_AUDIO_PATH[\s\S]{0,520}data\.string_value' `
    '完成音乐路径未进入 Pomodoro Settings 快照'
Assert-Contains $ownerHeader 'char\s+completion_audio_path\[' `
    'Pomodoro 设置未保存完成音乐逻辑路径'
Assert-Contains $storeHeader 'POMODORO_STORE_SCHEMA_VERSION\s+2U' `
    'Pomodoro Store 未升级到包含完成音乐路径的 schema 2'
Assert-Contains $storeHeader 'bool\s+migration_required' `
    'Pomodoro Store 未公开旧 schema 迁移事实'
Assert-Contains $storeHeader 'pomodoro_store_settings_are_valid\s*\(' `
    'Pomodoro Store 未公开统一设置 schema 校验'
Assert-Contains $ownerSource 'pomodoro_store_settings_are_valid\s*\(' `
    'Pomodoro Application 未复用 Store 的统一设置校验'
Assert-Contains $storeSource 'POMODORO_LEGACY_SCHEMA_VERSION\s+1U' `
    'Pomodoro Store 未兼容 schema 1'
Assert-Contains $storeSource `
    'nvs_set_str\s*\(\s*handle\s*,\s*"complete_mp3"\s*,\s*settings->completion_audio_path\s*\)' `
    'Pomodoro Store 未持久化完成音乐路径'
Assert-Contains $sharedProviderHeader 'const char\s+\*file_suffix' `
    '通用字段元数据未声明文件后缀'
Assert-InOrder $sharedProviderRegistry @(
    'source->file_suffix',
    'web_console_copy_string\s*\(\s*source->file_suffix',
    'destination->file_suffix'
) '通用 Provider Registry 未深复制文件后缀元数据'
Assert-Contains $sharedProviderHttp `
    'cJSON_AddStringToObject\s*\(\s*object\s*,\s*"fileSuffix"\s*,\s*field->file_suffix\s*\)' `
    'Capabilities 未编码文件选择后缀'

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
Assert-Contains $provider '\.section_id\s*=\s*"hub"[\s\S]{0,160}\.label\s*=\s*"Hub"' `
    '设置首页缺少 Hub 分组'
Assert-Contains $provider '\.section_id\s*=\s*"pomodoro"[\s\S]{0,160}\.label\s*=\s*"番茄钟"' `
    '设置首页缺少番茄钟分组'
Assert-Contains $provider '\.section_id\s*=\s*"system"[\s\S]{0,160}\.label\s*=\s*"设备与系统"' `
    '设置首页缺少设备与系统分组'
Assert-NotContains $provider '\.section_id\s*=\s*"network"' `
    '设置首页不得保留独立网络分组'
Assert-NotContains $provider '\.section_id\s*=\s*"(?!hub"|pomodoro"|system")' '设置首页不得出现 Hub、番茄钟、设备与系统以外的分组'
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
Assert-Contains 'sdkconfig.defaults' 'CONFIG_WEB_CONSOLE_FILES=y' `
    'DeskMate 默认构建未启用完成音乐选择所需的 Files 模块'

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
Assert-Contains $pomodoroTask `
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
