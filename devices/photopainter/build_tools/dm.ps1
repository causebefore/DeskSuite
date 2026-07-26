# PhotoPainter Device 构建工具（薄 wrapper，转发到 dm.py）
# 用法: .\build_tools\dm.ps1 <命令> [-ServiceRoot <服务端目录>] [-Port COMx] [-FullLog] [-TailLines N] [-Detach]
# 命令: build / build-log / flash / flash-font / monitor / flash-monitor / kill-port / clean / menuconfig / ota / flash-log / monitor-log

param(
    [Parameter(Position = 0)]
    [string]$Command,

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

$script:PyExe = "C:\Users\lbq08\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe"
$script:DmPy = Join-Path $PSScriptRoot "dm.py"

if (-not (Test-Path -LiteralPath $script:PyExe)) {
    Write-Host "`n❌ 固定 Python 不存在：$($script:PyExe)" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $script:DmPy)) {
    Write-Host "`n❌ dm.py 不存在：$($script:DmPy)" -ForegroundColor Red
    exit 1
}

$pyArgs = @()
if ($Command) { $pyArgs += $Command }
if ($ServiceRoot) { $pyArgs += "--service-root", $ServiceRoot }
if ($Port) { $pyArgs += "--port", $Port }
if ($FullLog) { $pyArgs += "--full-log" }
if ($TailLines) { $pyArgs += "--tail-lines", $TailLines }
if ($Detach) { $pyArgs += "--detach" }
if ($RemainingArgs) { $pyArgs += $RemainingArgs }

& $script:PyExe $script:DmPy @pyArgs
exit $LASTEXITCODE
