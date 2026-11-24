#Requires -RunAsAdministrator
param(
    [string]$PluginsDir = ''
)
$ErrorActionPreference = 'Stop'

$pluginCandidates = @(
    (Join-Path $PSScriptRoot 'SonyHifFormat.8bi'),
    (Join-Path $PSScriptRoot 'build\SonyHifFormat.8bi')
)
$srcPlugin = $pluginCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $srcPlugin) {
    throw "SonyHifFormat.8bi not found. Place it next to install.ps1 or run build.ps1 first."
}

function Find-PhotoshopPluginsDir {
    $regKeys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Photoshop.exe',
        'HKLM:\SOFTWARE\Adobe\Photoshop'
    )
    $appPath = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Photoshop.exe' -ErrorAction SilentlyContinue).Path
    if ($appPath) {
        $cand = Join-Path $appPath 'Plug-ins'
        if (Test-Path $cand) { return $cand }
    }
    foreach ($drive in @('C:', 'D:', 'E:')) {
        $found = Get-ChildItem "$drive\Program Files\Adobe\Adobe Photoshop *\Plug-ins" -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return ''
}

if (-not $PluginsDir) {
    $PluginsDir = Find-PhotoshopPluginsDir
}
if (-not $PluginsDir -or -not (Test-Path $PluginsDir)) {
    throw "Photoshop Plug-ins directory not found. Pass -PluginsDir '<path-to-Photoshop>\Plug-ins' explicitly."
}

$running = Get-Process -Name 'Photoshop' -ErrorAction SilentlyContinue
if ($running) {
    Write-Error "Photoshop.exe is currently running (PID $($running[0].Id)). Close Photoshop before installing."
    exit 1
}

$destPlugin = Join-Path $PluginsDir 'SonyHifFormat.8bi'
Copy-Item -Force $srcPlugin $destPlugin
Write-Host "Installed Photoshop format plugin: $destPlugin" -ForegroundColor Green
