# Shared Docker Desktop helpers. Always target its engine explicitly.
function Invoke-LocalDocker {
    $ErrorActionPreference = 'Continue'
    & docker --context desktop-linux @args
}

function Assert-LocalDockerStorageOnD {
    # This PC keeps Docker Desktop's standard disk directory as a junction to D.
    $standard = Join-Path $env:LOCALAPPDATA 'Docker\wsl\disk'
    $disk = Get-Item -LiteralPath $standard -Force -ErrorAction SilentlyContinue
    if ($disk -and $disk.Target -and
        ([string]$disk.Target).StartsWith('D:\',[StringComparison]::OrdinalIgnoreCase)) { return }
    # Also support a disk location configured through Docker Desktop Settings.
    $settingsPath = Join-Path $env:APPDATA 'Docker\settings-store.json'
    if (Test-Path -LiteralPath $settingsPath) {
        $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
        $dataFolder = [string]$settings.DataFolder
        if ($dataFolder.StartsWith('D:\',[StringComparison]::OrdinalIgnoreCase) -and
            (Test-Path -LiteralPath $dataFolder) -and
            -not (Test-Path -LiteralPath (Join-Path $standard 'docker_data.vhdx'))) { return }
    }
    throw 'Docker Desktop disk storage on D is not confirmed. See docs/CHAY_LOCAL_A_Z.md before starting or pulling images.'
}

function Start-LocalDocker {
    Assert-LocalDockerStorageOnD
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw 'Docker Desktop is required. Reuse the existing installation; see docs/CHAY_LOCAL_A_Z.md.'
    }
    Invoke-LocalDocker info --format '{{.ServerVersion}}' *> $null
    if ($LASTEXITCODE -ne 0) {
        # Docker Desktop is pre-existing software, not a new installation.
        $ErrorActionPreference = 'Continue'
        & docker desktop start --detach
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0) { throw 'Open Docker Desktop and wait for Engine running, then retry.' }
    }
    for ($attempt = 0; $attempt -lt 45; $attempt++) {
        Invoke-LocalDocker info --format '{{.ServerVersion}}' *> $null
        if ($LASTEXITCODE -eq 0) { return }
        Start-Sleep -Seconds 1
    }
    throw 'Docker Desktop engine is not ready. Open Docker Desktop to inspect its status.'
}

function Invoke-LocalCompose {
    $localRoot = Split-Path $PSScriptRoot -Parent
    Invoke-LocalDocker compose --env-file (Join-Path $localRoot '.private\thingsboard\compose.env') `
        -f (Join-Path $localRoot 'server\thingsboard_local\compose.yml') @args
}
