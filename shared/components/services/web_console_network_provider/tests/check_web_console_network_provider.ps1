$ErrorActionPreference = 'Stop'

$componentRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..\..')).Path
$sourcePath = Join-Path $componentRoot 'web_console_network_provider.c'
$headerPath = Join-Path $componentRoot 'include\web_console_network_provider.h'
$cmakePath = Join-Path $componentRoot 'CMakeLists.txt'
$consoleRoot = Join-Path $repoRoot 'shared\components\services\web_console_service'
$consoleCmakePath = Join-Path $consoleRoot 'CMakeLists.txt'
$deskmateProjectPath = Join-Path $repoRoot 'devices\deskmate\CMakeLists.txt'
$deskmateMainCmakePath = Join-Path $repoRoot 'devices\deskmate\main\CMakeLists.txt'
$deskmateAppPath = Join-Path $repoRoot 'devices\deskmate\main\application\app_web_console.cpp'
$photopainterProjectPath = Join-Path $repoRoot 'devices\photopainter\CMakeLists.txt'
$communicationRoot = Join-Path $repoRoot 'shared\components\communication'

$source = Get-Content -LiteralPath $sourcePath -Raw
$header = Get-Content -LiteralPath $headerPath -Raw
$cmake = Get-Content -LiteralPath $cmakePath -Raw
$consoleCmake = Get-Content -LiteralPath $consoleCmakePath -Raw
$deskmateProject = Get-Content -LiteralPath $deskmateProjectPath -Raw
$deskmateMainCmake = Get-Content -LiteralPath $deskmateMainCmakePath -Raw
$deskmateApp = Get-Content -LiteralPath $deskmateAppPath -Raw
$photopainterProject = Get-Content -LiteralPath $photopainterProjectPath -Raw
$sourceWithoutComments = [regex]::Replace($source, '(?s)/\*.*?\*/|//[^\r\n]*', '')

