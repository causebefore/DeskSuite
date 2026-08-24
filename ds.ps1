# DeskSuite 统一设备构建入口。
# 用法：.\ds.ps1 <命令> <产品> [选项]
# ota 默认发布到 products.toml 配置的 Ubuntu Hub；-ServiceRoot 显式改为本地发布。

param(
    [Parameter(Position = 0)]
    [string]$Command,

    [Parameter(Position = 1)]
    [string]$Product,

    [string]$ServiceRoot,
    [string]$Port,
    [switch]$FullLog,

    [ValidateRange(1, 500)]
    [int]$TailLines,

    [switch]$Detach,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$defaultPythonExe = "C:\Users\lbq08\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe"
$pythonExe = if ([string]::IsNullOrWhiteSpace($env:DESKSUITE_PYTHON_PATH)) {
    $defaultPythonExe
} else {
    $env:DESKSUITE_PYTHON_PATH
}
$toolPath = Join-Path $PSScriptRoot "build_tools\ds.py"

if (-not (Test-Path -LiteralPath $pythonExe)) {
    Write-Host "`n❌ 固定 Python 不存在：$pythonExe" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $toolPath)) {
    Write-Host "`n❌ DeskSuite 构建工具不存在：$toolPath" -ForegroundColor Red
    exit 1
}

$toolArgs = @()
if ($Command) { $toolArgs += $Command }
if ($Product) { $toolArgs += $Product }
if ($ServiceRoot) { $toolArgs += "--service-root", $ServiceRoot }
if ($Port) { $toolArgs += "--port", $Port }
if ($FullLog) { $toolArgs += "--full-log" }
if ($TailLines) { $toolArgs += "--tail-lines", $TailLines }
if ($Detach) { $toolArgs += "--detach" }
if ($RemainingArgs) { $toolArgs += $RemainingArgs }

& $pythonExe $toolPath @toolArgs
exit $LASTEXITCODE
