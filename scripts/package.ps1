# Builds WarmerSun and packages the distributable zip.
# The asset is always named WarmerSun.zip so the stable
# /releases/latest/download/WarmerSun.zip link keeps working.
param(
    [string]$Version = "dev",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bin  = Join-Path $root "dist\bin\win_x64"
$stage = Join-Path $root "dist\package"
$zip  = Join-Path $root "dist\WarmerSun.zip"

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (-not (Test-Path (Join-Path $bin "dxgi.dll"))) { throw "missing build output in $bin" }

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $bin "*") $stage -Recurse -Force

# Stamp the version into the package's install note.
$install = Join-Path $stage "INSTALL.txt"
if (Test-Path $install) {
    Add-Content $install "`r`nVersion: $Version`r`n"
}

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip

Write-Host "package: $zip"
Get-ChildItem $stage | ForEach-Object { "  {0,-18} {1,10} bytes" -f $_.Name, $_.Length }
