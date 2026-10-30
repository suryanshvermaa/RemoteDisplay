# Builds installer\out\RemoteDisplayClientSetup.msi (the viewer only) and installer\out\RemoteDisplayClient.exe
# (the same program as a plain file, which runs without installing).
#   powershell -ExecutionPolicy Bypass -File installer\Build-Client.ps1 [-Version 1.2.3.0]
# Build the client first:  cmake --build build --config Release --target rd-client
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\Release\RemoteDisplayClient.exe"
$stage = Join-Path $PSScriptRoot "stage-client"
$out = Join-Path $PSScriptRoot "out"

if (-not (Test-Path $exe)) { throw "missing $exe (build the rd-client target first)" }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, $out | Out-Null
Copy-Item $exe $stage
Copy-Item $exe $out -Force

$wixBin = if ($env:WIX) { Join-Path $env:WIX "bin" } else { "C:\Program Files (x86)\WiX Toolset v3.14\bin" }
if (-not (Test-Path (Join-Path $wixBin "candle.exe"))) { $wixBin = "C:\Program Files (x86)\WiX Toolset v3.11\bin" }
$candle = Join-Path $wixBin "candle.exe"
$light = Join-Path $wixBin "light.exe"
if (-not (Test-Path $candle)) { throw "WiX 3 not found (winget install WiXToolset.WiXToolset --version 3.14.1.8722)" }

$obj = Join-Path $PSScriptRoot "obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$wxs = Join-Path $PSScriptRoot "Client.wxs"
if ($Version) {  # a version given here (the CI passes the release tag) replaces the one in the .wxs
    $tmp = Join-Path $obj "Client.versioned.wxs"
    (Get-Content $wxs -Raw) -replace '(?s)(<Product\b.*?\bVersion=")[0-9.]+(")', "`${1}$Version`$2" | Set-Content $tmp -Encoding UTF8
    $wxs = $tmp
}
$wixobj = Join-Path $obj "Client.wixobj"
& $candle -nologo -arch x64 "-dStageDir=$stage" -out $wixobj $wxs
if ($LASTEXITCODE -ne 0) { throw "candle failed" }

$msi = Join-Path $out "RemoteDisplayClientSetup.msi"
& $light -nologo -out $msi $wixobj
if ($LASTEXITCODE -ne 0) { throw "light failed" }
Write-Host "client installer: $msi"
Write-Host "client program:   $(Join-Path $out 'RemoteDisplayClient.exe')"
