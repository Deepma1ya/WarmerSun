# WarmerSun build script (MinGW-w64).
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1
param(
    [string]$Gxx = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $root "src"
$out  = Join-Path $root "dist\bin\win_x64"

if (-not $Gxx) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { $Gxx = $cmd.Source }
}
if (-not $Gxx -or -not (Test-Path $Gxx)) {
    foreach ($candidate in @("C:\mingw64\bin\g++.exe", "C:\mingw32\bin\g++.exe")) {
        if (Test-Path $candidate) { $Gxx = $candidate; break }
    }
}
if (-not $Gxx -or -not (Test-Path $Gxx)) {
    throw "g++ not found. Install MinGW-w64 or pass -Gxx <path>."
}

if ($Clean -and (Test-Path $out)) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

$common = @(
    "-std=c++17", "-O2", "-shared", "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", (Join-Path $src "common"),
    "-static-libgcc", "-static-libstdc++", "-static"
)

function Build-Target([string]$name, [string[]]$sources, [string[]]$libs) {
    $target = Join-Path $out $name
    Write-Host "==> $name"
    $args = @()
    $args += $common
    $args += "-o"; $args += $target
    $args += $sources
    $args += $libs
    & $Gxx @args
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
}

Build-Target "dxgi.dll" @(
    (Join-Path $src "dxgi\ws_dxgi.cpp"),
    (Join-Path $src "dxgi\ws_renderer.cpp"),
    (Join-Path $src "common\ws_common.cpp")
) @("-ld3d11", "-lole32", "-lshlwapi", "-luser32", "-lgdi32")

Build-Target "dinput8.dll" @(
    (Join-Path $src "dinput8\ws_dinput8.cpp"),
    (Join-Path $src "common\ws_common.cpp")
) @("-lole32", "-lshlwapi", "-luser32")

Copy-Item (Join-Path $root "assets\WarmerSun.ini") (Join-Path $out "WarmerSun.ini") -Force
Copy-Item (Join-Path $root "assets\INSTALL.txt") (Join-Path $out "INSTALL.txt") -Force

Write-Host ""
Write-Host "Built:"
Get-ChildItem $out | ForEach-Object { "  {0,-20} {1,10} bytes" -f $_.Name, $_.Length }
