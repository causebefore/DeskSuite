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

function Invoke-HubUrlHostTest {
    $compiler = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($null -eq $compiler) {
        $compiler = Get-Command gcc.exe -ErrorAction SilentlyContinue
    }
    if ($null -eq $compiler) {
        $failures.Add('缺少可用的 clang.exe 或 gcc.exe，无法编译 Hub URL host-test')
        return
    }

    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $tempDir = Join-Path $tempRoot ("deskmate-hub-url-test-{0}" -f [guid]::NewGuid().ToString('N'))
    $resolvedTempDir = [IO.Path]::GetFullPath($tempDir)
    if (-not $resolvedTempDir.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $failures.Add('Hub URL host-test 临时目录越出系统临时目录')
        return
    }

    New-Item -ItemType Directory -Path $resolvedTempDir | Out-Null
    try {
        $espErrHeader = Join-Path $resolvedTempDir 'esp_err.h'
        @'
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
'@ | Set-Content -LiteralPath $espErrHeader

        $testSource = Join-Path $repoRoot $hubUrlHostTest
        $implementationSource = Join-Path $repoRoot 'main\application\app_network_hub_url.c'
        $applicationInclude = Join-Path $repoRoot 'main\application'
        $executable = Join-Path $resolvedTempDir 'test_app_network_hub_url.exe'
        & $compiler.Source '-std=c11' '-Wall' '-Wextra' '-Werror' `
            "-I$resolvedTempDir" "-I$applicationInclude" `
            $testSource $implementationSource '-o' $executable
        if ($LASTEXITCODE -ne 0) {
            $failures.Add("Hub URL host-test 编译失败（退出码 $LASTEXITCODE）")
            return
        }

        & $executable
        if ($LASTEXITCODE -ne 0) {
            $failures.Add("Hub URL host-test 执行失败（退出码 $LASTEXITCODE）")
        }
    }
    finally {
        if (Test-Path -LiteralPath $resolvedTempDir) {
            Remove-Item -LiteralPath $resolvedTempDir -Recurse -Force
        }
    }
}

Invoke-HubUrlHostTest

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
Assert-Contains $hubUrlHostTest 'assert\s*\(' 'Hub URL host-test 未使用明确断言'
Assert-Contains $hubUrlHostTest 'assert_hub_url_success\s*\(\s*"HTTP://Example\.COM/"\s*,\s*"http://example\.com"\s*\)' 'Hub URL host-test 未断言合法输入的精确规范化输出'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*"https://' 'Hub URL host-test 未断言 https 输入失败'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*"http://user@' 'Hub URL host-test 未断言 userinfo 输入失败'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*"http://[^"]+\?[^"]*"' 'Hub URL host-test 未断言 query 输入失败'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*"http://[^"]+#[^"]*"' 'Hub URL host-test 未断言 fragment 输入失败'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*"http://[^"]+/[^"]+"' 'Hub URL host-test 未断言业务 path 输入失败'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*overlong' 'Hub URL host-test 未断言超长输入失败'
Assert-Contains $hubUrlHostTest 'assert_hub_url_rejected\s*\(\s*non_ascii' 'Hub URL host-test 未断言非 ASCII 输入失败'

Assert-Contains $networkHeader 'app_network_get_hub_settings_snapshot_copy\s*\(' `
    'Network Application 未公开 Hub 设置快照入口'
Assert-Contains $networkHeader 'app_network_request_test_hub_url_copy\s*\(' `
    'Network Application 未公开 Hub 候选测试入口'
Assert-Contains $networkHeader 'app_network_request_update_hub_url_copy\s*\(' `
    'Network Application 未公开 Hub 候选保存入口'
Assert-Contains $networkHeader 'app_network_get_hub_request_result_copy\s*\(' `
    'Network Application 未公开 Hub 测试/保存统一结果查询'
Assert-Contains $networkTask 's_next_hub_request_id\s*==\s*UINT64_MAX' `
    'Hub 请求 ID 未在最大值处拒绝回绕'
Assert-Contains $networkTask 's_hub_request\.result\.state\s*==\s*APP_NETWORK_HUB_REQUEST_STATE_PENDING' `
    'Hub 测试和保存未共享同一个 pending 请求槽'
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
Assert-TextContains $hubHealthCheck 'TRANSPORT_HTTP_GET' `
    'Hub 健康检查必须使用 GET 请求'
Assert-TextContains $hubHealthCheck 'APP_NETWORK_HUB_HEALTH_TIMEOUT_MS' `
    'Hub 健康检查必须使用独立的有界超时'
Assert-TextNotContains $hubHealthCheck 'Authorization|device_token|token' `
    'Hub 健康检查不得携带访问令牌或设备 Token'
Assert-TextContains $hubHealthCheck `
    'cJSON_ParseWithLengthOpts\s*\(\s*response\.body\s*,\s*response\.body_len\s*\+\s*1U\s*,\s*NULL\s*,\s*true\s*\)' `
    'Hub 健康响应必须按实际长度严格解析并要求完整 NUL 结尾，拒绝 trailing garbage'
Assert-TextNotContains $hubHealthCheck 'cJSON_Parse\s*\(' `
    'Hub 健康响应不得使用接受 trailing garbage 的宽松 cJSON_Parse'
