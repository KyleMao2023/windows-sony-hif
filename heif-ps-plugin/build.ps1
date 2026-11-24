param(
    [string]$PhotoshopSdkDir = '',
    [string]$Cargo = 'cargo',
    [string]$Gxx = 'g++',
    [string]$Windres = 'windres',
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repoRoot = Split-Path -Parent $root
$buildDir = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function Resolve-Tool([string]$name, [string]$hintPath) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($hintPath -and (Test-Path $hintPath)) { return $hintPath }
    throw "Required tool '$name' not found on PATH."
}

$sdkCandidates = @(@(
    $PhotoshopSdkDir,
    $env:PHOTOSHOP_SDK_DIR,
    (Join-Path $repoRoot 'sdk\photoshop-cpp-sdk'),
    (Join-Path $root 'sdk\photoshop-cpp-sdk')
) | Where-Object { $_ -and (Test-Path $_) })

if ($sdkCandidates.Count -eq 0) {
    throw @"
Adobe Photoshop C++ SDK directory not found.
Clone https://github.com/AdobeDocs/photoshop-cpp-sdk and either:
  1. Pass -PhotoshopSdkDir <path-to-photoshop-cpp-sdk>, OR
  2. Set `$env:PHOTOSHOP_SDK_DIR, OR
  3. Place it at $($repoRoot)\sdk\photoshop-cpp-sdk
"@
}
$sdkRoot = $sdkCandidates[0]
$psInc1 = Join-Path $sdkRoot 'pluginsdk\photoshopapi\photoshop'
$psInc2 = Join-Path $sdkRoot 'pluginsdk\photoshopapi\pica_sp'
if (-not (Test-Path (Join-Path $psInc1 'PIFormat.h'))) {
    throw "PIFormat.h not found under $psInc1. Point -PhotoshopSdkDir to the root of AdobeDocs/photoshop-cpp-sdk."
}

$cargoExe = Resolve-Tool $Cargo (Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe')
$gxxExe = Resolve-Tool $Gxx 'D:\Program Files\Mingw64\bin\g++.exe'
$windresExe = Resolve-Tool $Windres 'D:\Program Files\Mingw64\bin\windres.exe'

$manifest = Join-Path $root 'rust-sony-hif\Cargo.toml'
$cargoArgs = @('build', '--release', '--manifest-path', $manifest)
if ($Offline) { $cargoArgs += '--offline' }

$rustcVer = (& rustc -vV 2>$null) -join "`n"
$rustLib = Join-Path $root 'rust-sony-hif\target\release\libsony_hif.a'
if ($rustcVer -notmatch 'host:\s+x86_64-pc-windows-gnu') {
    $cargoArgs += @('--target', 'x86_64-pc-windows-gnu')
    $rustLib = Join-Path $root 'rust-sony-hif\target\x86_64-pc-windows-gnu\release\libsony_hif.a'
}

& $cargoExe @cargoArgs
if ($LASTEXITCODE -ne 0) { throw "Cargo build failed ($LASTEXITCODE)" }
if (-not (Test-Path $rustLib)) { throw "Static library not found at $rustLib" }

$srcDir = Join-Path $root 'src'
$resOut = Join-Path $buildDir 'SonyHifFormat.res'
& $windresExe (Join-Path $srcDir 'SonyHifFormat.rc') -I $srcDir -O coff -o $resOut
if ($LASTEXITCODE -ne 0) { throw "windres failed ($LASTEXITCODE)" }

$pluginOut = Join-Path $buildDir 'SonyHifFormat.8bi'
& $gxxExe -O2 -shared -std=c++17 `
    "-I$srcDir" "-I$psInc1" "-I$psInc2" `
    (Join-Path $srcDir 'SonyHifFormat.cpp') `
    $resOut `
    (Join-Path $srcDir 'SonyHifFormat.def') `
    $rustLib `
    -o $pluginOut `
    -static -static-libgcc -static-libstdc++ `
    -luser32 -lws2_32 -luserenv -lbcrypt -lntdll -lsynchronization
if ($LASTEXITCODE -ne 0) { throw "SonyHifFormat.8bi link failed ($LASTEXITCODE)" }

& $gxxExe -O2 -std=c++17 `
    "-I$psInc1" "-I$psInc2" `
    (Join-Path $root 'test_ps_plugin.cpp') `
    -o (Join-Path $buildDir 'test_ps_plugin.exe') `
    -static -static-libgcc -static-libstdc++ `
    -lmscms -luser32
if ($LASTEXITCODE -ne 0) { throw "test_ps_plugin.exe link failed ($LASTEXITCODE)" }

Write-Host "Built: $pluginOut" -ForegroundColor Green
