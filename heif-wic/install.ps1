#Requires -RunAsAdministrator
param(
    [string]$InstallDir = 'C:\Program Files\CustomHeifWic',
    [string[]]$VerifyFiles = @()
)
$ErrorActionPreference = 'Stop'

$dllCandidates = @(
    (Join-Path $PSScriptRoot 'heif_wic.dll'),
    (Join-Path $PSScriptRoot 'build\heif_wic.dll')
)
$srcDll = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $srcDll) {
    throw "heif_wic.dll not found. Place heif_wic.dll next to install.ps1 or run build.ps1 first."
}

$destDll = Join-Path $InstallDir 'heif_wic.dll'
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Get-Process -Name 'Photos','Microsoft.Photos' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
try {
    Copy-Item -Force $srcDll $destDll
} catch {
    Write-Error "Cannot overwrite $destDll because it is currently locked by Explorer or a preview process. Run uninstall.ps1, close preview windows, and retry."
    exit 1
}

# Grant Read+Execute to ALL APPLICATION PACKAGES so UWP apps (Microsoft Photos) can load the COM DLL
icacls $InstallDir /grant "*S-1-15-2-1:(OI)(CI)(RX)" /T | Out-Null
& regsvr32.exe /s $destDll
if ($LASTEXITCODE -ne 0) {
    throw "regsvr32 failed with exit code $LASTEXITCODE"
}

Write-Host "Installed and registered: $destDll" -ForegroundColor Green

$verifyExe = @(
    (Join-Path $PSScriptRoot 'verify_heif_wic.exe'),
    (Join-Path $PSScriptRoot 'build\verify_heif_wic.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($VerifyFiles.Count -gt 0 -and $verifyExe) {
    & $verifyExe @VerifyFiles
}
