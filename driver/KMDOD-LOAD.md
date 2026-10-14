# Load the KMDOD sample driver (developer PC only)

**Developer only. Not for release.** This loads Microsoft's KMDOD sample display driver on the AMD GPU of this PC.
While it is loaded the GPU has no Direct3D 11, and the PC crashes if it goes to sleep. Read `KMDOD-TEST.md` first.

Run every command below in an **elevated** PowerShell (Run as administrator). Run them in order.

## 1. Remove the RemoteDisplay virtual monitor, if it is present

Skip this step only if you want the virtual monitor and the sample driver active at the same time, which is not
supported.

```bash
"A:\projects\RemoteDisplay\build\Release\RemoteDisplay.exe" --admin-ctl driver-purge
```

## 2. Copy the signed package to a permanent folder

The package was first built in the Windows temp folder, which can be cleared. Copy it once:

```bash
robocopy "C:\Users\Suryansh\AppData\Local\Temp\claude\A--projects\996841ff-4c50-48c5-8024-30148a23d153\scratchpad\kmdod-build\pkg" "C:\RemoteDisplay-dev\kmdod-pkg" /E
```

`robocopy` exit codes `0` to `7` are success. Anything from `8` up is an error.

## 3. Check that the package is there

This must print `True`. If it prints `False`, do not continue: the install script stops without it.

```bash
Test-Path "C:\RemoteDisplay-dev\kmdod-pkg\SampleDisplay.inf"
```

## 4. Check that test signing is on

The output must contain `testsigning Yes`:

```bash
bcdedit /enum | findstr /i testsigning
```

## 5. Install the sample and bind it to the AMD GPU

The script checks that the catalog is validly signed, and it refuses to run without `-AcceptGpuLoss`:

```bash
powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action install -InfPath "C:\RemoteDisplay-dev\kmdod-pkg\SampleDisplay.inf" -AcceptGpuLoss
```

## 6. Reboot

The driver takes effect only after a restart:

```bash
shutdown /r /t 0
```

## 7. Check that it loaded

After the reboot, run:

```bash
powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action status
```

Expected:
- the AMD device shows `Device Description: Kernel mode display only sample driver`, with a `Driver Name: oem…inf`;
- `KDODSamp service: Running`;
- no `RemoteDisplay Virtual Monitor` device, if step 1 was done.

## Restore the AMD driver

When you no longer want the sample loaded, run the rollback with the `oemNN.inf` name from step 7, then reboot:

```bash
powershell -ExecutionPolicy Bypass -File "A:\projects\RemoteDisplay\driver\kmdod-dev.ps1" -Action rollback -PublishedName oemNN.inf
```

```bash
shutdown /r /t 0
```

Full restore steps, including Safe Mode recovery, are in `KMDOD-TEST.md`.

## Troubleshooting

- **`-InfPath must point to SampleDisplay.inf`**: step 2 did not run, or its `robocopy` source path was wrong. Run steps 2 and 3 again.
- **Install reports a catalog signature error**: the package was changed after signing. Copy it again from the scratchpad folder, or rebuild and re-sign it.
- **Screen is black after the reboot**: boot into Safe Mode and run the rollback command above.
