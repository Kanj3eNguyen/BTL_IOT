# Run manually after reviewing -WhatIf. Never touches Docker disks or backups.
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param([switch]$RemoveOptionalClang)

$ErrorActionPreference = 'Stop'
$targets = [Collections.Generic.List[string]]::new()
$distRoot = 'D:\Espressif\dist'
if (Test-Path -LiteralPath $distRoot) {
    # Files only: keep the directory and all installed toolchains.
    Get-ChildItem -LiteralPath $distRoot -File | Where-Object {
        $_.Name -match '\.(zip|tar\.xz|tar\.gz)$'
    } | ForEach-Object { $targets.Add($_.FullName) }
}
$rootfsArchive = 'D:\ThingsBoard_Local\downloads\alpine-minirootfs-3.24.3-x86_64.tar.gz'
if (Test-Path -LiteralPath $rootfsArchive) { $targets.Add($rootfsArchive) }

# Remove transfer/launcher leftovers only after the old dedicated distro is gone.
$oldDistro = Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss' -ErrorAction SilentlyContinue |
    Get-ItemProperty | Where-Object { $_.DistributionName -eq 'DriverMonitorLocal' }
$serverTemps = @()
if (-not $oldDistro) {
    $serverTemps = @(
        'D:\ThingsBoard_Local\migration-to-desktop\images.tar',
        'D:\ThingsBoard_Local\engine.pid',
        'D:\ThingsBoard_Local\engine.log',
        'D:\ThingsBoard_Local\engine-error.log',
        'D:\ThingsBoard_Local\start-engine.sh',
        'D:\ThingsBoard_Local\downloads\alpine-rootfs-path.txt'
    )
    foreach ($temporary in $serverTemps) {
        if (Test-Path -LiteralPath $temporary) { $targets.Add($temporary) }
    }
}

if ($RemoveOptionalClang) {
    $projectRoot = Split-Path $PSScriptRoot -Parent
    $cache = Join-Path $projectRoot 'examples\ov3660_face_detect\build_guard\CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache) -or
        -not (Select-String -LiteralPath $cache -Pattern '^IDF_TOOLCHAIN:STRING=gcc$' -Quiet)) {
        throw 'Cannot confirm this project uses GCC. Keep Clang installed.'
    }
    if (Get-Process -Name 'clang','clang++','clang-cl','clangd' -ErrorAction SilentlyContinue) {
        throw 'Clang is running. Close its build/editor before removing optional tools.'
    }
    $targets.Add('D:\Espressif\tools\esp-clang')
    $targets.Add('D:\Espressif\tools\esp-clang-libs')
}

# Validate every absolute target before deleting anything. No links are followed.
$validated = foreach ($target in $targets) {
    if (-not (Test-Path -LiteralPath $target)) { continue }
    $absolute = (Resolve-Path -LiteralPath $target).ProviderPath
    $parent = Split-Path $absolute -Parent
    $allowed = ($parent -eq $distRoot -and
                [IO.Path]::GetFileName($absolute) -match '\.(zip|tar\.xz|tar\.gz)$') -or
               $absolute -eq $rootfsArchive -or
               $absolute -in $serverTemps -or
               ($RemoveOptionalClang -and $absolute -in @(
                   'D:\Espressif\tools\esp-clang', 'D:\Espressif\tools\esp-clang-libs'))
    if (-not $allowed) { throw "Unexpected target: $absolute" }
    $item = Get-Item -LiteralPath $absolute -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing linked target: $absolute"
    }
    if ($item.PSIsContainer -and
        (Get-ChildItem -LiteralPath $absolute -Recurse -Force -Attributes ReparsePoint)) {
        throw "Refusing directory containing links: $absolute"
    }
    $absolute
}

foreach ($absolute in $validated) {
    if ($PSCmdlet.ShouldProcess($absolute, 'Delete redundant downloaded archive or optional Clang')) {
        Remove-Item -LiteralPath $absolute -Recurse -Force
        Write-Output "Removed: $absolute"
    }
}
Get-PSDrive -Name D | Select-Object Name, @{Name='FreeGiB'; Expression={[math]::Round($_.Free / 1GB, 2)}}
