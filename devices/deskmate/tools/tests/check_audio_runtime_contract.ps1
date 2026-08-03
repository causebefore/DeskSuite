$ErrorActionPreference = 'Stop'

$deviceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()
$cache = @{}

function Read-DeviceFile {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    if ($cache.ContainsKey($RelativePath)) {
        return $cache[$RelativePath]
    }
    $path = Join-Path $deviceRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $failures.Add("缺少待检查文件: $RelativePath")
        $cache[$RelativePath] = ''
        return ''
    }
    $cache[$RelativePath] = Get-Content -Raw -LiteralPath $path
    return $cache[$RelativePath]
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ((Read-DeviceFile $RelativePath) -notmatch $Pattern) {
        $failures.Add($Message)
    }
}

function Assert-NotContains {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ((Read-DeviceFile $RelativePath) -match $Pattern) {
        $failures.Add($Message)
    }
}

function Assert-FileMissing {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (Test-Path -LiteralPath (Join-Path $deviceRoot $RelativePath)) {
        $failures.Add($Message)
    }
}

function Assert-OnlyFilesContain {
    param(
        [Parameter(Mandatory = $true)][string[]]$SearchRoots,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [string[]]$AllowedRelativePaths = @(),
        [Parameter(Mandatory = $true)][string]$Message
    )

    foreach ($root in $SearchRoots) {
        $path = Join-Path $deviceRoot $root
        foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File) {
            if ($file.Extension -notin @('.c', '.cpp', '.h', '.hpp')) {
                continue
            }
            $relative = [System.IO.Path]::GetRelativePath($deviceRoot, $file.FullName)
            if ((Get-Content -Raw -LiteralPath $file.FullName) -match $Pattern -and
                $AllowedRelativePaths -notcontains $relative) {
                $failures.Add("${Message}: $relative")
            }
        }
    }
}

$audioHeader = 'components\services\audio_service\include\audio_service.h'
$audioSource = 'components\services\audio_service\src\audio_service.cpp'
$audioTask = 'components\services\audio_service\src\audio_service_playback_task.cpp'
$audioInternal = 'components\services\audio_service\src\audio_service_internal.hpp'
$processorHeader = 'components\services\audio_processor_service\include\audio_processor_service.h'
$processorSource = 'components\services\audio_processor_service\src\audio_processor_service.cpp'
$processorTask = 'components\services\audio_processor_service\src\audio_processor_service_task.cpp'
$processorInternal = 'components\services\audio_processor_service\src\audio_processor_service_internal.hpp'
$voiceHeader = 'components\services\voice_service\include\voice_service.h'
$voiceSource = 'components\services\voice_service\src\voice_service.cpp'
$voiceTask = 'components\services\voice_service\src\voice_service_task.cpp'
$voiceInternal = 'components\services\voice_service\src\voice_service_internal.hpp'
$pomodoroTask = 'main\application\app_pomodoro_task.c'
$powerTask = 'main\application\app_power_task.c'
$appVoice = 'main\application\app_voice.c'
$appVoiceHeader = 'main\application\app_voice.h'
$appMain = 'main\app_main.c'
$webMutation = '..\..\shared\components\services\web_console_service\src\files\web_console_service_mutation.cpp'
$webTransaction = '..\..\shared\components\services\web_console_service\src\files\web_console_service_transaction.cpp'
$webTransfer = '..\..\shared\components\services\web_console_service\src\files\web_console_service_transfer.cpp'

foreach ($header in @($audioHeader, $processorHeader, $voiceHeader)) {
    Assert-Contains $header '(?s)#ifdef\s+__cplusplus\s*extern\s+"C"' "$header 未保持公共 C ABI"
    Assert-NotContains $header '\bclass\s+\w+|std::' "$header 暴露了 C++ 私有类型"
}

foreach ($contract in @(
    'audio_service_playback_state_t',
    'audio_service_pcm_stream_config_t',
    'audio_service_file_playback_result_state_t',
    'audio_service_file_playback_result_t',
    'audio_service_status_t',
    'audio_service_init\s*\(',
    'audio_service_start\s*\(',
    'audio_service_stop\s*\(\s*uint32_t\s+timeout_ms',
    'audio_service_deinit\s*\(',
    'audio_service_get_status_copy\s*\(',
    'audio_service_request_play_mp3_file_copy\s*\(',
    'audio_service_request_cancel_file_playback\s*\(',
    'audio_service_open_pcm_stream\s*\(',
    'audio_service_write_pcm_stream_borrow\s*\(',
    'audio_service_close_pcm_stream\s*\('
)) {
    Assert-Contains $audioHeader $contract "Audio Service 公共契约缺少: $contract"
}

