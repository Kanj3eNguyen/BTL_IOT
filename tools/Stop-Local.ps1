$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Docker.ps1')
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
Invoke-LocalCompose stop
if ($LASTEXITCODE -ne 0) { throw 'Could not stop containers cleanly; check Docker before shutdown.' }
Write-Output 'Project containers and gateway stopped. Docker Desktop, other projects, database volumes and private keys are preserved.'
