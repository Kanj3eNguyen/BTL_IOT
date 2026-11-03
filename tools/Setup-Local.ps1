param(
    [Parameter(Mandatory=$true)][string]$HostIP,
    [string]$Storage = 'D:\ThingsBoard_Local'
)
$ErrorActionPreference = 'Stop'
function Invoke-LocalWsl {
    $ErrorActionPreference = 'Continue'
    & wsl @args
}
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
if (-not ([IO.Path]::GetFullPath($Storage)).StartsWith('D:\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose a storage folder on D for this project.'
}
New-Item -ItemType Directory -Force "$projectRoot\.private\tmp","$projectRoot\.private\thingsboard","$Storage\downloads" | Out-Null
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
$distros = ((Invoke-LocalWsl --list --quiet) -join "`n") -replace "`0",''
if ($distros -notmatch '(?m)^DriverMonitorLocal\s*$') {
    # Fetch and checksum a minimal official Alpine rootfs, stored entirely on D.
    $env:DRIVER_LOCAL_STORAGE = $Storage
    @'
import hashlib, os, pathlib, re, urllib.request
p=pathlib.Path(os.environ['DRIVER_LOCAL_STORAGE'])/'downloads'
base='https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/'
index=urllib.request.urlopen(base+'latest-releases.yaml').read().decode()
name=re.search(r'file: (alpine-minirootfs-[^\s]+\.tar\.gz)',index).group(1)
target=p/name
urllib.request.urlretrieve(base+name,target)
expected=urllib.request.urlopen(base+name+'.sha256').read().decode().split()[0]
assert hashlib.sha256(target.read_bytes()).hexdigest()==expected
(p/'alpine-rootfs-path.txt').write_text(str(target))
'@ | & $python -
    if ($LASTEXITCODE -ne 0) { throw 'Rootfs download/checksum failed.' }
    $rootfs = (Get-Content "$Storage\downloads\alpine-rootfs-path.txt" -Raw).Trim()
    Invoke-LocalWsl --import DriverMonitorLocal "$Storage\wsl" $rootfs --version 2
    if ($LASTEXITCODE -ne 0) { throw 'WSL import failed. WSL 2 must already be installed.' }
    Invoke-LocalWsl -d DriverMonitorLocal -u root -- apk add --no-cache docker docker-cli-compose ca-certificates
    if ($LASTEXITCODE -ne 0) { throw 'Docker package installation failed.' }
}
& $guardPython -m server.mqtt_guard.provision --host $HostIP --configure-board
if ($LASTEXITCODE -ne 0) { throw 'Provisioning failed. Create sdkconfig/menuconfig first if missing.' }
@'
import pathlib,secrets
p=pathlib.Path('.private/thingsboard/compose.env')
if not p.exists():p.write_text('TB_DB_PASSWORD='+secrets.token_urlsafe(32)+'\n')
'@ | & $python -
$engineCheck = Invoke-LocalWsl -d DriverMonitorLocal -u root -- pgrep -x dockerd
if ($LASTEXITCODE -ne 0) {
    New-Item -ItemType Directory -Force "$projectRoot\.private\runtime" | Out-Null
    $engine = Start-Process wsl.exe -ArgumentList @('-d','DriverMonitorLocal','-u','root','--','dockerd') `
        -WindowStyle Hidden -RedirectStandardOutput "$projectRoot\.private\runtime\engine.log" `
        -RedirectStandardError "$projectRoot\.private\runtime\engine-error.log" -PassThru
    $engine.Id | Set-Content "$projectRoot\.private\runtime\engine.pid"
}
$ready = $false
for ($i=0; $i -lt 30; $i++) {
    Invoke-LocalWsl -d DriverMonitorLocal -u root -- docker info --format '{{.ServerVersion}}' *> $null
    if ($LASTEXITCODE -eq 0) {$ready=$true; break}
    Start-Sleep -Seconds 1
}
if (-not $ready) { throw 'Docker engine not ready.' }
$linuxRoot = (Invoke-LocalWsl -d DriverMonitorLocal -u root -- wslpath -a $projectRoot.Replace('\','/')).Trim()
$composeArgs = @('-d','DriverMonitorLocal','-u','root','--','docker','compose','--env-file',
    "$linuxRoot/.private/thingsboard/compose.env",'-f',"$linuxRoot/server/thingsboard_local/compose.yml")
Invoke-LocalWsl @composeArgs pull
if ($LASTEXITCODE -ne 0) { throw 'ThingsBoard image pull failed.' }
if (-not (Test-Path "$projectRoot\.private\thingsboard\database_initialized")) {
    Invoke-LocalWsl @composeArgs run --rm -e INSTALL_TB=true -e LOAD_DEMO=true thingsboard
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
