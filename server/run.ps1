param([int]$Port = 8765)
$ErrorActionPreference = 'Stop'
$pythonCommand = Get-Command python -ErrorAction Stop
& $pythonCommand.Source (Join-Path $PSScriptRoot 'app.py') --host 127.0.0.1 --port $Port
exit $LASTEXITCODE
