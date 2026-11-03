param(
    [string]$Port = 'COM4',
    [string]$IdfPath = 'D:\esp\v5.5.5\esp-idf',
    [string]$ToolsPath = 'D:\Espressif',
    [string]$PythonEnv = 'D:\Espressif\tools\python\v5.5.5\venv',
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force (Join-Path $projectRoot '.private\tmp') | Out-Null
$env:TEMP = Join-Path $projectRoot '.private\tmp'
$env:TMP = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = $PythonEnv
$env:IDF_TARGET = 'esp32s3'
$env:IDF_CCACHE_ENABLE = '1'
$env:CCACHE_DIR = Join-Path $projectRoot '.private\ccache'
$env:IDF_PY_BUILD_JOBS = '1'
# ESP-IDF writes ordinary progress to stderr. Windows PowerShell must not
# turn that progress into a terminating NativeCommandError when logging.
$ErrorActionPreference = 'Continue'
. (Join-Path $IdfPath 'export.ps1')
$activationResult = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($activationResult -ne 0 -or -not $env:IDF_PATH) { throw 'ESP-IDF activation failed.' }
$env:IDF_EXTRA_ACTIONS_PATH = Join-Path $projectRoot 'tools'
Set-Location (Join-Path $projectRoot 'examples\ov3660_face_detect')
$action = if ($BuildOnly) { 'build' } else { 'flash' }
$ErrorActionPreference = 'Continue'
python (Join-Path $IdfPath 'tools\idf.py') -B build_guard -D IDF_TARGET=esp32s3 `
    -D BSP=esp32_s3_eye_noglib -D DETECT_MODEL=human_face_detect `
    -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_s3_eye_noglib -D CCACHE_ENABLE=ON -p $Port $action
$buildResult = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildResult -ne 0) { throw 'ESP-IDF build/flash failed. Read the error above.' }
