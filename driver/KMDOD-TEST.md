# KMDOD sample driver on this PC: install, test and restore

**Developer only. Not for release.** This document covers a Microsoft display-only driver (the KMDOD sample) that
was bound to the AMD GPU on the development PC for testing. It must never be included in the installer or given to
other people's PCs. For other machines, use the normal RemoteDisplay installer.

## What this driver is, and what it does to the GPU

- Source: Microsoft's `video/KMDOD` sample (`microsoft/Windows-driver-samples`). Built with the WDK 10.0.26100
  command-line tools (`cl`/`link`) and signed with the existing test certificate `CN=RemoteDisplay Test`.
- It drives the boot framebuffer. It calls `DxgkCbAcquirePostDisplayOwnership`, so it only starts on the device that
  shows the boot screen.
- While it is bound to the AMD GPU (`PCI\VEN_1002&DEV_164C`):
  - the GPU has **no Direct3D 11**, so DXGI Desktop Duplication and the RemoteDisplay GPU capture and viewer paths fail;
  - the screen works at the boot-framebuffer resolution, with no 3D acceleration;
  - the README warns the driver **bugchecks (crashes) if the PC goes to sleep**.
- The RemoteDisplay app switches to its GDI capture and software decoder automatically while this is in place.

## Install (what was done)

Run from an elevated PowerShell. Files: the signed package folder (`SampleDisplay.inf`, `SampleDisplay.sys`,
`sampledisplay.cat`). The INF matches only `PCI\VEN_1002&DEV_164C` (the generic VGA and ACPI matches were removed),
so Windows can bind it to nothing else.

```bash
pnputil /add-driver "<package folder>\SampleDisplay.inf"
```

```bash
powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action install -InfPath "<package folder>\SampleDisplay.inf" -AcceptGpuLoss
```

The package folder used for the test was
`C:\Users\Suryansh\AppData\Local\Temp\claude\A--projects\996841ff-4c50-48c5-8024-30148a23d153\scratchpad\kmdod-build\pkg`.
That folder is inside Windows' temp area and may be cleared. Copy it somewhere permanent first, for example:

```bash
robocopy "<package folder>" "C:\RemoteDisplay-dev\kmdod-pkg" /E
```

Then reboot: `shutdown /r /t 0`.

## Current state on the test PC (when this was written)

- `oem33.inf` (the sample) is bound to the AMD Radeon device, status `Started`.
- The `KDODSamp` service is `Running`.
- The original AMD driver is still in the store: `oem14.inf` (`u0387389.inf`) with extension `oem13.inf`
  (`amdwddme.inf`). Windows reinstalls it when the sample is removed.

## Restore the original AMD driver

Run every command in an **elevated** PowerShell.

1. **Check the current state:**

   ```bash
   powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action status
   ```

2. **Remove the sample driver package and its binding:**

   ```bash
   powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action rollback -PublishedName oem33.inf
   ```

   This runs `pnputil /delete-driver oem33.inf /uninstall /force`.

3. **Reboot:**

   ```bash
   shutdown /r /t 0
   ```

4. **Check the restore.** After the reboot, run:

   ```bash
   powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action status
   ```

   Expected: the AMD device's `Driver Name` is `oem14.inf` (the `Driver Status` of `oem33.inf` is no longer
   `Started`), and `KDODSamp service: not installed`.

   Then check the GPU is back, with the 3D capability:

   ```bash
   powershell -Command "Get-CimInstance Win32_VideoController | Select-Object Name,Status | Format-List"
   ```

   Expected: `Name : AMD Radeon(TM) Graphics`, `Status : OK`.

   Optionally check whether the driver file is still on disk:

   ```bash
   powershell -Command "Test-Path C:\Windows\System32\drivers\SampleDisplay.sys"
   ```

   `False` is expected. If it prints `True`, the file was left behind. It is not loaded once the package is removed,
   so it is harmless, and it can be deleted from an elevated shell with `del C:\Windows\System32\drivers\SampleDisplay.sys`.

## If the restore does not work

**The uninstall says the driver is in use, or the screen is black after the reboot:**

1. Boot into Safe Mode: Settings > System > Recovery > Advanced startup > Restart now > Troubleshoot > Advanced
   options > Startup Settings > Restart, then press `4`.
2. In Safe Mode, run the rollback command from step 2 above, then reboot normally.

**The AMD driver does not come back after the reboot.** Rebind it from its store package, in an elevated PowerShell:

```bash
pnputil /scan-devices
```

If that does not bind it, find the AMD INF in the driver store and point `devcon` at it:

```bash
powershell -Command "Get-ChildItem C:\Windows\System32\DriverStore\FileRepository -Recurse -Filter u0387389.inf | Select-Object -First 1 -ExpandProperty FullName"
```

```bash
& "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe" update "<the path printed above>" "PCI\VEN_1002&DEV_164C"
```

Then reboot.

**Last resort:** System Restore is turned off on this PC, so there is no restore point. A full Windows reinstall
was accepted as the fallback.

## Things that stay after the restore (on purpose)

- **Test signing** stays on (`bcdedit /enum` shows `testsigning Yes`). The RemoteDisplay virtual monitor driver also
  needs it. Turn it off only if you no longer use that driver: `bcdedit /set testsigning off`, then reboot.
- **The test certificate** `CN=RemoteDisplay Test` stays in the Local Machine Trusted Root and Trusted Publisher stores.

## Reinstall later

1. Copy the package to a permanent folder (see Install).
2. Run the install commands above, then reboot.
3. To remove it again, use the restore steps.

## What to expect in the app while the sample driver is bound

- Sharing uses the GDI path (`capture: GDI, whole desktop`). It works, at about 11 to 12 frames per second, and
  captures the whole virtual desktop.
- The viewer on the same PC uses the software decoder (`software video (no Direct3D 11 on this GPU)`).
- Secure-desktop (UAC and sign-in screen) pictures are not available in this mode.
- Do not put the PC to sleep while the sample is bound.

## Files

- `driver/kmdod-dev.ps1`: the status, install and rollback script used above.
- `driver/KMDOD-TEST.md`: this document.
- The package and the sample source are not in the repository. The package is only in the scratchpad folder named above.
