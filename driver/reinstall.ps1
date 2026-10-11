# Replaces the installed RemoteDisplay virtual monitor with the package in driver\out\pkg (test-signed).
# Run in an ADMINISTRATOR PowerShell:
#   powershell -ExecutionPolicy Bypass -File A:\projects\RemoteDisplay\driver\reinstall.ps1
$ErrorActionPreference = "Continue"
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host "Run this from an administrator PowerShell."; exit 1 }

$devcon = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
$inf = Join-Path $PSScriptRoot "out\pkg\RemoteDisplayVirtual.inf"
$hwid = "root\RemoteDisplayVirtual"
$log = "C:\ProgramData\RemoteDisplay\driver.log"

Write-Host "1/4 removing the old device"
& $devcon remove $hwid | Out-Host

Write-Host "2/4 removing old RemoteDisplay packages from the driver store"
$lines = pnputil /enum-drivers
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Original Name:\s+remotedisplayvirtual\.inf') {
        $pub = ($lines[$i - 1] -replace '.*Published Name:\s+', '').Trim()
        if ($pub -like 'oem*.inf') { pnputil /delete-driver $pub /uninstall | Out-Host }
    }
}

Write-Host "3/4 adding the new package"
pnputil /add-driver $inf | Out-Host

Write-Host "4/4 creating the device"
Remove-Item $log -ErrorAction SilentlyContinue
& $devcon install $inf $hwid | Out-Host

Start-Sleep -Seconds 8
Write-Host "`n=== device status ==="
Get-PnpDevice | Where-Object { $_.FriendlyName -eq 'RemoteDisplay Virtual Monitor' } |
    Format-List Status, Problem, InstanceId | Out-Host
Write-Host "=== driver.log ==="
if (Test-Path $log) { Get-Content $log | Select-Object -First 40 | Out-Host } else { Write-Host "(no log written)" }
