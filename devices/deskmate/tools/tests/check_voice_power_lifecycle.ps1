$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Get-RepoContent {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("缺少待检查文件: $RelativePath")
        return ''
    }
    return Get-Content -Raw -LiteralPath $path
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    if ((Get-RepoContent $RelativePath) -notmatch $Pattern) {
        $failures.Add("缺少语音生命周期能力: $RelativePath / $Pattern")
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    if ((Get-RepoContent $RelativePath) -match $Pattern) {
        $failures.Add("仍存在禁止的语音生命周期实现: $RelativePath / $Pattern")
    }
}

function Assert-InOrder {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string[]]$Tokens
    )

    $content = Get-RepoContent $RelativePath
    $cursor = -1
    foreach ($token in $Tokens) {
        $next = $content.IndexOf($token, $cursor + 1, [System.StringComparison]::Ordinal)
        if ($next -lt 0) {
            $failures.Add("缺少有序调用: $RelativePath / $token")
            return
        }
        $cursor = $next
    }
}

function Assert-SectionNotContains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$StartToken,
        [Parameter(Mandatory = $true)][string]$EndToken,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $content = Get-RepoContent $RelativePath
    $start = $content.IndexOf($StartToken, [System.StringComparison]::Ordinal)
    $end = if ($start -ge 0) {
        $content.IndexOf($EndToken, $start + $StartToken.Length, [System.StringComparison]::Ordinal)
    } else {
        -1
    }
    if ($start -lt 0 -or $end -lt 0) {
        $failures.Add("无法定位待检查代码段: $RelativePath / $StartToken")
        return
    }
    if ($content.Substring($start, $end - $start) -match $Pattern) {
        $failures.Add("代码段包含禁止调用: $RelativePath / $StartToken / $Pattern")
    }
}

$audioHeader = 'components\services\audio_service\include\audio_service.h'
$audioSource = 'components\services\audio_service\src\audio_service.c'
$processorHeader = 'components\services\audio_processor_service\include\audio_processor_service.h'
$processorSource = 'components\services\audio_processor_service\src\audio_processor_service.c'
$processorTask = 'components\services\audio_processor_service\src\audio_processor_service_task.c'
$voiceHeader = 'components\services\voice_service\include\voice_service.h'
$voiceSource = 'components\services\voice_service\src\voice_service.c'
$voiceTask = 'components\services\voice_service\src\voice_service_task.c'
$appVoiceHeader = 'main\application\app_voice.h'
$appVoiceSource = 'main\application\app_voice.c'
$appMain = 'main\app_main.c'
$powerHeader = 'main\application\app_power.h'
$powerTask = 'main\application\app_power_task.c'

Assert-Contains $audioHeader 'audio_service_state_t'
Assert-Contains $audioHeader 'audio_service_start\s*\('
Assert-Contains $audioHeader 'audio_service_get_status_copy\s*\('
Assert-Contains $audioSource 's_ctx\.state\s*=\s*AUDIO_SERVICE_STATE_STOPPED'
Assert-Contains $audioSource 'AUDIO_SERVICE_STATE_CLEANUP_FAILED'

Assert-Contains $processorHeader 'audio_processor_service_state_t'
Assert-Contains $processorHeader 'audio_processor_capture_state_t'
Assert-Contains $processorHeader 'audio_processor_service_start\s*\('
Assert-Contains $processorHeader 'audio_processor_service_stop\s*\('
Assert-Contains $processorHeader 'audio_processor_service_get_status_copy\s*\('
Assert-Contains $processorSource 's_ctx\.state\s*=\s*AUDIO_PROCESSOR_STATE_STOPPED'
Assert-Contains $processorTask 'xEventGroupWaitBits'
Assert-Contains $processorTask 'portMAX_DELAY'
Assert-Contains $processorTask 'APS_TASK_FEED_PARKED'
Assert-Contains $processorTask 'APS_TASK_FETCH_PARKED'
Assert-Contains $processorTask 'APS_TASK_DRAIN_DONE'
Assert-NotContains $processorTask 'vTaskDelay\s*\(\s*pdMS_TO_TICKS\s*\(\s*10'
Assert-NotContains $processorSource 'static\s+void\s+audio_processor_(feed|fetch)_task'
Assert-NotContains $processorSource 'vTaskDelete\s*\('