$oldAudioApi = 'audio_service_(enable_input|enable_output|read|write|get_volume|set_volume|get_mute|set_mute|is_running|is_initialized|get_sample_rate_hz)\b'
Assert-OnlyFilesContain @('components\services', 'main') $oldAudioApi @() '仍存在已删除的 Audio Service 转发 API'

Assert-Contains $audioInternal 'class\s+AudioServiceRuntime\s+final' '缺少私有 AudioServiceRuntime'
Assert-Contains $processorInternal 'class\s+AudioProcessorRuntime\s+final' '缺少私有 AudioProcessorRuntime'
Assert-Contains $voiceInternal 'class\s+VoiceServiceRuntime\s+final' '缺少私有 VoiceServiceRuntime'
Assert-FileMissing 'components\services\audio_service\src\audio_service.c' 'Audio Service 旧 C 实现仍存在'
Assert-FileMissing 'components\services\audio_processor_service\src\audio_processor_service.c' 'Processor 旧 C 实现仍存在'
Assert-FileMissing 'components\services\audio_processor_service\src\audio_processor_service_task.c' 'Processor Task 旧 C 实现仍存在'
Assert-FileMissing 'components\services\voice_service\src\voice_service.c' 'Voice 旧 C 实现仍存在'
Assert-FileMissing 'components\services\voice_service\src\voice_service_task.c' 'Voice Task 旧 C 实现仍存在'
Assert-NotContains $audioSource 'runtime->playback_state\s*=' 'Audio Service 门面仍在写播放状态'

Assert-OnlyFilesContain @('components\services', 'main') `
    'device_audio_(enable_output|set_output_volume|write)\b' `
    @($audioTask) `
    '发现 Audio Service 之外的输出硬件写入者'
Assert-OnlyFilesContain @('components\services', 'main') `
    'device_audio_(enable_input|read)\b' `
    @($processorSource) `
    '发现 Audio Processor 之外的输入硬件所有者'

Assert-Contains $processorHeader 'bool\s+input_active' 'Processor 状态未暴露真实 input_active'
Assert-Contains $processorSource 'device_audio_get_sample_rate_hz\s*\(' 'Processor 未读取 Device 硬件采样率'
Assert-Contains $processorSource 'APS_AFE_SAMPLE_RATE\s+16000' 'Processor 未固定 AFE 16 kHz 契约'
Assert-NotContains $processorSource 'audio_service_' 'Processor 仍依赖 Audio Service 转发输入'
Assert-Contains 'components\services\audio_processor_service\idf_component.yml' `
    '(?s)espressif/esp_audio_effects:\s*version:\s*"~1\.2\.1"' `
    'Processor 未保留直接 Audio Effects 依赖'

Assert-NotContains $voiceSource 'device_audio_|xStreamBuffer|voice_play|playback_task|volatile\s+playback' `
    'Voice 仍拥有输出硬件、播放缓冲或第二播放 Task'
Assert-NotContains $voiceTask 'voice_play|playback_task|StreamBuffer' 'Voice Task 文件仍包含播放 Task'
Assert-Contains $voiceSource 'audio_service_open_pcm_stream' 'Voice 未在首个 TTS PCM 帧打开统一流'
Assert-Contains $voiceSource 'audio_service_write_pcm_stream_borrow' 'Voice 未向统一 PCM 流提交样本'
Assert-Contains $voiceSource 'audio_service_close_pcm_stream' 'Voice 未通过统一 PCM 流收敛结束'
Assert-Contains $voiceSource `
    '(?s)uploaded_pcm_bytes\s*==\s*0U.*?!websocket_attempt\.received_response' `
    'WebSocket 回退未同时约束零上传和未收到响应'
Assert-Contains 'main\Kconfig.projbuild' '(?s)DESKMATE_VOICE_CHAT_DURATION_MS.*?range\s+2000\s+10000' `
    'Voice 会话时长未统一为 2000 到 10000 ms'

Assert-NotContains $appVoice 'audio_service_|processor_idle|input_active|output_active' `
    'app_voice 仍转发通用音频状态或管理 Audio Service'
Assert-NotContains $appVoiceHeader 'processor_idle|input_active|output_active' 'app_voice_status_t 仍暴露下层音频字段'
Assert-Contains $powerTask 'audio_service_get_status_copy' 'Power 未直接读取 Audio Service'
Assert-Contains $powerTask 'audio_processor_service_get_status_copy' 'Power 未直接读取 Processor'
Assert-Contains $powerTask 'voice_service_get_status_copy' 'Power 未直接读取 Voice Service'
Assert-Contains $powerTask 'APP_POWER_BLOCKER_AUDIO_PLAYBACK' 'Power 未建立独立播放阻塞条件'
Assert-Contains $powerTask 'AUDIO_SERVICE_PLAYBACK_STATE_IDLE' 'Power 未把非空闲播放事务作为阻塞条件'
Assert-Contains $appMain '(?s)device_audio_init.*?audio_service_init.*?audio_service_start.*?audio_processor_service_init.*?voice_service_init' `
    'Composition Root 未按 Device、Audio、Processor、Voice 顺序装配'

