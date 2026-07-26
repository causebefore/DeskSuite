param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("publish")]
    [string] $Action
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $ProjectRoot "build"
$ServerRoot = Resolve-Path -LiteralPath (Join-Path $ProjectRoot "..\..\services\hub")
$FirmwareDir = Join-Path $ServerRoot "firmwares"
$ManifestFile = Join-Path $FirmwareDir "manifest.json"

function Get-GeneratedVersion {
    # 在 build 目录下搜索版本头文件，避免硬编码 ESP-IDF 组件目录结构
    $found = Get-ChildItem -Path $BuildDir -Recurse -Filter "deskmate_version.h" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $found) {
        throw "找不到版本头文件，请先运行 build: deskmate_version.h"
    }
    $GeneratedVersionFile = $found.FullName
    if (-not (Test-Path -LiteralPath $GeneratedVersionFile)) {
        throw "找不到版本文件，请先运行 build: $GeneratedVersionFile"
    }

    $content = Get-Content -LiteralPath $GeneratedVersionFile -Raw
    if ($content -notmatch 'DESKMATE_BUILD_VERSION\s+"([^"]+)"') {
        throw "版本文件格式无效: $GeneratedVersionFile"
    }
    return $Matches[1]
}

function Publish-Firmware {
    $version = Get-GeneratedVersion
    $sourceBin = Join-Path $BuildDir "esp32.bin"
    if (-not (Test-Path -LiteralPath $sourceBin)) {
        throw "找不到构建产物: $sourceBin"
    }

    New-Item -ItemType Directory -Force -Path $FirmwareDir | Out-Null
    $fileName = "esp32-$version.bin"
    $targetBin = Join-Path $FirmwareDir $fileName
    Copy-Item -LiteralPath $sourceBin -Destination $targetBin -Force

    $size = (Get-Item -LiteralPath $targetBin).Length
    $manifest = [ordered]@{
        firmware = [ordered]@{
            version = $version
            filename = $fileName
            size = $size
        }
        font = $null
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 4
    Set-Content -LiteralPath $ManifestFile -Value $manifestJson

    Write-Host "  OTA firmware published: $fileName ($size bytes)" -ForegroundColor Green
    Write-Host "  OTA manifest updated: $ManifestFile" -ForegroundColor Green
}

switch ($Action) {
    "publish" { Publish-Firmware }
}