Assert-Contains $voiceHeader 'voice_service_state_t'
Assert-Contains $voiceHeader 'voice_service_start\s*\('
Assert-Contains $voiceHeader 'voice_service_stop\s*\('
Assert-Contains $voiceHeader 'voice_service_get_status_copy\s*\('
Assert-Contains $voiceSource 's_ctx\.state\s*=\s*VOICE_SERVICE_STATE_STOPPED'
Assert-Contains $voiceTask 'static\s+void\s+voice_service_chat_task'
Assert-Contains $voiceTask 'static\s+void\s+voice_service_playback_task'
Assert-NotContains $voiceSource 'static\s+void\s+voice_(chat|playback)_task'
Assert-NotContains $voiceSource 'xTaskCreate(WithCaps)?\s*\('
Assert-NotContains $voiceSource 'vTaskDelete(WithCaps)?\s*\('

Assert-Contains $appVoiceHeader 'app_voice_state_t'
Assert-Contains $appVoiceHeader 'app_voice_start\s*\('
Assert-Contains $appVoiceHeader 'app_voice_stop\s*\('
Assert-Contains $appVoiceHeader 'app_voice_deinit\s*\('
Assert-Contains $appVoiceHeader 'app_voice_get_status_copy\s*\('
Assert-Contains $appVoiceSource 'APP_VOICE_STATE_STOPPED'
Assert-Contains $appVoiceSource 'APP_VOICE_STATE_FAILED'
Assert-Contains $appVoiceSource 'APP_VOICE_STATE_RUNNING'
Assert-Contains $appVoiceSource 'app_page_get_current\(\)\s*!=\s*PRESENTATION_PAGE_VOICE'
Assert-Contains $appVoiceSource 'DEVICE_BUTTON_EVENT_RIGHT_LONG'
Assert-InOrder $appVoiceSource @(
    'audio_service_start()',
    'audio_processor_service_start()',
    'voice_service_start()'
)
Assert-InOrder $appMain @(
    'ui_runtime_start(APP_UI_START_TIMEOUT_MS)',
    'app_page_dispatch_initial_presentation()',
    'rtc_service_start()',
    'app_pomodoro_start()',
    'init_audio_runtime()',
    'app_voice_start(APP_VOICE_LIFECYCLE_TIMEOUT_MS)',
    'app_power_start()',
    'button_service_start()'
)

Assert-Contains $powerHeader 'APP_POWER_STEP_VOICE_STOP'
Assert-Contains $powerHeader 'APP_POWER_STEP_VOICE_START'
Assert-Contains $powerHeader 'APP_POWER_BLOCKER_AUDIO_PROCESSOR'
Assert-Contains $powerTask 'app_voice_get_status_copy'
Assert-Contains $powerTask 'voice\.session_busy'
Assert-Contains $powerTask 'voice\.processor_idle'
Assert-Contains $powerTask 'voice\.input_active'
Assert-Contains $powerTask 'voice\.output_active'
Assert-Contains $powerTask 'voice\.network_lease_held'
Assert-InOrder $powerTask @(
    'stop_voice_for_sleep(expected_generation)',
    'stop_ui_for_sleep(expected_generation)',
    'stop_network_for_power_save(expected_generation)'
)
Assert-SectionNotContains $powerTask `
    'static esp_err_t run_offline_display_session' `
    'static esp_err_t run_sleep_session' `
    'stop_voice_for_sleep|stop_ui_for_sleep|device_power_enter_light_sleep'
Assert-InOrder $powerTask @(
    'resume_network_runtime()',
    'resume_voice_runtime()',
    'resume_ui_runtime()',
    'button_service_request_light_sleep_wakeup_copy'
)
Assert-SectionNotContains `
    $powerTask `
    'static esp_err_t run_network_maintenance' `
    'static esp_err_t run_sleep_session' `
    'app_voice_start|resume_voice_runtime'

if ($failures.Count -gt 0) {
    Write-Host '语音生命周期与低功耗契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host '语音生命周期与低功耗契约检查通过。' -ForegroundColor Green