Assert-Contains 'components\services\audio_service\idf_component.yml' `
    '(?s)espressif/esp_audio_codec:\s*version:\s*"2\.5\.0"' `
    'Audio Service 未精确固定 esp_audio_codec 2.5.0'
Assert-Contains 'components\services\audio_service\idf_component.yml' `
    '(?s)espressif/esp_audio_effects:\s*version:\s*"~1\.2\.1"' `
    'Audio Service 未声明直接 Audio Effects 依赖'
Assert-Contains 'dependencies.lock' '(?s)espressif/esp_audio_codec:.*?version:\s*2\.5\.0' `
    '依赖锁未解析 esp_audio_codec 2.5.0'
Assert-Contains 'dependencies.lock' '(?s)espressif/esp_audio_effects:.*?version:\s*1\.2\.1' `
    '依赖锁未解析 esp_audio_effects 1.2.1'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_AUDIO_DECODER_MP3_SUPPORT=y' '默认配置未启用 MP3 Decoder'
Assert-NotContains 'sdkconfig.defaults' '(?m)^CONFIG_AUDIO_(DECODER_(?!MP3)|ENCODER_|SIMPLE_DEC_).*?=y$' `
    '默认配置仍启用了 MP3 之外的编解码器或容器'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_FATFS_FS_LOCK=5' 'FatFs 打开文件锁未与 max_files 对齐为 5'
Assert-Contains 'components\sys\system_filesystem.c' 'SYSTEM_FILESYSTEM_MAX_OPEN_FILES\s+5U' `
    '文件系统 max_files 未保持为 5'
Assert-Contains 'sdkconfig.defaults' 'CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES=262144' 'PCM 抗抖动缓冲未保持 262144 字节'
Assert-NotContains 'sdkconfig.defaults' 'DESKMATE_VOICE_PLAY_STREAM_BYTES|DESKMATE_VOICE_PLAYBACK_STOP_TIMEOUT_MS' `
    '仍存在旧 Voice 私有播放配置'

Assert-Contains $audioTask 'kEncodedBufferBytes\s*=\s*4096U' 'MP3 未使用 4 KiB 流式输入块'
Assert-Contains $audioTask 'kMaximumDecodedBytes\s*=\s*65536U' 'MP3 解码扩容缺少有界上限'
Assert-Contains $audioTask 'kPlaybackMaximumSeconds\s*=\s*30U' 'MP3 播放未限制为 30 秒'
Assert-Contains $audioTask 'kPlaybackTaskStackBytes\s*=\s*20U\s*\*\s*1024U' '播放 Task 未使用首轮 20 KiB 栈'
Assert-Contains $audioTask 'ESP_AUDIO_ERR_BUFF_NOT_ENOUGH' 'MP3 解码未处理输出扩容请求'
Assert-Contains $audioTask 'esp_ae_ch_cvt_process' 'MP3/PCM 输出未执行声道转换'
Assert-Contains $audioTask 'esp_ae_rate_cvt_process' 'MP3/PCM 输出未执行采样率转换'
Assert-Contains $audioTask 'AUDIO_SERVICE_EVENT_FILE_PLAYBACK_FINISHED' '文件请求未发布唯一终态事件'

Assert-Contains $pomodoroTask 'APP_POMODORO_COMPLETE_MP3_PATH\s+"/sdcard/pomodoro-complete\.mp3"' `
    'Pomodoro 固定提示音路径不符合契约'
Assert-Contains $pomodoroTask 'audio_service_request_play_mp3_file_copy\s*\(\s*APP_POMODORO_COMPLETE_MP3_PATH' `
    'Pomodoro 未以完成代次提交 MP3 请求'
Assert-Contains $pomodoroTask 'audio_service_request_cancel_file_playback' 'Pomodoro 未取消旧完成代次'
Assert-OnlyFilesContain @('main\application') `
    'audio_service_request_play_mp3_file_copy\b' `
    @($pomodoroTask) `
    'Pomodoro MP3 请求出现在 Task 所有者之外'

Assert-Contains $webMutation 'errno\s*==\s*EBUSY' '网页文件删除/移动未识别 EBUSY'
Assert-Contains $webTransaction 'rename_errno\s*==\s*EBUSY' '覆盖事务未识别目标正在使用'
Assert-Contains $webTransfer '"409 Conflict"[\s\S]{0,240}文件正在使用' `
    '网页文件服务未把文件占用映射为 409'

if ($failures.Count -gt 0) {
    Write-Host '音频 C++ Runtime、MP3 与所有权契约检查失败：' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host '音频 C++ Runtime、MP3 与所有权契约检查通过。' -ForegroundColor Green
