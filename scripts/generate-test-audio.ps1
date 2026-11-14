param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\sdcard\homehub\voices'))
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
if ((Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.wav' -File | Select-Object -First 1) -or
    (Test-Path -LiteralPath (Join-Path $OutputDirectory 'manifest.json'))) {
    throw 'Output directory already contains audio or a manifest. Choose a new empty directory; existing recordings are preserved.'
}
Add-Type -AssemblyName System.Speech
$speaker = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voice = $speaker.GetInstalledVoices() | Where-Object {$_.Enabled -and $_.VoiceInfo.Culture.Name -eq 'zh-CN'} | Select-Object -First 1
if (-not $voice) { throw 'No installed zh-CN Windows speech voice. Import human recordings instead.' }
$speaker.SelectVoice($voice.VoiceInfo.Name)
$speaker.Rate = 0
$speaker.Volume = 90
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$phrases = [ordered]@{
    'arrive_01.wav'='欢迎回家，先歇一歇。'
    'arrive_02.wav'='回来啦，今天辛苦了。'
    'arrive_03.wav'='钥匙放好了，欢迎回家。'
    'arrive_04.wav'='家里有你，真好。'
    'arrive_05.wav'='喝口水，慢慢来。'
    'arrive_06.wav'='忙了一天，放松一下吧。'
    'leave_01.wav'='出门记得带好钥匙。'
    'leave_02.wav'='一路平安，回家见。'
    'leave_03.wav'='记得带上手机和钥匙。'
    'leave_04.wav'='出门慢一点，不着急。'
    'leave_05.wav'='祝你今天顺顺利利。'
    'leave_06.wav'='记得按时吃饭，照顾好自己。'
    'take_key.wav'='出门记得拿钥匙。'
}
$records = @()
try {
    foreach ($name in $phrases.Keys) {
        $file = Join-Path $OutputDirectory $name
        $speaker.SetOutputToWaveFile($file, $format)
        $speaker.Speak($phrases[$name])
        $speaker.SetOutputToNull()
        $records += [ordered]@{file=$name;text=$phrases[$name];source='Windows system speech, synthetic Mandarin test audio';voice=$voice.VoiceInfo.Name}
    }
} finally { $speaker.Dispose() }
Copy-Item -LiteralPath (Join-Path $OutputDirectory 'arrive_01.wav') -Destination (Join-Path $OutputDirectory 'arrive.wav') -Force
Copy-Item -LiteralPath (Join-Path $OutputDirectory 'leave_01.wav') -Destination (Join-Path $OutputDirectory 'leave.wav') -Force
$manifest = [ordered]@{sample_rate=16000;bits=16;channels=1;is_human_recording=$false;is_dialect=$false;source='Windows speech synthesis';phrases=$records}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Output "Generated $($records.Count) synthetic Mandarin test prompts and 2 default aliases in $OutputDirectory"
