param([switch]$NoPause)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
$testPython = Join-Path $projectRoot '.venv\Scripts\python.exe'
$env:PYTHONPATH = "$projectRoot;$projectRoot\.venv\Lib\site-packages"
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:TEMP = Join-Path $projectRoot '.private\tmp'
$env:TMP = $env:TEMP
function Invoke-LivePython {
    $ErrorActionPreference = 'Continue'
    & $testPython tests/security/demo_live_gateway.py @args
    if ($LASTEXITCODE -ne 0) { throw 'Live security demo failed; inspect the output and its private report.' }
}
$runFolder = $null
try {
    $runFolder = (Invoke-LivePython prepare).Trim()
    Write-Output 'Restarting only the gateway to load the temporary demo identity; the board reconnects automatically.'
    & "$PSScriptRoot\Restart-Gateway.ps1"
    $demoArgs = @('run','--folder',$runFolder)
    if ($NoPause) { $demoArgs += '--no-pause' }
    Invoke-LivePython @demoArgs
} finally {
    if ($runFolder) {
        try { Invoke-LivePython cleanup --folder $runFolder }
        finally { & "$PSScriptRoot\Restart-Gateway.ps1" }
    }
}
