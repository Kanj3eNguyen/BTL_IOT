$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$configFile = Join-Path $projectRoot '.private\mqtt_guard\config.json'
$listeners = @(Get-NetTCPConnection -LocalPort 8885 -State Listen -ErrorAction SilentlyContinue)
foreach ($listener in $listeners) {
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$($listener.OwningProcess)"
    if (-not $process -or $process.CommandLine -notlike '*server.mqtt_guard.gateway*' -or
        $process.CommandLine.IndexOf($configFile,[StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw 'Port 8885 belongs to another application. No process was stopped.'
    }
    Stop-Process -Id $process.ProcessId
}
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    if (-not (Get-NetTCPConnection -LocalPort 8885 -State Listen -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 250
}
& (Join-Path $PSScriptRoot 'Start-Local.ps1')