Assert-TextContains $hubHealthCheck `
    'strnlen\s*\(\s*response\.body\s*,\s*response\.body_len\s*\+\s*1U\s*\)\s*==\s*response\.body_len' `
    'Hub 健康响应必须拒绝实际 body_len 内夹带 NUL 后垃圾的响应体'
Assert-TextContains $hubHealthCheck `
    'cJSON_IsObject\s*\(\s*root\s*\)[\s\S]{0,180}cJSON_IsString\s*\(\s*status\s*\)[\s\S]{0,180}strcmp\s*\(\s*status->valuestring\s*,\s*"ok"\s*\)\s*==\s*0' `
    'Hub 健康响应必须是顶层 object 且 status 为精确字符串 ok'

$portalSave = Read-CFunction $networkTask 'save_network_config'
Assert-TextInOrder $portalSave @(
    'config->service_url\[0\]\s*!=\s*''\\0''',
    'app_network_hub_url_parse_copy\s*\(\s*config->service_url\s*,\s*normalized_service_url\s*\)',
    's_hub_settings_initialized',
    's_hub_portal_save_pending\s*=\s*true',
    'settings_store_load_copy\s*\(',
    'settings_store_copy_string\s*\(\s*settings\.service_url[\s\S]{0,180}normalized_service_url',
    'settings_store_save\s*\(',
    'if\s*\(\s*error\s*==\s*ESP_OK\s*&&\s*hub_url_supplied\s*\)',
    'settings_store_copy_string\s*\(\s*s_hub_snapshot\.service_url[\s\S]{0,180}normalized_service_url',
    's_hub_snapshot\.version\+\+',
    'invalidate_hub_test_result_for_candidate\s*\(\s*s_hub_snapshot\.service_url\s*,\s*s_hub_snapshot\.version\s*\)',
    's_hub_portal_save_pending\s*=\s*false'
) 'Portal 保存必须先规范化 Hub 地址、互斥持久化，并仅在成功后短锁发布快照、版本与测试失效事实'
Assert-TextContains $portalSave `
    's_hub_snapshot\.version\s*==\s*UINT64_MAX' `
    'Portal Hub 地址保存必须在版本最大值处拒绝回绕'
Assert-TextContains $portalSave `
    'if\s*\(\s*!s_hub_settings_initialized[\s\S]{0,260}return\s+ESP_ERR_INVALID_STATE' `
    'Portal 保存必须在 Hub 初始 snapshot 尚未 ready 时拒绝，避免发布错误初始版本'
Assert-TextContains $portalSave `
    's_hub_request\.valid[\s\S]{0,140}s_hub_request\.result\.state\s*==\s*APP_NETWORK_HUB_REQUEST_STATE_PENDING' `
    'Portal 保存不得与 Web Hub pending 请求并发覆盖 network_cfg'
Assert-TextNotContains $portalSave `
    'settings_store_copy_string\s*\(\s*settings\.service_url[\s\S]{0,180}config->service_url' `
    'Portal 原始 Hub URL 不得绕过纯 helper 直接落盘'
Assert-TextMatchCount $portalSave `
    'settings_store_copy_string\s*\(\s*s_hub_snapshot\.service_url' 1 `
    'Portal Hub 快照只能在持久化成功分支发布一次'
