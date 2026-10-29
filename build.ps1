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

$imgui = Join-Path $root "third_party\imgui-1.91.8"
$minhook = Join-Path $root "third_party\minhook-1.3.3"
$common = @(
    "-std=c++17", "-O2", "-shared", "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", (Join-Path $src "common"),
    "-I", $imgui,
    "-I", (Join-Path $imgui "backends"),
    "-I", (Join-Path $minhook "include"),
    "-static-libgcc", "-static-libstdc++", "-static"
)

# MinHook is C; build it with gcc and link the objects into dxgi.dll.
$gcc = Join-Path (Split-Path $Gxx) "gcc.exe"
$mhObjDir = Join-Path $root "dist\obj\minhook"
New-Item -ItemType Directory -Force $mhObjDir | Out-Null
$mhSources = @("src\buffer.c", "src\hook.c", "src\trampoline.c", "src\hde\hde64.c")
$mhObjects = @()
foreach ($s in $mhSources) {
    $obj = Join-Path $mhObjDir ((Split-Path $s -Leaf) + ".o")
    & $gcc -c -O2 -I (Join-Path $minhook "include") -I (Join-Path $minhook "src") `
        (Join-Path $minhook $s) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "minhook compile failed: $s" }
    $mhObjects += $obj
}

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

$dxgiSources = @(
    (Join-Path $src "dxgi\ws_dxgi.cpp"),
    (Join-Path $src "dxgi\ws_renderer.cpp"),
    (Join-Path $src "dxgi\ws_menu.cpp"),
    (Join-Path $src "common\ws_common.cpp"),
    (Join-Path $imgui "imgui.cpp"),
    (Join-Path $imgui "imgui_draw.cpp"),
    (Join-Path $imgui "imgui_tables.cpp"),
    (Join-Path $imgui "imgui_widgets.cpp"),
    (Join-Path $imgui "backends\imgui_impl_dx11.cpp"),
    (Join-Path $imgui "backends\imgui_impl_win32.cpp")
) + $mhObjects
Build-Target "dxgi.dll" $dxgiSources @("-ld3d11", "-ld3dcompiler", "-lole32", "-lshlwapi", "-luser32", "-lgdi32", "-ldwmapi")

Build-Target "dinput8.dll" @(
    (Join-Path $src "dinput8\ws_dinput8.cpp"),
    (Join-Path $src "common\ws_common.cpp")
) @("-lole32", "-lshlwapi", "-luser32")

Copy-Item (Join-Path $root "assets\WarmerSun.ini") (Join-Path $out "WarmerSun.ini") -Force
Copy-Item (Join-Path $root "assets\INSTALL.txt") (Join-Path $out "INSTALL.txt") -Force

Write-Host ""
Write-Host "Built:"
Get-ChildItem $out | ForEach-Object { "  {0,-20} {1,10} bytes" -f $_.Name, $_.Length }



