# Builds installer\out\RemoteDisplaySetup.msi from the Release build and the signed driver package.
#   powershell -ExecutionPolicy Bypass -File installer\Build-Installer.ps1
# Needs the WiX 7 CLI (winget: WiXToolset.WiXCLI). Build the app and the driver first:
#   cmake --build build --config Release --target RemoteDisplay
#   powershell -File driver\build.ps1 -Package
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\Release\RemoteDisplay.exe"
$pkg = Join-Path $root "driver\out\pkg"
$stage = Join-Path $PSScriptRoot "stage"
$out = Join-Path $PSScriptRoot "out"

if (-not (Test-Path $exe)) { throw "missing $exe (build the RemoteDisplay target first)" }
foreach ($f in "RemoteDisplayVirtual.inf", "RemoteDisplayVirtual.cat", "RemoteDisplayVirtual.dll") {
    if (-not (Test-Path (Join-Path $pkg $f))) { throw "missing $f in $pkg (run driver\build.ps1 -Package)" }
}

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $stage "driver"), $out | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $pkg "*") (Join-Path $stage "driver") -Force

$wixBin = "C:\Program Files (x86)\WiX Toolset v3.14\bin"
$candle = Join-Path $wixBin "candle.exe"
$light = Join-Path $wixBin "light.exe"
if (-not (Test-Path $candle)) { throw "WiX 3.14 not found (winget install WiXToolset.WiXToolset --version 3.14.1.8722)" }

$obj = Join-Path $PSScriptRoot "obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$wixobj = Join-Path $obj "RemoteDisplay.wixobj"
& $candle -nologo -arch x64 "-dStageDir=$stage" -out $wixobj (Join-Path $PSScriptRoot "RemoteDisplay.wxs")
if ($LASTEXITCODE -ne 0) { throw "candle failed" }

$msi = Join-Path $out "RemoteDisplaySetup.msi"
& $light -nologo -out $msi $wixobj
if ($LASTEXITCODE -ne 0) { throw "light failed" }
Write-Host "installer: $msi"
