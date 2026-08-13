param(
    [string] $HostName = '',
    [string] $Port = '',
    [string] $LogLevel = '',
    [switch] $SkipPortCleanup
)

# hyper_rlcd server launcher (called by start_server.bat)
$ErrorActionPreference = 'Continue'
Set-Location -LiteralPath $PSScriptRoot

function Get-TomlScalar {
    param(
        [string] $Section,
        [string] $Key
    )

    $configPath = Join-Path $PSScriptRoot "config.toml"
    if (-not (Test-Path -LiteralPath $configPath)) {
        throw "config.toml not found: $configPath"
    }

    $currentSection = ""
    foreach ($line in Get-Content -LiteralPath $configPath -Encoding UTF8) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith("#")) {
            continue
        }

        if ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]")) {
            $currentSection = $trimmed.Substring(1, $trimmed.Length - 2).Trim()
            continue
        }

        if ($currentSection -ne $Section) {
            continue
        }

        $separator = $trimmed.IndexOf("=")
        if ($separator -lt 0) {
            continue
        }

        $name = $trimmed.Substring(0, $separator).Trim()
        if ($name -eq $Key) {
            $value = $trimmed.Substring($separator + 1).Trim()
            return $value.Trim('"').Trim("'")
        }
    }
    throw "config.toml missing [$Section].$Key"
}

function Stop-PortListeners {
    param(
        [int] $LocalPort
    )

    $listeners = Get-NetTCPConnection -LocalPort $LocalPort -State Listen -ErrorAction SilentlyContinue
    if (-not $listeners) {
        return
    }

    $processIds = $listeners | Select-Object -ExpandProperty OwningProcess -Unique
    foreach ($processId in $processIds) {
        if ($processId -eq $PID) {
            continue
        }

        $processSnapshot = @(Get-CimInstance Win32_Process)
        $processById = @{}
        foreach ($item in $processSnapshot) {
            $processById[[int] $item.ProcessId] = $item
        }

        # Uvicorn --reload 会让子进程继承监听 socket。只结束 netstat 报告的 PID
        # 会留下仍可提供请求的孤儿 worker，因此要从 uv/uvicorn 根进程开始清理整棵树。
        $treeRootId = [int] $processId
        $cursorId = [int] $processId
        while ($processById.ContainsKey($cursorId)) {
            $cursor = $processById[$cursorId]
            $commandLine = [string] $cursor.CommandLine
            if (
                $commandLine -match '(?i)(^|\s)uv(?:\.exe)?\s+run(?:\s|$)' -or
                $commandLine -match '(?i)-m\s+uvicorn(?:\s|$)'
            ) {
                $treeRootId = $cursorId
            }
            $parentId = [int] $cursor.ParentProcessId
            if ($parentId -le 0 -or -not $processById.ContainsKey($parentId)) {
                break
            }
            $cursorId = $parentId
        }

        $treeIds = [System.Collections.Generic.List[int]]::new()
        $treeIds.Add($treeRootId)
        for ($index = 0; $index -lt $treeIds.Count; $index++) {
            $parentId = $treeIds[$index]
            foreach ($child in $processSnapshot) {
                $childId = [int] $child.ProcessId
                if (
                    [int] $child.ParentProcessId -eq $parentId -and
                    -not $treeIds.Contains($childId)
                ) {
                    $treeIds.Add($childId)
                }
            }
        }

        $targetIds = @($treeIds)
        [array]::Reverse($targetIds)
        Write-Host "  stopping previous listener tree on port ${LocalPort}: root=$treeRootId pids=$($targetIds -join ',')" -ForegroundColor Yellow
        foreach ($targetId in $targetIds) {
            if ($targetId -ne $PID) {
                Stop-Process -Id $targetId -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $deadline = (Get-Date).AddSeconds(5)
    do {
        $remaining = Get-NetTCPConnection -LocalPort $LocalPort -State Listen -ErrorAction SilentlyContinue
        if (-not $remaining) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)

    if ($remaining) {
        $remainingPids = ($remaining | Select-Object -ExpandProperty OwningProcess -Unique) -join ', '
        throw "port $LocalPort is still occupied by pid(s): $remainingPids"
    }
}

if (-not $HostName) {
    $HostName = Get-TomlScalar -Section 'server' -Key 'host'
}
if (-not $Port) {
    $Port = Get-TomlScalar -Section 'server' -Key 'port'
}
if (-not $LogLevel) {
    $LogLevel = Get-TomlScalar -Section 'server' -Key 'log_level'
}
$PortNumber = [int] $Port

Write-Host ""
Write-Host "  hyper_rlcd server" -ForegroundColor Cyan
Write-Host "  --------------------------------------" -ForegroundColor Cyan
Write-Host "  listen ${HostName}:${Port}  (SERVER_URL in device menuconfig = this PC LAN IP)" -ForegroundColor Cyan
Write-Host "  stop: Ctrl+C or close window" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path ".env")) {
    Write-Host "  [warn] .env missing. Run:  copy .env.example .env" -ForegroundColor Yellow
    Write-Host "         then fill weather key etc. before starting." -ForegroundColor Yellow
    Write-Host ""
}

if (-not $SkipPortCleanup) {
    Stop-PortListeners -LocalPort $PortNumber
}

Write-Host "  starting uvicorn (first run auto-syncs deps, may be slow)..." -ForegroundColor Green
Write-Host ""

$uvCommand = Get-Command uv -ErrorAction Stop
# 仅监听源目录，避免 rendered_frames 和 runtime_logs 的运行期写入产生 watchfiles 噪声。
$appReloadDir = Join-Path $PSScriptRoot "app"
$webReloadDir = Join-Path $PSScriptRoot "web"
if (-not (Test-Path -LiteralPath $appReloadDir -PathType Container)) {
    throw "app reload directory not found: $appReloadDir"
}
if (-not (Test-Path -LiteralPath $webReloadDir -PathType Container)) {
    throw "web reload directory not found: $webReloadDir"
}

# 显式数组可确保 Windows PowerShell 5.1 把每个路径作为独立参数传给 uv/uvicorn。
$uvicornArguments = @(
    'run'
    '--project'
    $PSScriptRoot
    'python'
    '-m'
    'uvicorn'
    'app.main:app'
    '--reload'
    '--reload-dir'
    $appReloadDir
    '--reload-dir'
    $webReloadDir
    '--ws'
    'websockets-sansio'
    '--host'
    $HostName
    '--port'
    [string] $PortNumber
    '--log-level'
    $LogLevel
)
& $uvCommand.Source @uvicornArguments

Write-Host ""
Write-Host "  server exited (error or manually stopped)." -ForegroundColor Yellow
