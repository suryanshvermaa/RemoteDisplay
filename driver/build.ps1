# Builds the RemoteDisplay virtual monitor driver (IddCx 1.2, UMDF 2.33) with the WDK on this machine.
#   powershell -ExecutionPolicy Bypass -File driver\build.ps1            # compile and link
#   powershell -ExecutionPolicy Bypass -File driver\build.ps1 -Package   # also stamp, catalog and sign
# Outputs go to driver\out\. Installing the package is a separate step (see driver\README.md).
param(
    [switch]$Package,
    [string]$TestCertThumbprint = ""
)
$ErrorActionPreference = "Stop"

$wdk = "C:\Program Files (x86)\Windows Kits\10"
$sdk = "10.0.26100.0"
# UMDF framework version. The OS must provide it: this PC's Windows only has WUDFx02000 and rejects 2.33
# (the WUDF coinstaller fails with error 87). Keep this, the headers, the stub and the INF in step.
$wdfVer = "2.15"
$wdfMinor = $wdfVer.Split(".")[1]
$msvc = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$cl = "$msvc\bin\Hostx64\x64\cl.exe"
$link = "$msvc\bin\Hostx64\x64\link.exe"

$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "driver\src"
$out = Join-Path $root "driver\out"
New-Item -ItemType Directory -Force $out | Out-Null

# Headers. Order matters: the UMDF WDF headers come from the wdf folder, IddCx from its version folder.
$incs = @(
    "$msvc\include",
    "$wdk\Include\$sdk\um",
    "$wdk\Include\$sdk\shared",
    "$wdk\Include\$sdk\ucrt",
    "$wdk\Include\$sdk\winrt",
    "$wdk\Include\$sdk\um\iddcx\1.2",
    "$wdk\Include\wdf\umdf\$wdfVer"
)
$cflags = @(
    "/nologo", "/c", "/EHsc", "/std:c++20", "/W3", "/WX", "/MT",
    "/D", "UNICODE", "/D", "_UNICODE",
    "/D", "_WIN32_WINNT=0x0A00", "/D", "WINVER=0x0A00", "/D", "NTDDI_VERSION=0x0A000008",
    "/D", "UMDF_USING_NTSTATUS", "/D", "UMDF_VERSION_MAJOR=2", "/D", "UMDF_VERSION_MINOR=$wdfMinor"
)
foreach ($i in $incs) { $cflags += @("/I", $i) }

$objs = @()
foreach ($f in @("Driver.cpp", "Edid.cpp")) {
    $obj = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($f) + ".obj")
    & $cl @cflags "/Fo$obj" (Join-Path $src $f)
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $f" }
    $objs += $obj
}

# Libraries: the UMDF stub, the IddCx stub, and the D3D/DXGI import libraries.
$libs = @(
    "$wdk\Lib\wdf\umdf\x64\$wdfVer\WdfDriverStubUm.lib",
    "$wdk\Lib\$sdk\um\x64\iddcx\1.2\iddcxstub.lib",
    "$wdk\Lib\$sdk\um\x64\d3d11.lib",
    "$wdk\Lib\$sdk\um\x64\dxgi.lib",
    "$wdk\Lib\$sdk\um\x64\uuid.lib",
    "$wdk\Lib\$sdk\um\x64\ntdll.lib"
)
$dll = Join-Path $out "RemoteDisplayVirtual.dll"
$lflags = @(
    "/nologo", "/DLL", "/SUBSYSTEM:WINDOWS", "/MANIFEST:NO", "/OUT:$dll", "/EXPORT:DriverEntry",
    "/LIBPATH:$msvc\lib\x64", "/LIBPATH:$wdk\Lib\$sdk\ucrt\x64", "/LIBPATH:$wdk\Lib\$sdk\um\x64"
)
& $link @lflags @objs @libs
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Write-Host "built $dll"

if ($Package) {
    # The package: DLL, INF and catalog, as Windows expects them in a driver folder.
    $pkg = Join-Path $out "pkg"
    Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $pkg | Out-Null
    Copy-Item $dll (Join-Path $pkg "RemoteDisplayVirtual.dll")
    # The INF names the UMDF version it was built for.
    # The coinstaller reads the version as major.minor.build (the in-box drivers write e.g. 1.11.0); "2.15" alone is rejected.
    (Get-Content (Join-Path $root "driver\inf\RemoteDisplayVirtual.inf") -Raw).Replace('$UMDFVERSION$', "$wdfVer.0") |
        Set-Content -Encoding ascii (Join-Path $pkg "RemoteDisplayVirtual.inf")

    $stampinf = "$wdk\bin\$sdk\x64\stampinf.exe"
    $inf2cat = "$wdk\bin\$sdk\x86\Inf2Cat.exe"
    $signtool = "$wdk\bin\$sdk\x64\signtool.exe"

    & $stampinf -f (Join-Path $pkg "RemoteDisplayVirtual.inf") -d "*" -a amd64 -v "1.0.0.0"
    if ($LASTEXITCODE -ne 0) { throw "stampinf failed" }
    & $inf2cat "/driver:$pkg" "/os:10_X64"
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed (check the INF with the message above)" }

    # A test certificate in the current user's store. It is not trusted by Windows until you add it
    # to the machine's trusted publishers (a separate, administrator step; see driver\README.md).
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=RemoteDisplay Test" } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=RemoteDisplay Test" `
            -CertStoreLocation "Cert:\CurrentUser\My" -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(3)
    }
    foreach ($f in @((Join-Path $pkg "RemoteDisplayVirtual.dll"), (Join-Path $pkg "RemoteDisplayVirtual.cat"))) {
        & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $f
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f" }
    }
    Write-Host "package ready: $pkg"
}
