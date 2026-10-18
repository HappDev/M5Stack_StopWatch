param([string]$Port, [switch]$Flash, [switch]$Update)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (!$env:IDF_PATH) { throw 'Open ESP-IDF 5.5.4 PowerShell first (IDF_PATH is missing).' }
$taskPython = if ($env:IDF_PYTHON_ENV_PATH) { Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe' } else { 'python' }
$taskArgs = @((Join-Path $PSScriptRoot 'build.py'))
if ($Port) { $taskArgs += @('--port', $Port) }
if ($Flash) { $taskArgs += '--flash' }
if ($Update) { $taskArgs += '--update' }
& $taskPython @taskArgs
exit $LASTEXITCODE
