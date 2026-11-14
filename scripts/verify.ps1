param([switch]$BuildFirmware)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$env:PYTHONIOENCODING = 'utf-8'
Push-Location -LiteralPath $taskRoot
try {
    python tests/core/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Core behavior verification failed' }
    python tests/platform/run_storage.py
    if ($LASTEXITCODE -ne 0) { throw 'Storage fault injection failed' }
    python tests/platform/run_config.py
    if ($LASTEXITCODE -ne 0) { throw 'Assembled sensor configuration verification failed' }
    python tests/platform/run_audio.py
    if ($LASTEXITCODE -ne 0) { throw 'Audio control-flow fault injection failed' }
    python tests/platform/run_tickets.py
    if ($LASTEXITCODE -ne 0) { throw 'Durable played-receipt verification failed' }
    python tests/platform/run_seed.py
    if ($LASTEXITCODE -ne 0) { throw 'First-boot test voice verification failed' }
    python tests/platform/run_seed_card.py
    if ($LASTEXITCODE -ne 0) { throw 'Read-only TF voice validation failed' }
    python tests/reminder_install/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Requested reminder migration verification failed' }
    python tests/event_memory/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Event queue payload memory verification failed' }
    python tests/event_queue/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'PSRAM event queue ownership verification failed' }
    python tests/voice_wake/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Voice wake lifecycle verification failed' }
    python tests/cloud_http/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Bounded cloud HTTPS verification failed' }
    python tests/supabase/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Supabase delivery verification failed' }
    python tests/tuya_light/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Tuya key-light control verification failed' }
    python tests/chat_cloud/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Chat cloud adapter verification failed' }
    python tests/ui_face/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'Face lifecycle and geometry verification failed' }
    python tests/ui_render/run_render.py
    if ($LASTEXITCODE -ne 0) { throw 'Actual LVGL face rendering verification failed' }
    python tests/json_memory/run_tests.py
    if ($LASTEXITCODE -ne 0) { throw 'JSON allocation routing verification failed' }
    python -m unittest discover -s tests -p test_release_tools.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Release protection verification failed' }
    python -m unittest discover -s tests -p test_provision_tuya.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Device authorization import verification failed' }
    python -m unittest discover -s server/tests -v
    if ($LASTEXITCODE -ne 0) { throw 'HTTP verification failed' }
    node --test server/tests/ui-state.test.js
    if ($LASTEXITCODE -ne 0) { throw 'Phone UI state verification failed' }
    node --check server/static/app.js
    if ($LASTEXITCODE -ne 0) { throw 'Phone JavaScript syntax verification failed' }
    python -m py_compile server/app.py server/import_voice.py scripts/release_tools.py scripts/provision_tuya.py
    if ($LASTEXITCODE -ne 0) { throw 'Python syntax verification failed' }
    if ($BuildFirmware) { & (Join-Path $PSScriptRoot 'build-firmware.ps1') }
    Write-Output 'Software checks passed. Physical GPIO, microphone, speaker, SD power-loss and boot still require the board.'
} finally { Pop-Location }
