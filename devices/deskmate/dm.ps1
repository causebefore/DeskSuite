# DeskMate 构建工具脚本
# 用法: .\dm.ps1 <命令>
# 命令: build, ota, flash, flash-font, monitor, flash-monitor, kill-port, clean, set-target-s3, menuconfig

param(
    [Parameter(Position=0)]
    [ValidateSet("build", "ota", "flash", "flash-font", "monitor", "flash-monitor", "kill-port", "clean", "set-target-s3", "menuconfig")]
    [string]$Command,

    [string]$Port = "COM4"
)

# ESP-IDF 组件脚本可能使用 Python 默认编码读取 UTF-8 配置文件。
# 统一当前控制台与所有子进程的编码，避免 Windows GBK 环境下解析 sdkconfig 失败。
$scriptUtf8Encoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $scriptUtf8Encoding
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"

# 颜色输出
function Write-Step {
    param([string]$Message)
    Write-Host "`n═══════════════════════════════════════════════════════════" -ForegroundColor Cyan
    Write-Host "  $Message" -ForegroundColor Cyan
    Write-Host "═══════════════════════════════════════════════════════════`n" -ForegroundColor Cyan
}

function Write-Error-Exit {
    param([string]$Message)
    Write-Host "`n❌ $Message" -ForegroundColor Red
    exit 1
}

function Get-FontBinPath {
    $searchRoots = @(
        (Join-Path $PSScriptRoot "tools\fonts2bin"),
        (Join-Path (Split-Path $PSScriptRoot -Parent) "tools\fonts2bin")
    )

    foreach ($searchRoot in $searchRoots) {
        if (-not (Test-Path $searchRoot)) {
            continue
        }

        $fontBin = Get-ChildItem -Path $searchRoot -Recurse -File -Filter "font.bin" |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($fontBin) {
            return $fontBin.FullName
        }
    }

    return $null
}

function Get-FontPartitionOffset {
    $partitionFile = "partitions.csv"
    if (-not (Test-Path $partitionFile)) {
        return $null
    }

    $line = Get-Content $partitionFile | Where-Object {
        $_ -match '^\s*font\s*,'
    } | Select-Object -First 1

    if (-not $line) {
        return $null
    }

    $parts = $line.Split(',') | ForEach-Object { $_.Trim() }
    if ($parts.Count -lt 4 -or [string]::IsNullOrWhiteSpace($parts[3])) {
        return $null
    }

    return $parts[3]
}