Assert-TextMatchCount $portalSave 's_hub_snapshot\.version\+\+' 1 `
    'Portal Hub 版本只能在持久化成功分支递增一次'
Assert-Contains $networkTask `
    'request_hub_url_copy[\s\S]{0,1600}s_hub_portal_save_pending' `
    'Web Hub 请求入口必须拒绝与 Portal 保存 reservation 并发'

$hubCandidateInvalidation = Read-CFunction $networkTask 'invalidate_hub_test_result_for_candidate'
Assert-TextInOrder $hubCandidateInvalidation @(
    'const\s+char\s*\*\s*candidate',
    'strcmp\s*\([^,]+,\s*candidate\s*\)\s*!=\s*0',
    '(?:candidate_url|service_url)',
    '(?:candidate_version|version)',
    'APP_NETWORK_HUB_TEST_STATE_INVALIDATED'
) 'Hub 候选地址变化必须绑定候选与版本，并清除或降级旧测试结果'
Assert-TextNotContains $hubCandidateInvalidation 'system_storage_(get|set)_network_config|settings_store_save|nvs_' 'Hub 候选地址变化不得持久化配置'

$hubTest = Read-CFunction $networkTask 'handle_hub_test_command'
Assert-TextInOrder $hubTest @(
    'const\s+char\s*\*\s*candidate\s*=\s*command->hub_url',
    'invalidate_hub_test_result_for_candidate\s*\(\s*candidate',
    'perform_hub_health_check\s*\(\s*candidate',
    '(?:result|test_result)\.(?:candidate_url|service_url)[\s\S]{0,180}candidate',
    'finish_hub'
) 'Hub 候选测试必须由网络 Task 实际探测同一地址，并把地址绑定到测试结果'
Assert-TextNotContains $hubTest 'system_storage_(get|set)_network_config|settings_store_save|nvs_' 'Hub 候选测试不得持久化任何配置'

$hubUpdate = Read-CFunction $networkTask 'handle_hub_update_command'
Assert-TextNotContainsBefore $hubUpdate 'system_storage_set_network_config_borrow\s*\(' 'settings_store_copy_string\s*\(\s*s_hub_snapshot\.service_url|s_hub_snapshot\.version\+\+|APP_NETWORK_HUB_REQUEST_STATE_SUCCEEDED' 'Hub 持久化成功前不得发布新的地址、版本或成功结果'
Assert-TextInOrder $hubUpdate @(
    'const\s+char\s*\*\s*candidate\s*=\s*command->hub_url',
    'invalidate_hub_test_result_for_candidate\s*\(\s*candidate',
    'perform_hub_health_check\s*\(\s*candidate',
    'system_storage_get_network_config_copy\s*\(\s*&network_cfg\s*\)',
    'settings_store_copy_string\s*\(\s*network_cfg\.service_url[\s\S]{0,180}candidate',
    'system_storage_set_network_config_borrow\s*\(\s*&network_cfg\s*\)',
    'if\s*\(\s*error\s*==\s*ESP_OK\s*\)',
    'settings_store_copy_string\s*\(\s*s_hub_snapshot\.service_url[\s\S]{0,180}candidate\s*\)',
    's_hub_snapshot\.version\+\+',
    'invalidate_hub_test_result_for_candidate\s*\(\s*s_hub_snapshot\.service_url\s*,\s*s_hub_snapshot\.version\s*\)',
    'APP_NETWORK_HUB_REQUEST_STATE_SUCCEEDED'
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
Assert-TextMatchCount (Read-RepoFile $provider) '\.section_id\s*=\s*"hub"' 2 `
    'Hub Settings 与 Hub Actions 必须分别使用同一个 hub 分区 ID'
Assert-Contains $provider 'app_network_request_test_hub_url_copy\s*\(' `
    'Hub Action 未通过 Network Application 异步测试候选地址'
Assert-Contains $provider 'app_network_request_update_hub_url_copy\s*\(' `
    'Hub Settings 未通过 Network Application 异步保存候选地址'

$remoteLogReconfigure = Read-CFunction $networkTask 'reconfigure_remote_log_after_hub_update'
Assert-TextInOrder $remoteLogReconfigure @(
    'stop_remote_log_upload\s*\(',
    'app_network_get_backend_context_copy\s*\(',
    'remote_log_configure_copy\s*\(',
    'remote_log_start\s*\('
) 'Hub 成功提交后必须按停止、重配、启动顺序最佳努力切换远端日志'
Assert-TextNotContains $remoteLogReconfigure 'settings_store_save\s*\(|system_storage_set_network_config|service_url|device_token' `
    '远端日志重配失败不得回滚设置或记录地址、令牌'

Assert-Contains 'main\application\app_web_console.cpp' 'app_web_console_provider_get_actions_borrow\s*\(' `
    'DeskMate 网页控制台未装配 Hub Actions Provider'
Assert-NotContains 'main\application\app_web_console.cpp' 'web_console_network_provider' `
    'DeskMate 产品设置中心仍装配调试型 Network Status Provider'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_WEB_CONSOLE_ACTIONS=y' `
    'DeskMate 默认构建未启用 Actions 模块'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_SERVER_URL="http://192\.168\.6\.13:8765"' `
    'DeskMate 产品设置默认 Hub 地址未指向 Ubuntu 生产 Hub'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_CONNECT_PORTAL_DEFAULT_SERVICE_URL="http://192\.168\.6\.13:8765"' `
    'DeskMate 配网页默认 Hub 地址未指向 Ubuntu 生产 Hub'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_NETWORK_MANAGER_DEFAULT_SERVICE_URL="http://192\.168\.6\.13:8765"' `
    'DeskMate Network Manager 默认 Hub 地址未指向 Ubuntu 生产 Hub'
Assert-NotContains 'sdkconfig.defaults' '192\.168\.6\.248:8765' `
    'DeskMate 默认配置仍引用已停用的旧 Hub 地址'

Assert-TextMatchCount $hubUpdate 'settings_store_copy_string\s*\(\s*s_hub_snapshot\.service_url' 1 'Hub 新地址只能在 network_cfg 成功分支发布一次'
Assert-TextMatchCount $hubUpdate 's_hub_snapshot\.version\+\+' 1 'Hub 设置版本只能在 network_cfg 成功分支递增一次'
Assert-TextMatchCount $hubUpdate 'invalidate_hub_test_result_for_candidate\s*\(' 2 `
    'Web Hub 更新必须在候选提交前和新版本发布后各失效一次测试结果'
Assert-TextNotContains $hubUpdate '(?:latest|last).*hub.*test.*(?:result|success)' 'Hub 地址变化后不得复用旧候选测试结果替代当前地址重测'

if ($failures.Count -gt 0) {
    Write-Host 'DeskMate Hub 设置契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'DeskMate Hub 设置契约检查通过。' -ForegroundColor Green
