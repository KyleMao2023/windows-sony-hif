#Requires -RunAsAdministrator
param(
    [string]$PluginsDir = ''
)
$ErrorActionPreference = 'Stop'

if (-not $PluginsDir) {
    $appPath = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Photoshop.exe' -ErrorAction SilentlyContinue).Path
    if ($appPath -and (Test-Path (Join-Path $appPath 'Plug-ins\SonyHifFormat.8bi'))) {
        $PluginsDir = Join-Path $appPath 'Plug-ins'
    } else {
        foreach ($drive in @('C:', 'D:', 'E:')) {
            $found = Get-ChildItem "$drive\Program Files\Adobe\Adobe Photoshop *\Plug-ins\SonyHifFormat.8bi" -File -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($found) { $PluginsDir = $found.DirectoryName; break }
        }
    }
}

if (-not $PluginsDir) {
    Write-Host "SonyHifFormat.8bi was not found in common Photoshop Plug-ins directories." -ForegroundColor Yellow
    exit 0
}

$target = Join-Path $PluginsDir 'SonyHifFormat.8bi'
if (Test-Path $target) {
    $running = Get-Process -Name 'Photoshop' -ErrorAction SilentlyContinue
    if ($running) {
        Write-Error "Photoshop.exe is currently running. Close Photoshop before uninstalling."
        exit 1
    }
    Remove-Item -Force $target
    Write-Host "Removed: $target" -ForegroundColor Yellow
} else {
    Write-Host "Not present: $target" -ForegroundColor Yellow
}
