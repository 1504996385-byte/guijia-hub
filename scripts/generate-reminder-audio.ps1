$ErrorActionPreference = 'Stop'
# Local Windows speech only. Keep the previous take_key.wav untouched.
$reminderRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$reminderPath = Join-Path $reminderRoot 'sdcard\homehub\voices\take_key_v2.wav'
if (Test-Path -LiteralPath $reminderPath) {
    throw 'take_key_v2.wav already exists; existing audio is preserved.'
}
Add-Type -AssemblyName System.Speech
$reminderSpeaker = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $reminderVoice = $reminderSpeaker.GetInstalledVoices() |
        Where-Object {$_.Enabled -and $_.VoiceInfo.Culture.Name -eq 'zh-CN'} |
        Select-Object -First 1
    if (-not $reminderVoice) { throw 'No enabled local zh-CN Windows speech voice.' }
    $reminderSpeaker.SelectVoice($reminderVoice.VoiceInfo.Name)
    $reminderSpeaker.Rate = 0
    $reminderSpeaker.Volume = 100
    $reminderFormat = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
    $reminderSpeaker.SetOutputToWaveFile($reminderPath, $reminderFormat)
    $reminderSpeaker.Speak('主人，出门请带好钥匙，关好水电燃气门窗。')
    $reminderSpeaker.SetOutputToNull()
    Write-Output ('LOCAL_MANDARIN_VOICE=' + $reminderVoice.VoiceInfo.Name)
} finally {
    $reminderSpeaker.Dispose()
}
& python (Join-Path $PSScriptRoot 'embed-reminder-asset.py')
if ($LASTEXITCODE -ne 0) { throw 'Reminder WAV validation or C embedding failed.' }
