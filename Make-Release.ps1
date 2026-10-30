# Builds everything for a release on this PC and puts the files in release\v<version>\.
# It does not upload or publish anything: you create the GitHub release yourself with the files it prints.
#
#   powershell -ExecutionPolicy Bypass -File Make-Release.ps1 -Version 1.2.0
#
# Makes: the Windows app installer (with the optional virtual monitor driver when driver\out\pkg exists), the
# viewer-only client installer and plain .exe, the Android APK, and SHA256SUMS.txt.
# Needs: Visual Studio build tools + CMake, WiX 3.14, and for Android the toolchain in
# %LOCALAPPDATA%\RemoteDisplayAndroid (JDK 17, SDK) or JAVA_HOME and ANDROID_HOME set yourself.
# For a release-signed APK set ANDROID_KEYSTORE_FILE, ANDROID_KEYSTORE_PASSWORD, ANDROID_KEY_ALIAS and
# ANDROID_KEY_PASSWORD first; without them the APK is debug-signed, which installs fine for sideloading.
param(
    [Parameter(Mandatory = $true)][string]$Version,   # e.g. 1.2.0
    [switch]$SkipAndroid,
    [switch]$SkipTests
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$Version = $Version.TrimStart('v')
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "Version '$Version' must look like 1.2.3" }
$parts = $Version.Split('.') + @('0')
$msiVersion = ($parts[0..3]) -join '.'   # an MSI version has exactly four numbers
$dist = Join-Path $PSScriptRoot "release\v$Version"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

function Step($t) { Write-Host "`n== $t" -ForegroundColor Cyan }

Step "Build the Windows programs"
if (-not (Test-Path build)) { cmake -S . -B build -G "Visual Studio 18 2026" -A x64 }
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if (-not $SkipTests) {
    Step "Unit tests"
    $failed = @()
    foreach ($t in "test-edid", "test-session", "test-capture-layout", "test-driver-store", "test-gdi-path", "test-secure", "test-admin") {
        & ".\build\Release\$t.exe" | Out-Null
        if ($LASTEXITCODE -ne 0) { $failed += $t }
    }
    if ($failed) { throw "tests failed: $($failed -join ', ')" }
    Write-Host "all passed"
}

Step "Windows installers (version $msiVersion)"
powershell -ExecutionPolicy Bypass -File installer\Build-Installer.ps1 -Version $msiVersion
if ($LASTEXITCODE -ne 0) { throw "app installer failed" }
powershell -ExecutionPolicy Bypass -File installer\Build-Client.ps1 -Version $msiVersion
if ($LASTEXITCODE -ne 0) { throw "client installer failed" }
Copy-Item installer\out\RemoteDisplaySetup.msi       "$dist\RemoteDisplay-Setup-v$Version.msi"
Copy-Item installer\out\RemoteDisplayClientSetup.msi "$dist\RemoteDisplay-Client-Setup-v$Version.msi"
Copy-Item installer\out\RemoteDisplayClient.exe      "$dist\RemoteDisplay-Client-v$Version.exe"

if (-not $SkipAndroid) {
    Step "Android APK"
    $tool = Join-Path $env:LOCALAPPDATA "RemoteDisplayAndroid"
    if (-not $env:JAVA_HOME -and (Test-Path "$tool\jdk")) { $env:JAVA_HOME = (Get-ChildItem "$tool\jdk" -Directory | Select-Object -First 1).FullName }
    if (-not (Test-Path "android\local.properties") -and -not $env:ANDROID_HOME -and (Test-Path "$tool\sdk")) {
        $sdk = ("$tool\sdk" -replace '\\', '/') -replace ':', '\:'
        "sdk.dir=$sdk" | Set-Content "android\local.properties" -Encoding ASCII
    }
    $code = [int]$parts[0] * 1000000 + [int]$parts[1] * 1000 + [int]$parts[2]
    $signed = [bool]$env:ANDROID_KEYSTORE_FILE
    Push-Location android
    try {
        & .\gradlew.bat testDebugUnitTest $(if ($signed) { "assembleRelease" } else { "assembleDebug" }) "-PversionName=$Version" "-PversionCode=$code" --console=plain
        if ($LASTEXITCODE -ne 0) { throw "Android build failed" }
    } finally { Pop-Location }
    $apk = if ($signed) { "android\app\build\outputs\apk\release\app-release.apk" } else { "android\app\build\outputs\apk\debug\app-debug.apk" }
    Copy-Item $apk "$dist\RemoteDisplay-Android-v$Version.apk"
}

Step "Checksums"
Push-Location $dist
Get-ChildItem -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content SHA256SUMS.txt -Encoding ASCII
Get-Content SHA256SUMS.txt
Pop-Location

Write-Host "`nDone. Files are in $dist" -ForegroundColor Green
Write-Host "To publish: open your releases page on GitHub > Draft a new release > tag v$Version > attach these files."
Write-Host "Or with the GitHub CLI (this uploads, so run it yourself when you are ready):"
Write-Host "  gh release create v$Version `"$dist\*`" --repo <owner>/<repo> --title `"RemoteDisplay v$Version`" --draft"
