param([switch]$ServerOnly)
$ErrorActionPreference = 'Stop'
function Invoke-LocalWsl {
    $ErrorActionPreference = 'Continue'
    & wsl @args
}
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
$runtimePath = Join-Path $projectRoot '.private\runtime'
New-Item -ItemType Directory -Force $runtimePath,(Join-Path $projectRoot '.private\tmp') | Out-Null
$env:TEMP = Join-Path $projectRoot '.private\tmp'
$env:TMP = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PYTHONPATH = "$projectRoot;$projectRoot\.venv\Lib\site-packages"
$pythonCache = Join-Path $runtimePath 'python.path'
$pythonCommand = if (Test-Path $pythonCache) {(Get-Content $pythonCache -Raw).Trim()} else {''}
if (-not $pythonCommand -or -not (Test-Path $pythonCommand)) {
    $pythonCommand = (Get-Command python -ErrorAction Stop).Source
    $pythonCommand | Set-Content $pythonCache
}
$envFile = Join-Path $projectRoot '.private\thingsboard\compose.env'
$composeFile = Join-Path $projectRoot 'server\thingsboard_local\compose.yml'
$configFile = Join-Path $projectRoot '.private\mqtt_guard\config.json'
if (-not (Test-Path $envFile) -or -not (Test-Path $configFile)) {
    throw 'Run provision/setup first; see docs/CHAY_LOCAL_A_Z.md.'
}
# The dedicated WSL filesystem, Docker images and volumes are stored on D.
# A foreground WSL process keeps the engine alive without Docker Desktop.
$engineCheck = Invoke-LocalWsl -d DriverMonitorLocal -u root -- pgrep -x dockerd
if ($LASTEXITCODE -ne 0) {
    $engine = Start-Process wsl.exe -ArgumentList @('-d','DriverMonitorLocal','-u','root','--','dockerd') `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runtimePath 'engine.log') `
        -RedirectStandardError (Join-Path $runtimePath 'engine-error.log') -PassThru
    $engine.Id | Set-Content (Join-Path $runtimePath 'engine.pid')
}
$ready = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    Invoke-LocalWsl -d DriverMonitorLocal -u root -- docker info --format '{{.ServerVersion}}' *> $null
    if ($LASTEXITCODE -eq 0) { $ready = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $ready) { throw 'Local Docker engine did not start; see .private/runtime/engine-error.log.' }
$linuxRoot = (Invoke-LocalWsl -d DriverMonitorLocal -u root -- wslpath -a $projectRoot.Replace('\','/')).Trim()
Invoke-LocalWsl -d DriverMonitorLocal -u root -- docker compose --env-file "$linuxRoot/.private/thingsboard/compose.env" `
    -f "$linuxRoot/server/thingsboard_local/compose.yml" up -d
if ($LASTEXITCODE -ne 0) { throw 'ThingsBoard startup failed.' }
if (-not $ServerOnly) {
    $existing = Get-NetTCPConnection -LocalPort 8885 -State Listen -ErrorAction SilentlyContinue
    if (-not $existing) {
        $gateway = Start-Process $pythonCommand -ArgumentList @('-u','-m','server.mqtt_guard.gateway','--config',('"'+$configFile+'"')) `
            -WorkingDirectory $projectRoot -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $runtimePath 'gateway.log') `
            -RedirectStandardError (Join-Path $runtimePath 'gateway-error.log') -PassThru
        $gateway.Id | Set-Content (Join-Path $runtimePath 'gateway.pid')
        $listening = $false
        for ($attempt = 0; $attempt -lt 10; $attempt++) {
            if ($gateway.HasExited) { break }
            if (Get-NetTCPConnection -LocalPort 8885 -State Listen -ErrorAction SilentlyContinue) {
                $listening = $true; break
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $listening) { throw 'Gateway did not start; see .private/runtime/gateway-error.log.' }
    } else {
        $owner = Get-CimInstance Win32_Process -Filter "ProcessId=$($existing[0].OwningProcess)"
        if (-not $owner -or $owner.CommandLine -notlike '*server.mqtt_guard.gateway*' -or
            $owner.CommandLine.IndexOf($configFile,[StringComparison]::OrdinalIgnoreCase) -lt 0) {
            throw 'Port 8885 is owned by another application; no process was stopped.'
        }
    }
}
Write-Output 'ThingsBoard: http://127.0.0.1:8080 (first startup may take a few minutes)'
Write-Output 'Private login details: .private/thingsboard/DANG_NHAP.txt'
Write-Output 'MQTT guard: TLS TCP 8885; bootstrap time: UDP 123. Camera viewer uses COM4 separately.'
