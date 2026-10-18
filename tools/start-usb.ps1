$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
$taskCliArgs = @($args)
if ($taskCliArgs.Count -eq 0) { $taskCliArgs = @('--help') }
$taskCandidates = @()
if ($env:IDF_PYTHON_ENV_PATH) { $taskCandidates += (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe') }
$taskEnvRoot = Join-Path $env:USERPROFILE '.espressif\python_env'
if (Test-Path -LiteralPath $taskEnvRoot) {
    $taskCandidates += Get-ChildItem -LiteralPath $taskEnvRoot -Directory |
        Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' }
}
foreach ($taskPython in $taskCandidates) {
    if (Test-Path -LiteralPath $taskPython) {
        & $taskPython (Join-Path $PSScriptRoot 'otp_cli.py') @taskCliArgs
        exit $LASTEXITCODE
    }
}
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 (Join-Path $PSScriptRoot 'otp_cli.py') @taskCliArgs
    exit $LASTEXITCODE
}
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python (Join-Path $PSScriptRoot 'otp_cli.py') @taskCliArgs
    exit $LASTEXITCODE
}
throw 'Python 3.9+ was not found. Install Python or ESP-IDF.'