function Invoke-DevOtaVersion {
    param(
        [ValidateSet("publish")]
        [string]$Action
    )

    $scriptPath = Join-Path $PSScriptRoot "tools\dev_ota_version.ps1"
    if (-not (Test-Path $scriptPath)) {
        Write-Error-Exit "找不到开发 OTA 版本脚本: $scriptPath"
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath -Action $Action
    if ($LASTEXITCODE -ne 0) {
        Write-Error-Exit "开发 OTA 版本脚本执行失败: $Action"
    }
}

# 初始化 ESP-IDF 环境
function Initialize-IdfEnv {
    $profilePath = "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
    if (-not (Test-Path $profilePath)) {
        Write-Error-Exit "找不到 ESP-IDF 环境脚本: $profilePath"
    }
    . $profilePath
    Write-Host "✅ ESP-IDF v6.0.1 环境已加载" -ForegroundColor Green
}

# 强制释放 COM 端口
function Stop-ComPort {
    param([string]$PortName)

    Write-Step "释放 $PortName 端口"

    # 终止占用串口的进程
    $processNames = @("idf_monitor.py", "esp_idf_monitor", "python.exe", "python3.exe")

    foreach ($procName in $processNames) {
        $procs = Get-Process -Name $procName -ErrorAction SilentlyContinue
        foreach ($proc in $procs) {
            try {
                $proc.Modules | Where-Object { $_.ModuleName -like "*serial*" -or $_.ModuleName -like "*com*" } | Out-Null
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                Write-Host "  终止进程: $($proc.ProcessName) (PID: $($proc.Id))" -ForegroundColor Yellow
            } catch {
                # 忽略无法访问的进程
            }
        }
    }

    # 使用 mode 命令重置端口
    try {
        & mode $PortName BAUD=9600 PARITY=N DATA=8 STOP=1 2>$null | Out-Null
        Write-Host "  端口 $PortName 已重置" -ForegroundColor Green
    } catch {
        # 端口可能未被占用
    }

    Start-Sleep -Seconds 1
    Write-Host "✅ $PortName 端口已释放" -ForegroundColor Green
}

# 主逻辑
switch ($Command) {
    "build" {
        Write-Step "编译项目"
        Initialize-IdfEnv
        idf.py build
        if ($LASTEXITCODE -eq 0) {
            Write-Host "`n✅ 编译成功" -ForegroundColor Green
            Write-Host "💡 运行 .\dm.ps1 ota 发布固件到服务器" -ForegroundColor Cyan
        } else {
            Write-Error-Exit "编译失败"
        }
    }

    "ota" {
        Write-Step "发布固件到 OTA 服务器"
        if (-not (Test-Path "build\esp32.bin")) {
            Write-Error-Exit "找不到构建产物 build\esp32.bin，请先运行 .\dm.ps1 build"
        }
        Invoke-DevOtaVersion -Action publish
        Write-Host "`n✅ 固件已发布，设备将在下次定时检查时自动拉取更新" -ForegroundColor Green
    }

    "flash" {
        Write-Step "烧录固件到 $Port"
        Initialize-IdfEnv
        Stop-ComPort -PortName $Port
        idf.py -p $Port flash
        if ($LASTEXITCODE -eq 0) {
            Write-Host "`n✅ 烧录成功" -ForegroundColor Green
        } else {
            Write-Error-Exit "烧录失败"
        }
    }

    "flash-font" {
        $FontBin = Get-FontBinPath
        if (-not $FontBin) {
            Write-Error-Exit "字库文件不存在。`n  请先运行: python tools\fonts2bin\gen_font_bin.py"
        }
        $FontOffset = Get-FontPartitionOffset
        if (-not $FontOffset) {
            Write-Error-Exit "无法从 partitions.csv 解析 font 分区偏移"
        }
        Write-Step "烧录字库到 font 分区 ($FontOffset)"
        Initialize-IdfEnv
        Stop-ComPort -PortName $Port
        esptool.py --chip esp32s3 --port $Port --baud 921600 write_flash $FontOffset $FontBin
        if ($LASTEXITCODE -eq 0) {
            Write-Host "`n✅ 字库烧录成功" -ForegroundColor Green
        } else {
            Write-Error-Exit "字库烧录失败"
        }
    }

    "monitor" {
        Write-Step "监控串口 $Port"
        Initialize-IdfEnv
        Stop-ComPort -PortName $Port
        idf.py -p $Port monitor
    }

    "flash-monitor" {
        Write-Step "烧录并监控 $Port"
        Initialize-IdfEnv
        Stop-ComPort -PortName $Port
        idf.py -p $Port flash monitor
    }

    "kill-port" {
        Stop-ComPort -PortName $Port
        Write-Host "`n✅ COM 端口释放完成" -ForegroundColor Green
    }

    "clean" {
        Write-Step "清理构建目录"
        if (Test-Path "build") {
            Remove-Item -Recurse -Force "build"
            Write-Host "✅ build 目录已删除" -ForegroundColor Green
        } else {
            Write-Host "⚠️  build 目录不存在" -ForegroundColor Yellow
        }
    }

    "set-target-s3" {
        Write-Step "设置目标芯片为 ESP32-S3"
        Initialize-IdfEnv
        idf.py set-target esp32s3
        if ($LASTEXITCODE -eq 0) {
           Write-Host "`n✅ 目标已设置为 ESP32-S3" -ForegroundColor Green
            Write-Host "💡 请运行 .\dm.ps1 build 重新编译" -ForegroundColor Cyan
        } else {
            Write-Error-Exit "设置目标失败"
        }
    }

    "menuconfig" {
        Write-Step "打开菜单配置"
        Initialize-IdfEnv
        idf.py menuconfig
    }

    default {
        Write-Host "`nDeskMate 构建工具脚本`n" -ForegroundColor Cyan
        Write-Host "用法: .\dm.ps1 <命令> [-Port COMx]`n" -ForegroundColor White
        Write-Host "固件更新流程（默认走 OTA）:" -ForegroundColor Green
        Write-Host "  build           编译项目（生成开发版本号）"
        Write-Host "  ota             发布固件到 OTA 服务器（设备自动拉取更新）"
        Write-Host "`n串口命令（调试备用，非默认流程）:" -ForegroundColor Yellow
        Write-Host "  flash           烧录固件（USB）"
        Write-Host "  flash-font      烧录字库到 font 分区"
        Write-Host "  monitor         监控串口输出"
        Write-Host "  flash-monitor   烧录并监控"
        Write-Host "  kill-port       强制释放 COM 端口"
        Write-Host "`n其他命令:" -ForegroundColor Yellow
        Write-Host "  clean           清理构建目录"
        Write-Host "  set-target-s3   设置目标为 ESP32-S3"
        Write-Host "  menuconfig      打开菜单配置"
        Write-Host "`n示例:" -ForegroundColor Yellow
        Write-Host "  .\dm.ps1 build"
        Write-Host "  .\dm.ps1 ota"
        Write-Host "  .\dm.ps1 flash -Port COM5"
        Write-Host ""
    }
}
