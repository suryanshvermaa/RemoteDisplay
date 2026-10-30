# Builds installer\out\RemoteDisplaySetup.msi from the Release build.
#   powershell -ExecutionPolicy Bypass -File installer\Build-Installer.ps1
# Needs WiX 3.14 (candle/light). Build the app first:
#   cmake --build build --config Release --target RemoteDisplay
# The virtual monitor driver is included when its signed package exists in driver\out\pkg
# (powershell -File driver\build.ps1 -Package). Without it, the MSI is the app and service only.
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\Release\RemoteDisplay.exe"
$pkg = Join-Path $root "driver\out\pkg"
$stage = Join-Path $PSScriptRoot "stage"
$out = Join-Path $PSScriptRoot "out"

if (-not (Test-Path $exe)) { throw "missing $exe (build the RemoteDisplay target first)" }
$driverFiles = "RemoteDisplayVirtual.inf", "RemoteDisplayVirtual.cat", "RemoteDisplayVirtual.dll"
$withDriver = -not ($driverFiles | Where-Object { -not (Test-Path (Join-Path $pkg $_)) })

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "driver"), $out | Out-Null
Copy-Item $exe $stage
if ($withDriver) { Copy-Item (Join-Path $pkg "*") (Join-Path $stage "driver") -Force }

$wixBin = if ($env:WIX) { Join-Path $env:WIX "bin" } else { "C:\Program Files (x86)\WiX Toolset v3.14\bin" }
if (-not (Test-Path (Join-Path $wixBin "candle.exe"))) { $wixBin = "C:\Program Files (x86)\WiX Toolset v3.11\bin" }
$candle = Join-Path $wixBin "candle.exe"
$light = Join-Path $wixBin "light.exe"
if (-not (Test-Path $candle)) { throw "WiX 3 not found (winget install WiXToolset.WiXToolset --version 3.14.1.8722)" }

# A version given on the command line replaces the one in the .wxs.
$wxs = Join-Path $PSScriptRoot "RemoteDisplay.wxs"
if ($Version) {
    $tmp = Join-Path $PSScriptRoot "obj\RemoteDisplay.versioned.wxs"
    New-Item -ItemType Directory -Force (Split-Path $tmp) | Out-Null
    # Only the <Product ... Version="..."> attribute (not the XML declaration's version="1.0").
    (Get-Content $wxs -Raw) -replace '(?s)(<Product\b.*?\bVersion=")[0-9.]+(")', "`${1}$Version`$2" | Set-Content $tmp -Encoding UTF8
    $wxs = $tmp
}

$obj = Join-Path $PSScriptRoot "obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$wixobj = Join-Path $obj "RemoteDisplay.wixobj"
$defs = @("-dStageDir=$stage")
if ($withDriver) { $defs += "-dWithDriver=1" }
& $candle -nologo -arch x64 @defs -out $wixobj $wxs
if ($LASTEXITCODE -ne 0) { throw "candle failed" }

$msi = Join-Path $out "RemoteDisplaySetup.msi"
& $light -nologo -out $msi $wixobj
if ($LASTEXITCODE -ne 0) { throw "light failed" }
Write-Host ("installer: {0} ({1})" -f $msi, $(if ($withDriver) { "with the virtual monitor driver" } else { "without the driver" }))
