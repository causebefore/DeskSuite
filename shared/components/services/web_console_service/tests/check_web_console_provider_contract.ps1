$ErrorActionPreference = 'Stop'

$componentRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$repoRoot = (Resolve-Path (Join-Path $componentRoot '..\..\..\..')).Path
$compiler = Get-Command clang.exe -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
    $compiler = Get-Command gcc.exe -ErrorAction SilentlyContinue
}
if ($null -eq $compiler) {
    throw '缺少 clang.exe 或 gcc.exe，无法运行 Web Console Provider host 回归'
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempDir = Join-Path $tempRoot ("web-console-provider-test-{0}" -f [guid]::NewGuid().ToString('N'))
$resolvedTempDir = [IO.Path]::GetFullPath($tempDir)
if (-not $resolvedTempDir.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Provider host-test 临时目录越出系统临时目录'
}

New-Item -ItemType Directory -Path $resolvedTempDir | Out-Null
try {
    $espErrHeader = Join-Path $resolvedTempDir 'esp_err.h'
    @'
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
'@ | Set-Content -LiteralPath $espErrHeader

    $publicInclude = Join-Path $componentRoot 'include'
    $privateInclude = Join-Path $componentRoot 'src\providers'
    $legacyTest = Join-Path $PSScriptRoot 'test_web_console_provider_legacy_compatibility.c'
    $validationTest = Join-Path $PSScriptRoot 'test_web_console_provider_validation.cpp'
    $validationExe = Join-Path $resolvedTempDir 'test_web_console_provider_validation.exe'

    & $compiler.Source '-std=c11' '-fsyntax-only' '-Wno-missing-field-initializers' `
        '-Werror=incompatible-pointer-types' '-Werror=int-conversion' `
        "-I$resolvedTempDir" "-I$publicInclude" $legacyTest
    if ($LASTEXITCODE -ne 0) {
        throw "旧 Provider 位置初始化器兼容检查失败（退出码 $LASTEXITCODE）"
    }

    & $compiler.Source '-std=c++17' '-Wall' '-Wextra' '-Werror' `
        "-I$resolvedTempDir" "-I$publicInclude" "-I$privateInclude" $validationTest '-o' $validationExe
    if ($LASTEXITCODE -ne 0) {
        throw "Provider UTF-8 host-test 编译失败（退出码 $LASTEXITCODE）"
    }
    & $validationExe
    if ($LASTEXITCODE -ne 0) {
        throw "Provider UTF-8 host-test 执行失败（退出码 $LASTEXITCODE）"
    }
}
finally {
    if (Test-Path -LiteralPath $resolvedTempDir) {
        Remove-Item -LiteralPath $resolvedTempDir -Recurse -Force
    }
}

Write-Host 'Web Console Provider 兼容与 UTF-8 契约检查通过。' -ForegroundColor Green
