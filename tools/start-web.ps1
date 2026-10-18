param([switch]$Lan)
$ErrorActionPreference = 'Stop'
$taskServerArgs = @()
if ($Lan) { $taskServerArgs = @('--host', '0.0.0.0') }
$taskCandidates = @()
if ($env:IDF_PYTHON_ENV_PATH) { $taskCandidates += (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe') }
$taskEnvRoot = Join-Path $env:USERPROFILE '.espressif\python_env'
if (Test-Path -LiteralPath $taskEnvRoot) {
    $taskCandidates += Get-ChildItem -LiteralPath $taskEnvRoot -Directory |
        Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' }
}
foreach ($taskPython in $taskCandidates) {
    if (Test-Path -LiteralPath $taskPython) {
        & $taskPython (Join-Path $PSScriptRoot 'server.py') @taskServerArgs
        exit $LASTEXITCODE
    }
}
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 (Join-Path $PSScriptRoot 'server.py') @taskServerArgs
    exit $LASTEXITCODE
}
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python (Join-Path $PSScriptRoot 'server.py') @taskServerArgs
    exit $LASTEXITCODE
}
throw 'Python 3 was not found. Install ESP-IDF 5.5.4 or Python 3.'
