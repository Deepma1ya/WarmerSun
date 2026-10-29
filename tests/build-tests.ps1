# Builds and runs the WarmerSun end-to-end harness.
# Usage: powershell -ExecutionPolicy Bypass -File tests\build-tests.ps1 [-NoRun]
param(
    [string]$Gxx = "",
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out  = Join-Path $root "tests\out"

if (-not $Gxx) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { $Gxx = $cmd.Source }
}
if (-not $Gxx -or -not (Test-Path $Gxx)) {
    foreach ($candidate in @("C:\mingw64\bin\g++.exe", "C:\mingw32\bin\g++.exe")) {
        if (Test-Path $candidate) { $Gxx = $candidate; break }
    }
}
if (-not $Gxx -or -not (Test-Path $Gxx)) { throw "g++ not found" }

# Make sure the proxy DLLs are current.
& powershell -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1")

New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $root "dist\bin\win_x64\dxgi.dll") $out -Force
Copy-Item (Join-Path $root "dist\bin\win_x64\dinput8.dll") $out -Force

$ini = @"
[WarmerSun]
Enabled=1
Preset=D
KeyToggle=none
KeyPreset=none
KeyReload=none
DumpFrames=1
DumpAfter=155
DumpCount=1

[Preset.D]
Temperature=9.0
Tint=1.0
PreExposure=0.0
Exposure=0.0
PreContrast=1.0
Contrast=1.06
Saturation=1.07
Vibrance=0.18
Highlights=-0.12
Shadows=0.07
Whites=0.02
Blacks=-0.03
NightExposure=0.45
SunWarmth=0.40
Dynamic=1
AdaptSpeed=1.0
Key=0.16
MinEV=-2.0
MaxEV=1.5
Sharpen=1
Sharpness=0.35
"@
Set-Content (Join-Path $out "WarmerSun.ini") $ini -Encoding ASCII

Write-Host "==> harness"
& $Gxx -std=c++17 -O2 -municode -mwindows `
    (Join-Path $root "tests\harness.cpp") `
    -o (Join-Path $out "harness.exe") `
    -ld3d11 -ldxgi -lole32 -luser32 -static-libgcc -static-libstdc++ -static
if ($LASTEXITCODE -ne 0) { throw "harness build failed" }

if ($NoRun) {
    Write-Host "Built $out\harness.exe (not run)"
    exit 0
}

Remove-Item (Join-Path $out "WarmerSun_frame_*.dds") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $out "harness_before.dds") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $out "WarmerSun.log") -ErrorAction SilentlyContinue

Write-Host "==> run"
$p = Start-Process -FilePath (Join-Path $out "harness.exe") -WorkingDirectory $out -PassThru
if (-not $p.WaitForExit(30000)) {
    $p.Kill()
    throw "harness timed out"
}
Write-Host "exit: $($p.ExitCode)"

Start-Sleep -Milliseconds 300
Get-ChildItem $out -Filter "*.dds" | ForEach-Object { "  $($_.Name)  $($_.Length)" }
if (Test-Path (Join-Path $out "WarmerSun.log")) {
    Write-Host "--- log ---"
    Get-Content (Join-Path $out "WarmerSun.log") | Select-Object -Last 20
}
