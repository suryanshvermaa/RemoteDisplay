# RemoteDisplay virtual monitor (IddCx driver)

A virtual monitor for headless PCs and remote sessions. Windows treats it as a real display. The
streaming service captures it with DXGI Desktop Duplication, the same way it captures a physical
monitor, so the driver never copies pixels.

- `src\Driver.cpp`, `src\Driver.h`: the IddCx 1.2 UMDF 2 driver (one adapter, one 1920x1080 monitor).
- `src\Edid.cpp`, `src\Edid.h`: the monitor's EDID (identity, preferred 1920x1080@60 timing, name, limits).
- `inf\RemoteDisplayVirtual.inf`: the driver package. The hardware ID is `root\RemoteDisplayVirtual`.
- `build.ps1`: compiles, links and (with `-Package`) stamps, catalogs and signs the package.

The app side is `app\virtual_monitor.h`: it creates and removes the device with SetupAPI, through the
elevated helper (`RemoteDisplay.exe --admin-ctl vm-add=<INF>` and `vm-remove`).

## Build

Needs the WDK that matches the SDK (10.0.26100). Then:

```
powershell -ExecutionPolicy Bypass -File driver\build.ps1 -Package
```

Output: `driver\out\pkg\` (DLL, INF, catalog, all signed with the test certificate `CN=RemoteDisplay Test`).

## Install (test-signed, this PC only)

These steps change the boot configuration and the machine's trust store. Run them yourself in an
administrator terminal.

1. Turn on test signing and restart:

   ```
   bcdedit /set testsigning on
   ```

2. Trust the test certificate on this PC (exported to `driver\out\RemoteDisplayTest.cer`):

   ```
   certutil -addstore Root driver\out\RemoteDisplayTest.cer
   certutil -addstore TrustedPublisher driver\out\RemoteDisplayTest.cer
   ```

3. Add the driver to the driver store:

   ```
   pnputil /add-driver driver\out\pkg\RemoteDisplayVirtual.inf /install
   ```

4. Create the monitor (from an elevated terminal, using the full path to the INF):

   ```
   RemoteDisplay.exe --admin-ctl vm-add=A:\projects\RemoteDisplay\driver\out\pkg\RemoteDisplayVirtual.inf
   ```

   If Windows asks for a restart, do it. The monitor then shows under Settings > System > Display.

## Remove

```
RemoteDisplay.exe --admin-ctl vm-remove
pnputil /delete-driver RemoteDisplayVirtual.inf /uninstall
```

## Status

- The EDID and the INF checks pass (`test-edid`, and Inf2Cat reports no errors on the INF).
- The driver compiles against the WDK headers and links with the IddCx and UMDF stubs.
- **Not yet verified on a running system.** Loading the driver, the monitor appearing, its modes,
  and frames reaching the capture path all need the install steps above, including the restart
  after test signing.

## Known gaps

- The EDID describes one 1920x1080 monitor. Other sizes come from the mode list in `Driver.cpp`.
- Frames are released as soon as Windows presents them. Nothing renders them anywhere.
- Test signing only. A shipped driver needs Microsoft attestation signing and the matching
  release checks; that is a separate step.
