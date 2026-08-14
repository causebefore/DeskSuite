$ErrorActionPreference = 'Stop'

$componentRoot = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $componentRoot 'src\remote_log.c'
$source = Get-Content -LiteralPath $sourcePath -Raw

function Assert-Match {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Pattern,
        [Parameter(Mandatory = $true)]
        [string] $Message
    )

    if ($source -notmatch $Pattern) {
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

    $start = $source.IndexOf($StartMarker, [System.StringComparison]::Ordinal)
    if ($start -lt 0) {
        throw "无法找到远端日志源码区段：$StartMarker"
    }
    $end = $source.IndexOf($EndMarker, $start + $StartMarker.Length, [System.StringComparison]::Ordinal)
    if ($end -le $start) {
        throw "无法提取远端日志源码区段：$StartMarker"
    }
    return $source.Substring($start, $end - $start)
}

Assert-Match 'static\s+size_t\s+remote_log_utf8_sequence_length\s*\(' `
    '远端日志缺少 UTF-8 字符长度识别函数。'
Assert-Match 'static\s+void\s+remote_log_trim_incomplete_utf8\s*\(' `
    '远端日志缺少 UTF-8 不完整尾部清理函数。'
Assert-Match 'sequence_length\s*==\s*0U\s*\|\|\s*valid_length\s*\+\s*sequence_length\s*>\s*length' `
    '远端日志必须识别非法首字节或被固定缓冲区截断的字符。'
Assert-Match '\(byte\s*&\s*0xC0U\)\s*!=\s*0x80U' `
    '远端日志必须校验 UTF-8 continuation byte。'

$copySection = Get-SectionText 'static void remote_log_copy_text' 'static void remote_log_record_drop'
$captureSection = Get-SectionText 'static void remote_log_capture(' 'void __wrap_esp_log'
Assert-TextMatch $copySection '(?s)destination\[copy_length\]\s*=\s*''\\0''\s*;.*?remote_log_trim_incomplete_utf8\s*\(\s*destination\s*\)' `
    '固定字段复制后必须在目标缓冲区中修复 UTF-8 截断。'
Assert-TextMatch $captureSection '(?s)vsnprintf\s*\(\s*event\.line\.raw.*?strcspn\s*\(.*?remote_log_trim_incomplete_utf8\s*\(\s*event\.line\.raw\s*\).*?remote_log_copy_text\s*\(\s*event\.line\.message' `
    '格式化原始日志后必须先修复 UTF-8，再复制到上传 message 字段。'

# 构造 159 字节截断恰好落在三字节中文字符中间的样本。
$strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
$sampleBytes = [System.Text.Encoding]::UTF8.GetBytes('A' + ('中' * 60))
$truncatedBytes = [byte[]]::new(159)
[System.Array]::Copy($sampleBytes, $truncatedBytes, $truncatedBytes.Length)

try {
    $null = $strictUtf8.GetString($truncatedBytes)
    throw '测试样本没有形成预期的 UTF-8 截断。'
}
catch [System.Text.DecoderFallbackException] {
    # 预期：旧的逐字节截断无法按严格 UTF-8 解码。
}

# 与 C 实现同样从头扫描完整字符，确认裁剪后的数据可严格解码且不损伤完整字符。
$validLength = 0
while ($validLength -lt $truncatedBytes.Length) {
    $lead = $truncatedBytes[$validLength]
    if (($lead -band 0x80) -eq 0) {
        $sequenceLength = 1
    }
    elseif ($lead -ge 0xC2 -and $lead -le 0xDF) {
        $sequenceLength = 2
    }
    elseif ($lead -ge 0xE0 -and $lead -le 0xEF) {
        $sequenceLength = 3
    }
    elseif ($lead -ge 0xF0 -and $lead -le 0xF4) {
        $sequenceLength = 4
    }
    else {
        break
    }

    if ($validLength + $sequenceLength -gt $truncatedBytes.Length) {
        break
    }
    $sequenceComplete = $true
    for ($index = 1; $index -lt $sequenceLength; $index++) {
        if (($truncatedBytes[$validLength + $index] -band 0xC0) -ne 0x80) {
            $sequenceComplete = $false
            break
        }
    }
    if (-not $sequenceComplete) {
        break
    }
    $validLength += $sequenceLength
}

$repairedBytes = [byte[]]::new($validLength)
[System.Array]::Copy($truncatedBytes, $repairedBytes, $validLength)
$decoded = $strictUtf8.GetString($repairedBytes)
if ($decoded -ne ('A' + ('中' * 52))) {
    throw 'UTF-8 边界裁剪损伤了完整字符或保留了不完整尾部。'
}

Write-Host '远端日志 UTF-8 截断契约检查通过。' -ForegroundColor Green
