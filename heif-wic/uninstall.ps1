#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$destDir = 'C:\Program Files\CustomHeifWic'
$destDll = Join-Path $destDir 'heif_wic.dll'

if (Test-Path $destDll) {
    & regsvr32.exe /s /u $destDll
}

if (Test-Path $destDir) {
    Remove-Item -Recurse -Force $destDir -ErrorAction SilentlyContinue
}

Write-Host "Unregistered heif_wic.dll." -ForegroundColor Yellow
if (Test-Path $destDir) {
    Write-Host "Note: $destDir is still in use by a running process (e.g. Photos or Explorer). Close Photos / restart Explorer and delete $destDir manually." -ForegroundColor DarkYellow
}
