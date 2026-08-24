$ErrorActionPreference = 'Stop'

$deviceRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')

function Assert-SourceContains {
    param([string]$RelativePath, [string]$Pattern)
    $path = Join-Path $deviceRoot $RelativePath
    $content = Get-Content -Raw -Path $path
    if ($content -notmatch $Pattern) {
        throw "契约缺失: $RelativePath / $Pattern"
    }
}

Assert-SourceContains 'components\boards\reTerminal_E1001\board.h' 'BOARD_BUTTON_RIGHT_GPIO\s+\(GPIO_NUM_4\)'
Assert-SourceContains 'components\application\provisioning_app\src\provisioning_app.c' '"NO NETWORK"'
Assert-SourceContains 'components\application\provisioning_app\src\provisioning_app.c' '"NO SERVER"'
Assert-SourceContains 'components\application\provisioning_app\src\provisioning_app.c' 'HOLD MIDDLE 3S TO SETUP'
Assert-SourceContains 'components\application\provisioning_app\src\provisioning_app.c' 'network_manager_request_start_portal\(\)'
Assert-SourceContains 'components\application\photo_playback_app\src\photo_playback_app_task.cpp' 'PHOTO_PLAYBACK_PROVISIONING_HOLD_US'
Assert-SourceContains 'components\application\photo_playback_app\src\photo_playback_app_task.cpp' 'modal_allows_provisioning'
Assert-SourceContains 'components\application\content_refresh_app\src\content_refresh_app_task.cpp' 'CONTENT_REFRESH_APP_RESULT_LOCAL_FAILURE'
Assert-SourceContains 'components\application\power_management_app\src\power_management_app_task.cpp' 'status\.last_error != ESP_OK && status\.last_error != ESP_ERR_NOT_FOUND'
Assert-SourceContains 'components\application\power_management_app\src\power_management_app_task.cpp' 'timer_wakeup_boot'

$qrCalls = @(rg -n 'connect_render_portal_qr_borrow' $deviceRoot -g '!build/**' |
    Where-Object { $_ -notmatch '[\\/]tests[\\/]' })
if ($qrCalls.Count -ne 1) {
    throw "二维码渲染入口应保持唯一，实际命中: $($qrCalls.Count)"
}

Write-Host 'PhotoPainter 配网静态契约检查通过'
