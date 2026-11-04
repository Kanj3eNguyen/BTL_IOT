param(
    [Parameter(Mandatory=$true)][string]$HostIP,
    [string]$Storage = 'D:\ThingsBoard_Local'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Docker.ps1')
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
if (-not ([IO.Path]::GetFullPath($Storage)).StartsWith('D:\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose a storage folder on D for this project.'
}
Start-LocalDocker
if (Test-Path "$projectRoot\.private\thingsboard\database_initialized") {
    Invoke-LocalDocker volume inspect driver-monitor-local_postgres-data *> $null
    if ($LASTEXITCODE -ne 0) { throw 'An existing installation needs database migration into Docker Desktop; initialization was not rerun.' }
}
New-Item -ItemType Directory -Force "$projectRoot\.private\tmp","$projectRoot\.private\thingsboard",$Storage | Out-Null
$env:TEMP = "$projectRoot\.private\tmp"
$env:TMP = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PIP_CACHE_DIR = "$projectRoot\.private\pip-cache"
$python = (Get-Command python -ErrorAction Stop).Source
if (-not (Test-Path "$projectRoot\.venv\Scripts\python.exe")) {
    & $python -m venv "$projectRoot\.venv"
    if ($LASTEXITCODE -ne 0) { throw 'Could not create Python environment.' }
}
$guardPython = "$projectRoot\.venv\Scripts\python.exe"
$dependencyProbe = @'
import sys
try:
    import amqtt, paho.mqtt.client
    from cryptography import x509
except ImportError:
    sys.exit(1)
'@
& $guardPython -c $dependencyProbe
if ($LASTEXITCODE -ne 0) {
    $ErrorActionPreference = 'Continue'
    & $guardPython -m pip install --no-cache-dir -r requirements-mqtt-guard.txt
    $pipResult = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($pipResult -ne 0) { throw 'Could not install MQTT guard requirements.' }
}
$env:PYTHONPATH = "$projectRoot;$projectRoot\.venv\Lib\site-packages"
& $guardPython -m server.mqtt_guard.provision --host $HostIP --configure-board
if ($LASTEXITCODE -ne 0) { throw 'Provisioning failed. Create sdkconfig/menuconfig first if missing.' }
@'
import pathlib,secrets
p=pathlib.Path('.private/thingsboard/compose.env')
if not p.exists():p.write_text('TB_DB_PASSWORD='+secrets.token_urlsafe(32)+'\n')
'@ | & $python -
Invoke-LocalCompose pull
if ($LASTEXITCODE -ne 0) { throw 'ThingsBoard image pull failed.' }
if (-not (Test-Path "$projectRoot\.private\thingsboard\database_initialized")) {
    Invoke-LocalCompose run --rm -e INSTALL_TB=true -e LOAD_DEMO=true thingsboard
    if ($LASTEXITCODE -ne 0) { throw 'Database initialization failed. Do not delete existing volumes.' }
    'initialized' | Set-Content "$projectRoot\.private\thingsboard\database_initialized"
}
& "$PSScriptRoot\Start-Local.ps1" -ServerOnly
$ready = $false
for ($i=0; $i -lt 120; $i++) {
    try {
        $response = Invoke-WebRequest 'http://127.0.0.1:8080/' -UseBasicParsing -TimeoutSec 2
        if ($response.StatusCode -eq 200) {$ready=$true; break}
    } catch {}
    Start-Sleep -Seconds 1
}
if (-not $ready) { throw 'ThingsBoard is not ready yet. Check container logs and RAM.' }
& $python -m server.thingsboard_local.setup
if ($LASTEXITCODE -ne 0) { throw 'Local device/account setup failed.' }
& $python -m server.thingsboard_local.dashboard
if ($LASTEXITCODE -ne 0) { throw 'Dashboard setup failed.' }
& "$PSScriptRoot\Start-Local.ps1"
Write-Output 'Setup complete. Close any viewer and run tools/Build-Flash.ps1 to flash the board.'
