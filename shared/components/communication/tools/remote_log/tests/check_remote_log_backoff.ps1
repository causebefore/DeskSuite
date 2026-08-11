$ErrorActionPreference = 'Stop'

$componentRoot = Split-Path -Parent $PSScriptRoot
$taskSourcePath = Join-Path $componentRoot 'src\remote_log_task.c'
$taskSource = Get-Content -LiteralPath $taskSourcePath -Raw

function Assert-Match {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Pattern,
        [Parameter(Mandatory = $true)]
        [string] $Message
    )

    if ($taskSource -notmatch $Pattern) {
        throw $Message
    }
}

function Assert-TextMatch {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Text,
        [Parameter(Mandatory = $true)]
        [string] $Pattern,
        [Parameter(Mandatory = $true)]
        [string] $Message
    )

    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

function Get-SectionText {
    param(
        [Parameter(Mandatory = $true)]
        [string] $StartMarker,
        [Parameter(Mandatory = $true)]
        [string] $EndMarker
    )

    $start = $taskSource.IndexOf($StartMarker, [System.StringComparison]::Ordinal)
    if ($start -lt 0) {
        throw "无法找到远端日志源码区段：$StartMarker"
    }
    $end = $taskSource.IndexOf($EndMarker, $start + $StartMarker.Length, [System.StringComparison]::Ordinal)
    if ($end -le $start) {
        throw "无法提取远端日志源码区段：$StartMarker"
    }
    return $taskSource.Substring($start, $end - $start)
}

Assert-Match 's_retry_backoff_multipliers\s*\[\s*\]\s*=\s*\{\s*1U\s*,\s*5U\s*,\s*15U\s*,\s*60U\s*\}' `
    '远端日志失败重试必须使用 1、5、15、60 四档退避倍率。'
Assert-Match 'static\s+uint32_t\s+remote_log_retry_delay_ms\s*\(\s*uint8_t\s+retry_stage\s*\)' `
    '远端日志缺少按失败阶段计算退避时间的有界辅助函数。'
Assert-Match 'base_delay\s*>\s*UINT32_MAX\s*/\s*multiplier\s*\?\s*UINT32_MAX' `
    '远端日志退避乘法必须饱和，不能因配置过大而回绕成高频重试。'
Assert-Match 'static\s+uint8_t\s+remote_log_next_retry_stage\s*\(\s*uint8_t\s+retry_stage\s*\)' `
    '远端日志缺少在最后一档封顶的失败阶段推进函数。'
Assert-Match 'static\s+void\s+remote_log_wait_retry\s*\(\s*uint8_t\s+retry_stage\s*\)' `
    '远端日志等待函数必须显式接收当前失败阶段。'

$sessionSection = Get-SectionText 'static bool remote_log_ensure_session' 'static bool remote_log_receive_event'
$batchSection = Get-SectionText 'static bool remote_log_upload_batch' 'static void remote_log_task'
Assert-TextMatch $sessionSection '(?s)uint8_t\s+retry_stage\s*=\s*0U\s*;.*?remote_log_wait_retry\s*\(\s*retry_stage\s*\)\s*;.*?retry_stage\s*=\s*remote_log_next_retry_stage\s*\(\s*retry_stage\s*\)' `
    '启动日志会话失败后必须推进有界退避阶段。'
Assert-TextMatch $batchSection '(?s)uint8_t\s+retry_stage\s*=\s*0U\s*;.*?remote_log_wait_retry\s*\(\s*retry_stage\s*\)\s*;.*?retry_stage\s*=\s*remote_log_next_retry_stage\s*\(\s*retry_stage\s*\)' `
    '日志批次上传失败后必须推进有界退避阶段。'

Write-Host '远端日志失败退避契约检查通过。' -ForegroundColor Green