function Assert-True {
    param(
        [Parameter(Mandatory)]
        [bool]$Condition,
        [Parameter(Mandatory)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

$diagnosticsCalls = [regex]::Matches(
    $sourceWithoutComments,
    '\bnetwork_manager_get_diagnostics_copy\s*\('
).Count
Assert-True ($diagnosticsCalls -eq 1) 'Provider 必须且只能读取一次完整网络诊断快照。'

Assert-True (
    $sourceWithoutComments -notmatch '\bconnect_[a-zA-Z0-9_]+\s*\('
) 'Provider 不得直接调用 connect。'
Assert-True (
    $sourceWithoutComments -notmatch '\bnetwork_manager_(start|stop|request_|set_notify)'
) 'Provider 不得控制 Network Manager 或注册通知回调。'
Assert-True (
    $sourceWithoutComments -notmatch '\bnetwork_manager_(get_status_copy|get_portal_info_copy|has_saved_config)\s*\('
) 'Provider 不得绕过完整诊断快照引入第二网络事实来源。'
Assert-True (
    $sourceWithoutComments -notmatch '\b(xTaskCreate|xQueueCreate|xTimerCreate|esp_timer_create)\s*\('
) 'Provider 不得创建 Task、Queue 或 Timer。'
Assert-True (
    $sourceWithoutComments -notmatch '"ssid"|\.(ssid|password|device_token|service_url)\b|\bconfig_store\b'
) '首版 Provider 不得读取 SSID 或任何敏感网络配置。'

$fieldIds = @(
    'manager_state'
    'manager_last_error'
    'link_snapshot_error'
    'associated'
    'bssid'
    'primary_channel'
    'rssi_dbm'
    'ipv4'
    'gateway'
    'dns_primary'
    'has_saved_config'
    'portal_active'
)
foreach ($fieldId in $fieldIds) {
    Assert-True (
        $source.Contains('"' + $fieldId + '"')
    ) "缺少网络状态字段：$fieldId"
}
$declaredFieldIds = [regex]::Matches(
    $sourceWithoutComments,
    '\.id\s*=\s*"([^"]+)"'
) | ForEach-Object { $_.Groups[1].Value }
Assert-True (
    $declaredFieldIds.Count -eq $fieldIds.Count
) '网络分区必须恰好声明 12 个字段。'
Assert-True (
    ($declaredFieldIds | Sort-Object -Unique).Count -eq $fieldIds.Count
) '网络分区字段 ID 必须唯一。'
Assert-True (
    [regex]::Matches($sourceWithoutComments, '\.access\s*=\s*NETWORK_FIELD_READ_ONLY').Count -eq $fieldIds.Count
) '网络分区全部字段都必须只读。'
Assert-True (
    [regex]::Matches($sourceWithoutComments, '\.effect\s*=\s*WEB_CONSOLE_FIELD_EFFECT_NONE').Count -eq $fieldIds.Count
) '网络分区全部字段都必须没有设置生效语义。'

Assert-True (
    $header -match '\bweb_console_network_provider_get_status_borrow\s*\(\s*void\s*\)'
) '公共头必须暴露静态 Status Provider 借用接口。'
Assert-True (
    $header -match '(?s)#if\s+CONFIG_WEB_CONSOLE_STATUS.*web_console_network_provider_get_status_borrow\s*\(\s*void\s*\).*#endif'
) '公共接口必须与 Status 实现使用同一配置开关，避免裁剪构建出现无实现声明。'
Assert-True (
    $cmake -match '(?s)REQUIRES\s+web_console_service'
) 'Provider 的公共契约必须依赖 web_console_service。'
Assert-True (
    $cmake -match '(?s)PRIV_REQUIRES\s+network_manager'
) 'Provider 实现必须私有依赖 network_manager。'
Assert-True (
    $consoleCmake -notmatch '\b(connect|network_manager|web_console_network_provider)\b'
) 'Console Core 不得反向依赖 Communication 或网络 Provider。'
$consoleSourceReferences = Get-ChildItem -LiteralPath $consoleRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.cpp', '.h', '.hpp') } |
    Select-String -Pattern '#\s*include\s*[<"](connect|network_manager|web_console_network_provider)\.h[>"]'
Assert-True (
    $null -eq $consoleSourceReferences
) 'Console Core 源码不得包含 Communication 或网络 Provider 头文件。'

Assert-True (
    $deskmateProject -match 'shared/components/services/web_console_network_provider'
) 'DeskMate 必须显式发现网络 Provider 组件。'
Assert-True (
    $deskmateMainCmake -match '(?s)PRIV_REQUIRES.*web_console_network_provider'
) 'DeskMate main 必须显式依赖网络 Provider。'
Assert-True (
    $cmake -match '(?s)if\(CONFIG_WEB_CONSOLE_STATUS\).*web_console_network_provider\.c.*endif\(\)'
) '关闭 Status 后不得编译网络 Provider 实现源码。'
Assert-True (
    $deskmateApp -match '\bweb_console_network_provider_get_status_borrow\s*\('
) 'DeskMate 必须在产品装配点显式取得网络 Provider。'

Assert-True (
    $photopainterProject -notmatch 'web_console_(service|network_provider)'
) 'PhotoPainter 不得发现 Web Console 或网络 Provider。'
$communicationFiles = Get-ChildItem -LiteralPath $communicationRoot -Recurse -File |
    Where-Object {
        $_.Extension -in @('.c', '.cpp', '.h', '.hpp', '.cmake') -or
        $_.Name -in @('CMakeLists.txt', 'Kconfig', 'Kconfig.projbuild')
    }
$communicationReferences = $communicationFiles |
    Select-String -Pattern 'web_console_(service|network_provider)'
Assert-True (
    $null -eq $communicationReferences
) 'Communication 不得反向依赖 Web Console。'

Write-Output 'Web Console 网络 Provider 静态契约检查通过。'
