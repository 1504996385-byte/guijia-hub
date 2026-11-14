param([string]$SdkRoot='D:\Tuya\TuyaOpen',[switch]$Check)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING='utf-8'
$env:TUYAOPEN_EXPORT_IDE='1'
$env:TUYAOPEN_CN_DOWNLOAD='1'
$env:UV_CACHE_DIR=Join-Path $SdkRoot '.tools\uv-cache'
$env:OPEN_COUNTRY_CODE='China'
$taskProject=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\firmware'))
$taskLogs=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\logs'))
New-Item -ItemType Directory -Path $taskLogs -Force | Out-Null
$taskLog=Join-Path $taskLogs ('firmware-build-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.log')
$taskNativeLog=$taskLog+'.tool.log'
Start-Transcript -Path $taskLog -Force | Out-Null
try {
    . (Join-Path $SdkRoot 'export.ps1')
    New-Item -ItemType File -Path (Join-Path $SdkRoot '.cache\.dont_prompt_update_platform') -Force | Out-Null
    if($Check){tos.py check;if($LASTEXITCODE -ne 0){throw 'SDK prerequisite check failed'}}
    Set-Location -LiteralPath $taskProject
    $taskConfiguration=Join-Path $taskProject 'config\WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config'
    $taskConfigurationHash=(Get-FileHash -LiteralPath $taskConfiguration -Algorithm SHA256).Hash
    $taskStamp=Join-Path $taskProject '.build\.homehub-config-sha'
    if(-not(Test-Path -LiteralPath $taskStamp) -or (Get-Content -LiteralPath $taskStamp -Raw).Trim() -ne $taskConfigurationHash){
        & $env:OPEN_SDK_PYTHON (Join-Path $SdkRoot 'tos.py') config choice -c WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config 2>&1 | Tee-Object -FilePath $taskNativeLog -Append
        if($LASTEXITCODE -ne 0){throw 'Board configuration failed'}
    }
    & $env:OPEN_SDK_PYTHON (Join-Path $SdkRoot 'tos.py') build 2>&1 | Tee-Object -FilePath $taskNativeLog -Append
    if($LASTEXITCODE -ne 0){throw 'Firmware cross-compilation failed'}
    Set-Content -LiteralPath $taskStamp -Value $taskConfigurationHash -Encoding ascii
    $taskBin=Get-ChildItem -LiteralPath (Join-Path $taskProject 'dist') -Recurse -Filter '*_QIO_*.bin' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if(-not $taskBin){throw 'Build reported success but no QIO firmware was produced'}
    Write-Output ('FIRMWARE_BIN='+$taskBin.FullName)
    Get-FileHash -LiteralPath $taskBin.FullName -Algorithm SHA256 | Format-List
} finally {
    Stop-Transcript | Out-Null
    Write-Output ('BUILD_LOG='+$taskLog)
    Write-Output ('NATIVE_BUILD_LOG='+$taskNativeLog)
}
