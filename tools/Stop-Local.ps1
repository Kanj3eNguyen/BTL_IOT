$ErrorActionPreference = 'Stop'
function Invoke-LocalWsl {
    $ErrorActionPreference = 'Continue'
    & wsl @args
}
$projectRoot = Split-Path $PSScriptRoot -Parent
$pidFile = Join-Path $projectRoot '.private\runtime\gateway.pid'
if (Test-Path $pidFile) {
    $gatewayId = [int](Get-Content $pidFile -Raw)
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$gatewayId" -ErrorAction SilentlyContinue
    if ($process -and $process.CommandLine -like '*server.mqtt_guard.gateway*' -and
        $process.CommandLine.IndexOf((Join-Path $projectRoot '.private\mqtt_guard\config.json'),[StringComparison]::OrdinalIgnoreCase) -ge 0) {
        Stop-Process -Id $gatewayId
    }
}
$linuxRoot = (Invoke-LocalWsl -d DriverMonitorLocal -u root -- wslpath -a $projectRoot.Replace('\','/')).Trim()
Invoke-LocalWsl -d DriverMonitorLocal -u root -- docker compose --env-file "$linuxRoot/.private/thingsboard/compose.env" `
    -f "$linuxRoot/server/thingsboard_local/compose.yml" stop
if ($LASTEXITCODE -ne 0) { throw 'Could not stop containers cleanly; check Docker before shutdown.' }
Invoke-LocalWsl -d DriverMonitorLocal -u root -- pkill -TERM -x dockerd
Write-Output 'Local services stopped. Database volumes and private keys are preserved.'
