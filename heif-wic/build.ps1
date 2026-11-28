param(
    [string]$Cargo = 'cargo',
    [string]$Gxx = 'g++',
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function Resolve-Tool([string]$name, [string]$hintPath) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($hintPath -and (Test-Path $hintPath)) { return $hintPath }
    throw "Required tool '$name' not found on PATH."
}

$cargoExe = Resolve-Tool $Cargo (Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe')
$gxxExe = Resolve-Tool $Gxx 'D:\Program Files\Mingw64\bin\g++.exe'

$manifest = Join-Path $root 'rust-heif-dec\Cargo.toml'
$cargoArgs = @('build', '--release', '--manifest-path', $manifest)
if ($Offline) { $cargoArgs += '--offline' }

# Prefer x86_64-pc-windows-gnu archive when host rustc defaults to MSVC
$rustcVer = (& rustc -vV 2>$null) -join "`n"
$rustLib = Join-Path $root 'rust-heif-dec\target\release\libheif_dec.a'
if ($rustcVer -notmatch 'host:\s+x86_64-pc-windows-gnu') {
    $cargoArgs += @('--target', 'x86_64-pc-windows-gnu')
    $rustLib = Join-Path $root 'rust-heif-dec\target\x86_64-pc-windows-gnu\release\libheif_dec.a'
}

& $cargoExe @cargoArgs
if ($LASTEXITCODE -ne 0) { throw "Cargo build failed ($LASTEXITCODE)" }
if (-not (Test-Path $rustLib)) { throw "Static library not found at $rustLib" }

$dllOut = Join-Path $buildDir 'heif_wic.dll'
& $gxxExe -O2 -shared -std=c++17 `
    (Join-Path $root 'heif_wic.cpp') `
    (Join-Path $root 'heif_wic.def') `
    $rustLib `
    -o $dllOut `
    -static -static-libgcc -static-libstdc++ `
    -lwindowscodecs -lole32 -loleaut32 -luuid -lshlwapi -lws2_32 -luserenv -lbcrypt -lntdll -lsynchronization
if ($LASTEXITCODE -ne 0) { throw "heif_wic.dll link failed ($LASTEXITCODE)" }

& $gxxExe -O2 -std=c++17 -municode `
    (Join-Path $root 'test_heif_local.cpp') `
    -o (Join-Path $buildDir 'test_heif_local.exe') `
    -static -static-libgcc -static-libstdc++ `
    -lwindowscodecs -lole32 -luuid -lshlwapi
if ($LASTEXITCODE -ne 0) { throw "test_heif_local.exe link failed ($LASTEXITCODE)" }

& $gxxExe -O2 -std=c++17 -municode `
    (Join-Path $root 'verify_heif_wic.cpp') `
    -o (Join-Path $buildDir 'verify_heif_wic.exe') `
    -static -static-libgcc -static-libstdc++ `
    -lwindowscodecs -lole32 -luuid
if ($LASTEXITCODE -ne 0) { throw "verify_heif_wic.exe link failed ($LASTEXITCODE)" }

$exportOut = Join-Path $buildDir 'heif_export.exe'
& $gxxExe -O2 -std=c++17 -finput-charset=UTF-8 -municode -mwindows `
    (Join-Path $root 'heif_export.cpp') `
    $rustLib `
    -o $exportOut `
    -static -static-libgcc -static-libstdc++ `
    -lwindowscodecs -lole32 -loleaut32 -luuid -lshell32 -lws2_32 -luserenv -lbcrypt -lntdll -lsynchronization
if ($LASTEXITCODE -ne 0) { throw "heif_export.exe link failed ($LASTEXITCODE)" }

Write-Host "Built: $dllOut" -ForegroundColor Green
Write-Host "Built: $exportOut" -ForegroundColor Green
