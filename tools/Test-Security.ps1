param([switch]$Demo, [switch]$NoPause)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
$testPython = Join-Path $projectRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $testPython)) {
    throw 'Gateway Python environment is missing. Follow docs/CHAY_LOCAL_A_Z.md to set up the local server.'
}
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot '.private\mqtt_guard\config.json'))) {
    throw 'Local configuration is missing. Run setup before the security demonstration.'
}
New-Item -ItemType Directory -Force (Join-Path $projectRoot '.private\tmp') | Out-Null
$env:TEMP = Join-Path $projectRoot '.private\tmp'
$env:TMP = $env:TEMP
$env:PYTHONPATH = "$projectRoot;$projectRoot\.venv\Lib\site-packages"
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PYTHONIOENCODING = 'utf-8'

function Invoke-SecurityPython {
    # unittest and Python diagnostics write normal progress to stderr.
    $ErrorActionPreference = 'Continue'
    & $testPython @args
    if ($LASTEXITCODE -ne 0) { throw 'Security verification failed. Inspect the error above and the private test log.' }
}

Write-Output '=== Unit tests: protocol, replay state, freshness, data validation ==='
Invoke-SecurityPython -m unittest discover -s tests/security -v
Write-Output '=== Integration: real TLS/MQTT and ThingsBoard persistence ==='
$testArgs = @('tests/security/run_mqtt_integration.py','--thingsboard')
if ($Demo) { $testArgs += '--demo' }
if ($NoPause) { $testArgs += '--no-pause' }
Invoke-SecurityPython @testArgs
Write-Output 'PASS. Private integration report: .private/security_latest.json'
Write-Output 'The real board, its session, viewer and production data were not used as attack targets.'
