# Run in an Administrator PowerShell on the Windows PC hosting the server.
$ErrorActionPreference = 'Stop'
$taskRuleName = 'StopWatch-OTP-LAN-8765'
if (-not (Get-NetFirewallRule -Name $taskRuleName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -Name $taskRuleName -DisplayName 'StopWatch OTP - local network' `
        -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8765 `
        -RemoteAddress LocalSubnet -Profile Any | Out-Null
}
Write-Output 'TCP 8765 allowed from the local subnet. Remove with: Remove-NetFirewallRule -Name StopWatch-OTP-LAN-8765'
