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

## Use it for remote sessions

The streaming app captures the first screen by default. To stream the virtual monitor instead, set
this in `%APPDATA%\RemoteDisplay\config.ini`:

```
[host]
display=virtual
```

With the virtual monitor extended (Win+P, Extend), the app finds the display whose monitor is named
`RD Virtual` and captures it. If there is no such display, it falls back to the first screen and says
so in the log. `build\Release\test-display.exe` checks the same path: it reports the display it found,
the size, and the frames it gets.

## Duplicate and Extend while sharing

The sharing card has two choices. They are mutually exclusive, and neither one is set by default.

- **Duplicate displays while sharing**: while sharing, Windows shows the same picture on every screen.
  The layout you had before sharing is saved and put back when sharing stops. If the app was killed
  mid-session, the saved layout is put back the next time the app or the worker starts.
- **Extend: stream virtual monitor**: the screens stay extended, and the streamed picture is the virtual
  monitor.

The choice is stored as `display_mode` under `[host]` in `config.ini` (`duplicate`, `extend`, or empty).
Tested here: ticking Duplicate mirrors the screens while sharing (one display in the layout), stopping
restores both, and the restore also happens at the next start after a kill.
