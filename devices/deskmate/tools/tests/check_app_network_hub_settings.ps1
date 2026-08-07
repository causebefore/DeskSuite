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
        "(?s)static\s+\w+[\w\s\*]*\s+$FunctionName\s*\([^)]*\)\s*\{"
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

$networkTask = 'main\application\app_network_task.c'
$networkHeader = 'main\application\app_network.h'
$hubUrlHeader = 'main\application\app_network_hub_url.h'
$hubUrlHostTest = 'tools\tests\test_app_network_hub_url.c'
$provider = 'main\application\app_web_console_provider.c'

Assert-Contains $hubUrlHeader 'APP_NETWORK_HUB_URL_MAX_LENGTH\s+127U' 'Hub URL 纯 helper 未声明 127 字节上限'
Assert-Contains $hubUrlHeader 'app_network_hub_url_parse_copy\s*\(' 'Hub URL 纯 helper 未公开规范化入口'
Assert-Contains $hubUrlHostTest 'app_network_hub_url_parse_copy\s*\(' '缺少 Hub URL 纯 helper 的 host-test 调用'
Assert-Contains $hubUrlHostTest '"HTTP://Example\.COM/"[\s\S]{0,180}"http://example\.com"' 'Hub URL host-test 未覆盖 scheme/host 小写和唯一末尾斜杠'
Assert-Contains $hubUrlHostTest '"http://192\.168\.1\.2:8765/"' 'Hub URL host-test 未覆盖 IPv4 与显式端口'
Assert-Contains $hubUrlHostTest '"http://hub\.example:8765/"' 'Hub URL host-test 未覆盖 hostname 与可选端口'
Assert-Contains $hubUrlHostTest '"https://' 'Hub URL host-test 未拒绝非 http scheme'
Assert-Contains $hubUrlHostTest '"http://user@' 'Hub URL host-test 未拒绝 user info'
Assert-Contains $hubUrlHostTest '"http://[^"]+\?[^"]*"' 'Hub URL host-test 未拒绝 query'
Assert-Contains $hubUrlHostTest '"http://[^"]+#[^"]*"' 'Hub URL host-test 未拒绝 fragment'
Assert-Contains $hubUrlHostTest '"http://[^"]+/[^"]+"' 'Hub URL host-test 未拒绝业务 path'
Assert-Contains $hubUrlHostTest '127U|APP_NETWORK_HUB_URL_MAX_LENGTH' 'Hub URL host-test 未覆盖 ASCII 127 字节边界'
Assert-Contains $hubUrlHostTest '\\x80|0x80|非 ASCII' 'Hub URL host-test 未覆盖非 ASCII 拒绝'

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

$hubTest = Read-CFunction $networkTask 'handle_hub_test_command'
Assert-TextInOrder $hubTest @(
    'const\s+char\s*\*\s*candidate\s*=\s*command->hub_url',
    'perform_hub_health_check\s*\(\s*candidate',
    '(?:result|test_result)\.(?:candidate_url|service_url)[\s\S]{0,180}candidate',
    'finish_hub'
) 'Hub 候选测试必须由网络 Task 实际探测同一地址，并把地址绑定到测试结果'
Assert-TextNotContains $hubTest 'system_storage_(get|set)_network_config|settings_store_save|nvs_' 'Hub 候选测试不得持久化任何配置'

$hubUpdate = Read-CFunction $networkTask 'handle_hub_update_command'
Assert-TextNotContainsBefore $hubUpdate 'system_storage_set_network_config_borrow\s*\(' 'snapshot\.service_url\s*=|snapshot\.version\+\+|APP_NETWORK_HUB_UPDATE_STATE_SUCCEEDED' 'Hub 持久化成功前不得发布新的地址、版本或成功结果'
Assert-TextInOrder $hubUpdate @(
    'const\s+char\s*\*\s*candidate\s*=\s*command->hub_url',
    'perform_hub_health_check\s*\(\s*candidate',
    'system_storage_get_network_config_copy\s*\(\s*&network_cfg\s*\)',
    'settings_store_copy_string\s*\(\s*network_cfg\.service_url[\s\S]{0,180}candidate',
    'system_storage_set_network_config_borrow\s*\(\s*&network_cfg\s*\)',
    'if\s*\(\s*error\s*==\s*ESP_OK\s*\)',
    'snapshot\.service_url\s*=\s*candidate',
    'snapshot\.version\+\+',
    'APP_NETWORK_HUB_UPDATE_STATE_SUCCEEDED'
) 'Hub 保存必须在同一路径重测同一候选地址，并仅在 network_cfg 成功后发布快照、版本与成功结果'
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

Assert-TextMatchCount $hubUpdate 'snapshot\.service_url\s*=\s*candidate' 1 'Hub 新地址只能在 network_cfg 成功分支发布一次'
Assert-TextMatchCount $hubUpdate 'snapshot\.version\+\+' 1 'Hub 设置版本只能在 network_cfg 成功分支递增一次'
Assert-TextNotContains $hubUpdate '(?:latest|last).*hub.*test.*(?:result|success)' 'Hub 地址变化后不得复用旧候选测试结果替代当前地址重测'

if ($failures.Count -gt 0) {
    Write-Host 'DeskMate Hub 设置契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'DeskMate Hub 设置契约检查通过。' -ForegroundColor Green
