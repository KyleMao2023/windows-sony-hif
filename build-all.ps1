param(
    [string]$PhotoshopSdkDir = '',
    [string]$Version = '0.1.0',
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$distDir = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

Write-Host '=== Building heif-wic ===' -ForegroundColor Cyan
& (Join-Path $root 'heif-wic\build.ps1') -Offline:$Offline

Write-Host '=== Building heif-ps-plugin ===' -ForegroundColor Cyan
& (Join-Path $root 'heif-ps-plugin\build.ps1') -PhotoshopSdkDir $PhotoshopSdkDir -Offline:$Offline

Add-Type -AssemblyName System.IO.Compression.FileSystem

function New-ReleaseZip([string]$zipPath, [string]$stagingName, [hashtable]$files) {
    $stage = Join-Path $root "build\stage-$stagingName"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    foreach ($kv in $files.GetEnumerator()) {
        $dst = Join-Path $stage $kv.Key
        $parent = Split-Path -Parent $dst
        if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        Copy-Item -Force $kv.Value $dst
    }
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    Remove-Item -Recurse -Force $stage
    Get-FileHash -LiteralPath $zipPath -Algorithm SHA256 | Format-List Path, Hash
}

New-ReleaseZip (Join-Path $distDir "sony-heif-windows-wic-$Version-win64.zip") 'wic' @{
    'heif_wic.dll'        = Join-Path $root 'heif-wic\build\heif_wic.dll'
    'heif_export.exe'     = Join-Path $root 'heif-wic\build\heif_export.exe'
    'verify_heif_wic.exe' = Join-Path $root 'heif-wic\build\verify_heif_wic.exe'
    'install.ps1'         = Join-Path $root 'heif-wic\install.ps1'
    'uninstall.ps1'       = Join-Path $root 'heif-wic\uninstall.ps1'
    'README.md'           = Join-Path $root 'heif-wic\README.md'
    'README.zh-CN.md'     = Join-Path $root 'heif-wic\README.zh-CN.md'
    'LICENSE'             = Join-Path $root 'LICENSE'
    'THIRD_PARTY.md'      = Join-Path $root 'THIRD_PARTY.md'
}

New-ReleaseZip (Join-Path $distDir "sony-heif-photoshop-plugin-$Version-win64.zip") 'ps' @{
    'SonyHifFormat.8bi' = Join-Path $root 'heif-ps-plugin\build\SonyHifFormat.8bi'
    'install.ps1'       = Join-Path $root 'heif-ps-plugin\install.ps1'
    'uninstall.ps1'     = Join-Path $root 'heif-ps-plugin\uninstall.ps1'
    'README.md'         = Join-Path $root 'heif-ps-plugin\README.md'
    'README.zh-CN.md'   = Join-Path $root 'heif-ps-plugin\README.zh-CN.md'
    'LICENSE'           = Join-Path $root 'LICENSE'
    'THIRD_PARTY.md'    = Join-Path $root 'THIRD_PARTY.md'
}
