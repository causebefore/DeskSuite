$ErrorActionPreference = 'Stop'

$deviceRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$testBinary = Join-Path $env:TEMP 'photopainter_duration_policy_test.exe'
$includeDir = Join-Path $deviceRoot 'components\utils\include'
$stubDir = Join-Path $PSScriptRoot 'stubs'
$source = Join-Path $PSScriptRoot 'test_duration_policy.c'

& clang -std=c11 -Wall -Wextra -Werror "-I$stubDir" "-I$includeDir" $source -o $testBinary
if ($LASTEXITCODE -ne 0) {
    throw '编译持续时间边界测试失败'
}
& $testBinary
if ($LASTEXITCODE -ne 0) {
    throw '持续时间边界测试失败'
}
Remove-Item -LiteralPath $testBinary -ErrorAction SilentlyContinue

& (Join-Path $deviceRoot 'tests\static\check_provisioning_contract.ps1')
