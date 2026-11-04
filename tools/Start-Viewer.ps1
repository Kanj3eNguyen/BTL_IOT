param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port = 'COM4',
    [string]$Python,
    [int]$Baud = 1500000,
    [switch]$Classic,
    [switch]$Detached
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$viewer = Join-Path $projectRoot 'examples\ov3660_face_detect\driver_monitor_viewer.py'
$runtime = Join-Path $projectRoot '.private\runtime'
New-Item -ItemType Directory -Force $runtime,(Join-Path $projectRoot '.private\tmp') | Out-Null
$env:TEMP = Join-Path $projectRoot '.private\tmp'
$env:TMP = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$previousPythonPath = $env:PYTHONPATH
try {
    # Use the selected interpreter's own packages, independently of ESP-IDF/gateway.
    $env:PYTHONPATH = $null
    $candidates = @()
    if ($Python) {
        $candidates += $Python
    } else {
        $cached = Join-Path $runtime 'python.path'
        if (Test-Path -LiteralPath $cached) { $candidates += (Get-Content -LiteralPath $cached -Raw).Trim() }
        $candidates += Join-Path $projectRoot '.venv\Scripts\python.exe'
        $candidates += @(Get-Command python -All -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -and $_.Source -notlike '*\WindowsApps\*' } |
            Select-Object -ExpandProperty Source)
    }
    $selected = $null
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $ErrorActionPreference = 'Continue'
        $probe = if ($Classic) { 'import cv2, numpy, serial' } else { 'import cv2, numpy, serial, tkinter' }
        & $candidate -c $probe *> $null
        $probeExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($probeExit -eq 0) { $selected = $candidate; break }
    }
    if (-not $selected) {
        throw 'No selected Python has cv2/numpy/serial/Tkinter. Use -Python with a suitable viewer environment. Tkinter is an optional Python installation component; do not install viewer packages into ESP-IDF.'
    }
    Write-Output "Viewer Python: $selected"
    $viewerArgs = @('-u',$viewer,'--port',$Port,'--baud',"$Baud")
    if ($Classic) { $viewerArgs += '--classic' }
    if ($Detached) {
        $existing = Get-CimInstance Win32_Process | Where-Object {
            $_.Name -eq 'python.exe' -and $_.CommandLine -and
            $_.CommandLine.IndexOf($viewer,[StringComparison]::OrdinalIgnoreCase) -ge 0 -and
            $_.CommandLine -match "--port\s+$Port(?:\s|$)"
        }
        if ($existing) { throw "A viewer for $Port is already running. Close it with Q first." }
        $viewerArgs[1] = '"'+$viewer+'"'
        $process = Start-Process $selected -WorkingDirectory $projectRoot `
            -ArgumentList $viewerArgs `
            -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $runtime "viewer-$Port.log") `
            -RedirectStandardError (Join-Path $runtime "viewer-$Port-error.log")
        $process.Id | Set-Content (Join-Path $runtime "viewer-$Port.pid")
        Start-Sleep -Seconds 2
        if ($process.HasExited) { throw "Viewer exited; see .private/runtime/viewer-$Port-error.log." }
        Write-Output "Viewer started for $Port. Press Q in its video window to close."
    } else {
        $ErrorActionPreference = 'Continue'
        & $selected @viewerArgs
        $result = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($result -ne 0) { throw 'Viewer failed. Check the error above and close other COM applications.' }
    }
} finally {
    $env:PYTHONPATH = $previousPythonPath
}
