$ErrorActionPreference = 'Stop'
# Local Windows speech only. Keep the previous leave.wav untouched.
$departureRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$departurePath = Join-Path $departureRoot 'sdcard\homehub\voices\leave_v2.wav'
if (Test-Path -LiteralPath $departurePath) {
    throw 'leave_v2.wav already exists; existing audio is preserved.'
}
Add-Type -AssemblyName System.Speech
$departureSpeaker = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $departureVoice = $departureSpeaker.GetInstalledVoices() |
        Where-Object {$_.Enabled -and $_.VoiceInfo.Culture.Name -eq 'zh-CN'} |
        Select-Object -First 1
    if (-not $departureVoice) { throw 'No enabled local zh-CN Windows speech voice.' }
    $departureSpeaker.SelectVoice($departureVoice.VoiceInfo.Name)
    $departureSpeaker.Rate = 0
    $departureSpeaker.Volume = 100
    $departureFormat = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
    $departureSpeaker.SetOutputToWaveFile($departurePath, $departureFormat)
    $departureSpeaker.Speak('一路顺风，等你回家哦')
    $departureSpeaker.SetOutputToNull()
    Write-Output ('LOCAL_MANDARIN_VOICE=' + $departureVoice.VoiceInfo.Name)
} finally {
    $departureSpeaker.Dispose()
}
& python (Join-Path $PSScriptRoot 'embed-departure-asset.py')
if ($LASTEXITCODE -ne 0) { throw 'Departure WAV validation or C embedding failed.' }
