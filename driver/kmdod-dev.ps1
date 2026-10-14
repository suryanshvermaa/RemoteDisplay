# DEVELOPER ONLY - NOT FOR RELEASE. Not part of the installer. Do not ship this to other people's PCs.
#
# Stages, binds, reports on and removes the Microsoft KMDOD sample display driver on THIS machine.
# While it is bound to a GPU, that GPU has no Direct3D 11, so DXGI capture (this app's capture path)
# and most GPU-accelerated apps stop working on it. A reboot is needed for the change to take effect.
#
#   powershell -ExecutionPolicy Bypass -File driver\kmdod-dev.ps1 -Action status
#   powershell -ExecutionPolicy Bypass -File driver\kmdod-dev.ps1 -Action install -InfPath <SampleDisplay.inf> -AcceptGpuLoss
#   powershell -ExecutionPolicy Bypass -File driver\kmdod-dev.ps1 -Action rollback -PublishedName oemNN.inf
#
# install and rollback need an elevated (Administrator) shell. Reboot afterwards: shutdown /r /t 0

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("status", "install", "rollback")]
    [string]$Action,

    # Signed SampleDisplay.inf (from the signed package folder).
    [string]$InfPath,

    # Matches the AMD Radeon on this PC (PCI\VEN_1002&DEV_164C). Change it for another GPU.
    [string]$HardwareId = "PCI\VEN_1002&DEV_164C",

    # The oemNN.inf name that pnputil printed when the driver was added (for rollback).
    [string]$PublishedName,

    # Required for install: confirms you accept that this GPU loses Direct3D 11 until rollback.
    [switch]$AcceptGpuLoss
)

$ErrorActionPreference = "Stop"

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-Devcon {
    $path = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Tools" -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\devcon\.exe$" } |
        Select-Object -First 1
    if (-not $path) { throw "devcon.exe (x64) not found under the Windows Kits Tools folder" }
    return $path.FullName
}

function Show-Status {
    pnputil /enum-devices /class Display /drivers 2>&1 |
        Select-String -Pattern "Instance ID|Device Description|Driver Name|Driver Status" |
        ForEach-Object { $_.Line.Trim() }
    $svc = Get-Service KDODSamp -ErrorAction SilentlyContinue
    if ($svc) { "KDODSamp service: " + $svc.Status } else { "KDODSamp service: not installed" }
}

switch ($Action) {
    "status" {
        Show-Status
    }

    "install" {
        if (-not (Test-Admin)) { throw "Run this from an elevated (Administrator) PowerShell." }
        if (-not $AcceptGpuLoss) { throw "Pass -AcceptGpuLoss to confirm: the GPU loses Direct3D 11 until rollback." }
        if (-not $InfPath -or -not (Test-Path $InfPath)) { throw "-InfPath must point to SampleDisplay.inf." }
        $pkgDir = Split-Path -Parent $InfPath
        $cat = Get-ChildItem $pkgDir -Filter *.cat | Select-Object -First 1
        if (-not $cat) { throw "No .cat file next to the INF. Sign and catalog the package first." }
        if ((Get-AuthenticodeSignature $cat.FullName).Status -ne "Valid") { throw "The catalog is not validly signed." }

        Write-Host "Staging the driver package..."
        pnputil /add-driver $InfPath 2>&1 | ForEach-Object { $_ }
        Write-Host "Binding it to $HardwareId only..."
        & (Find-Devcon) update $InfPath $HardwareId
        Write-Host ""
        Show-Status
        Write-Host ""
        Write-Host "Reboot to finish: shutdown /r /t 0"
        Write-Host "Roll back with: -Action rollback -PublishedName <oemNN.inf from the status output>"
    }

    "rollback" {
        if (-not (Test-Admin)) { throw "Run this from an elevated (Administrator) PowerShell." }
        if (-not $PublishedName) { throw "-PublishedName is required (the oemNN.inf for the sample driver)." }
        pnputil /delete-driver $PublishedName /uninstall /force 2>&1 | ForEach-Object { $_ }
        Write-Host ""
        Write-Host "Reboot to finish: shutdown /r /t 0"
        Write-Host "After the reboot, run -Action status: the AMD driver (oem14.inf) should be back."
    }
}
