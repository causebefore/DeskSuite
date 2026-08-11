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
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
'@ | Set-Content -LiteralPath $espErrHeader

    @'
#pragma once
#define CONFIG_WEB_CONSOLE_FILES 1
#define CONFIG_WEB_CONSOLE_SETTINGS 1
#define CONFIG_WEB_CONSOLE_STATUS 1
#define CONFIG_WEB_CONSOLE_ACTIONS 0
'@ | Set-Content -LiteralPath (Join-Path $resolvedTempDir 'sdkconfig.h')

    @'
#pragma once
typedef void *httpd_handle_t;
typedef int httpd_method_t;
typedef struct httpd_req httpd_req_t;
'@ | Set-Content -LiteralPath (Join-Path $resolvedTempDir 'esp_http_server.h')

    $freertosDir = Join-Path $resolvedTempDir 'freertos'
    New-Item -ItemType Directory -Path $freertosDir | Out-Null
    @'
#pragma once
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef unsigned int TickType_t;
'@ | Set-Content -LiteralPath (Join-Path $freertosDir 'FreeRTOS.h')
    @'
#pragma once
#include "FreeRTOS.h"
'@ | Set-Content -LiteralPath (Join-Path $freertosDir 'semphr.h')
    @'
#pragma once
#include "FreeRTOS.h"
'@ | Set-Content -LiteralPath (Join-Path $freertosDir 'task.h')

    $publicInclude = Join-Path $componentRoot 'include'
    $privateInclude = Join-Path $componentRoot 'src\providers'
    $coreInclude = Join-Path $componentRoot 'src\core'
    $legacyTest = Join-Path $PSScriptRoot 'test_web_console_provider_legacy_compatibility.c'
    $validationTest = Join-Path $PSScriptRoot 'test_web_console_provider_validation.cpp'
    $validationExe = Join-Path $resolvedTempDir 'test_web_console_provider_validation.exe'
    $registryTest = Join-Path $PSScriptRoot 'test_web_console_provider_registry.cpp'
    $registrySource = Join-Path $componentRoot 'src\providers\web_console_provider_registry.cpp'
    $registryExe = Join-Path $resolvedTempDir 'test_web_console_provider_registry.exe'

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

    & $compiler.Source '-std=c++17' '-Wall' '-Wextra' '-Werror' `
        "-I$resolvedTempDir" "-I$publicInclude" "-I$privateInclude" "-I$coreInclude" `
        $registrySource $registryTest '-o' $registryExe
    if ($LASTEXITCODE -ne 0) {
        throw "Provider 注册表 host-test 编译失败（退出码 $LASTEXITCODE）"
    }
    & $registryExe
    if ($LASTEXITCODE -ne 0) {
        throw "Provider 注册表 host-test 执行失败（退出码 $LASTEXITCODE）"
    }
}
finally {
    if (Test-Path -LiteralPath $resolvedTempDir) {
        Remove-Item -LiteralPath $resolvedTempDir -Recurse -Force
    }
}

Write-Host 'Web Console Provider 兼容、UTF-8 与注册表契约检查通过。' -ForegroundColor Green
