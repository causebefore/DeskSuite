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

function Read-CFunction {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$FunctionName
    )

    $content = Read-RepoFile $RelativePath
    $signature = [regex]::Match(
        $content,
        "(?s)static\s+\w+[\w\s\*]*\s+$FunctionName\s*\([^)]*\)\s*\{"
    )
    if (-not $signature.Success) {
        $failures.Add("缺少待检查函数: $FunctionName")
        return ''
    }
    $openBrace = $content.IndexOf('{', $signature.Index)
    $depth = 0
    for ($index = $openBrace; $index -lt $content.Length; ++$index) {
        if ($content[$index] -eq '{') {
            ++$depth
        }
        elseif ($content[$index] -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $content.Substring($signature.Index, $index - $signature.Index + 1)
            }
        }
    }
    $failures.Add("函数大括号未闭合: $FunctionName")
    return ''
}

$networkTask = 'main\application\app_network_task.c'
$networkHeader = 'main\application\app_network.h'
$provider = 'main\application\app_web_console_provider.c'

Assert-Contains $networkHeader 'app_network_get_hub_settings_snapshot_copy\s*\(' `
    'Network Application 未公开 Hub 设置快照入口'
Assert-Contains $networkHeader 'app_network_request_test_hub_url_copy\s*\(' `
    'Network Application 未公开 Hub 候选测试入口'
Assert-Contains $networkHeader 'app_network_request_update_hub_url_copy\s*\(' `
    'Network Application 未公开 Hub 候选保存入口'
Assert-Contains $networkTask 'NETWORK_COMMAND_HUB_TEST' `
    '网络 Task 必须拥有 Hub 候选测试命令'
Assert-Contains $networkTask 'NETWORK_COMMAND_HUB_UPDATE' `
    '网络 Task 必须拥有 Hub 候选保存命令'
Assert-Contains $networkTask 'case\s+NETWORK_COMMAND_HUB_TEST\s*:' `
    '唯一网络 Task 未串行处理 Hub 候选测试命令'
Assert-Contains $networkTask 'case\s+NETWORK_COMMAND_HUB_UPDATE\s*:' `
    '唯一网络 Task 未串行处理 Hub 候选保存命令'
Assert-Contains $networkTask 'char\s+hub_url\s*\[\s*APP_NETWORK_HUB_URL_MAX_LENGTH\s*\+\s*1U\s*\]' `
    'Hub 命令未按值携带有界候选地址'

$hubHealthCheck = Read-CFunction $networkTask 'perform_hub_health_check'
Assert-TextInOrder $hubHealthCheck @(
    'const\s+char\s*\*\s*candidate',
    '"%s/healthz"\s*,\s*candidate',
    'transport_http_perform_borrow\s*\('
) 'Hub 健康检查必须对同一候选地址执行有界 /healthz 请求'
Assert-TextContains $hubHealthCheck 'HTTP_METHOD_GET' `
    'Hub 健康检查必须使用 GET 请求'
Assert-TextContains $hubHealthCheck 'APP_NETWORK_HUB_HEALTH_TIMEOUT_MS' `
    'Hub 健康检查必须使用独立的有界超时'
Assert-TextNotContains $hubHealthCheck 'Authorization|device_token|token' `
    'Hub 健康检查不得携带访问令牌或设备 Token'

$hubUpdate = Read-CFunction $networkTask 'handle_hub_update_command'
Assert-TextInOrder $hubUpdate @(
    'const\s+char\s*\*\s*candidate\s*=\s*command->hub_url',
    'perform_hub_health_check\s*\(\s*candidate',
    'system_storage_get_network_config_copy\s*\(\s*&network_cfg\s*\)',
    'settings_store_copy_string\s*\(\s*network_cfg\.service_url[\s\S]{0,180}candidate',
    'system_storage_set_network_config_borrow\s*\(\s*&network_cfg\s*\)'
) 'Hub 保存必须在同一路径重测同一候选地址，并且只在成功后提交 network_cfg'
Assert-TextMatchCount $hubUpdate 'system_storage_set_network_config_borrow\s*\(' 1 `
    'Hub 保存只能提交一次 network_cfg Blob'
Assert-TextNotContains $hubUpdate 'settings_store_save\s*\(|nvs_set_|nvs_commit\s*\(' `
    'Hub 保存不得写入 network_cfg 以外的持久化位置'
Assert-TextNotContains $hubUpdate 'network_cfg\.(ssid|password|device_token)\s*=' `
    'Hub 保存只能替换 network_cfg.service_url'

Assert-Contains $provider '\.section_id\s*=\s*"hub"' '缺少 Hub 设置 Provider'
Assert-Contains $provider '\.id\s*=\s*"hub_url"' 'Hub 设置 Provider 未公开地址字段'
Assert-NotContains $provider 'transport_http_perform_borrow|system_storage_set_network_config_borrow|Authorization|device_token' `
    'Hub Provider 越过 Network Application 直接访问传输、存储或凭据'

if ($failures.Count -gt 0) {
    Write-Host 'DeskMate Hub 设置契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'DeskMate Hub 设置契约检查通过。' -ForegroundColor Green
