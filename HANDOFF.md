# RemoteDisplay: handoff summary

Snapshot of the project and of the work done with Claude, so it can be continued in a new chat or on another
machine. Paste this file into a new Claude chat to give it the context.

**Important:** RemoteDisplay is a Windows-only C++ app (Win32, Direct3D 11, Media Foundation, WDK driver, WiX
installer). It builds and runs only on Windows. On a Mac you can read, plan and edit code, but building,
testing and installing must happen on the Windows PC.

## What the app is

A Windows remote desktop and screen-sharing app:

- **Host** (`RemoteDisplay.exe`): shares the screen. DXGI Desktop Duplication captures every screen on the GPU
  into one picture, then a hardware H.264 encoder (Media Foundation) compresses it, and the stream goes over UDP
  port 47800, encrypted with AES-128-GCM.
  - Pairing uses a 26-character key: ECDH P-256 handshake plus a PSK HMAC.
  - Each device has an ECDSA identity, and there is an "Always allow this device" trust list.
  - An approval prompt asks the host user before a new device connects.
- **Viewer / client** (`RemoteDisplayClient.exe`): a standalone viewer. It has no host, service or driver parts
  and needs no admin rights.
- **Windows service** (`RemoteDisplayHost`): starts a headless host worker (`--host-worker`) in the user's
  session, so the PC can be shared in the background, starting with Windows.
- **Optional virtual monitor driver** (IddCx, UMDF 2.15, `driver/`): adds a second virtual screen for the
  "Extend" mode. It is test-signed only.
- **Secure desktop helper:** with the admin's consent, viewers can see the UAC and sign-in screens. A SYSTEM
  helper captures them, and every use is audited.

## Repository layout (A:\projects\RemoteDisplay)

- `app/`
  - `main.cpp`: GUI app.
  - `worker.h`: headless host.
  - `service.h`: Windows service.
  - `admin.h`: elevated helper commands (`--admin-ctl`, `--service-ctl`).
  - `common.h`: config file at `%APPDATA%\RemoteDisplay\config.ini`.
  - `discovery.h`: LAN search.
  - `ui_theme.h`: theme and custom-drawn controls.
  - `virtual_monitor.h`, `display_mode.h`: virtual monitor and Duplicate/Extend.
  - `secure*.h`: secure desktop helper.
- `service/src/`
  - `host.h`: host loop.
  - `capture.h`: DXGI multi-screen capture.
  - `capture_layout.h`: screen layout math.
  - `encoder.h`: hardware H.264.
  - `gdi_capture.h`, `soft_encoder.h`: fallback when the GPU has no Direct3D 11.
  - `inject.h`: mouse and keyboard input.
  - `main.cpp`: `rd-host` CLI.
- `service/include/`: `protocol.h` (packets, port 47800, discovery `RDQ1`/`RDA1`), `session.h`, `identity.h`.
- `client/src/`
  - `main.cpp`: client connect window.
  - `viewer.h`: viewer, with GPU decode and a software decode + GDI fallback.
- `driver/`
  - IddCx driver source and `build.ps1`.
  - `kmdod-dev.ps1`, `KMDOD-TEST.md`, `KMDOD-LOAD.md`: developer-only notes on the Microsoft KMDOD sample
    driver. Not for release.
- `installer/`
  - `RemoteDisplay.wxs` + `Build-Installer.ps1`: app MSI, version 1.0.0.4.
  - `Client.wxs` + `Build-Client.ps1`: client-only MSI, version 1.0.0.0.
  - Output goes to `installer/out/`.
- `tests/`: C++ unit tests and PowerShell UI end-to-end scripts.
- Separate public folder `A:\projects\RemoteDisplay-releases`: README and `downloads/`, for publishing releases.

## Build and test (on Windows)

```bash
cmake --build build --config Release
```

```bash
powershell -ExecutionPolicy Bypass -File installer\Build-Installer.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File installer\Build-Client.ps1
```

Toolchain: Visual Studio 18 (MSVC 14.51), CMake 3.20 or later, WDK 10.0.26100, WiX 3.14 (candle/light). The
client is linked with the static C++ runtime.

**Unit tests** (in `build\Release`):
- `test-edid`, `test-session`, `test-secure`, `test-capture-layout`, `test-driver-store`, `test-gdi-path`,
  `test-admin`: all pass.
- `test-input <key> <port>`: needs a running `rd-host`. Passes.
- `test-display`, `test-display-mode`: skip unless a second display is present.

**Manual host for tests:** `RemoteDisplay.exe --host-worker --port <n> --no-approval --key <key>`. These are
test-only options; the service never passes them. `rd-host --port <n> --key <key>` listens on 127.0.0.1 only.

**Not run yet:** the UI end-to-end scripts (`gui_e2e`, `trust_e2e`, `background_e2e`, `tray_hidden_e2e`,
`secure_input_gate_e2e`, `viewer_input_e2e`). They move the mouse and type on the desktop, so run them while at
the PC. The UAC test is manual: see `tests/secure_desktop_manual.md`.

## Work done in this chat (most recent last)

1. **LAN discovery:** the host answers UDP broadcast `RDQ1` with `RDA1` plus the computer name.
   - App: "Find devices on this network" button, plus a background search every 15 seconds.
   - Client: a "PCs found on this network" list and a Find button.
2. **Multi-screen capture:** every screen on the GPU goes into one picture, mirrored screens are kept once, and
   the picture is scaled to at most 3840x2160. It works only for screens on one GPU adapter.
3. **KMDOD experiment:** the Microsoft display-only sample driver was built, signed and bound to the AMD GPU.
   - Result: it removes Direct3D 11 from the GPU and does not mirror screens, so it was not adopted.
   - It was restored afterwards; the AMD driver is `oem14.inf`.
4. **GDI + software H.264 fallback:** the host uses it automatically when Direct3D 11 is missing. Set
   `[host] capture=auto|dxgi|gdi` in `config.ini` to force a path. It runs at about 11 fps and has no
   UAC/sign-in picture.
5. **Separate client app and client-only MSI.**
6. **Installer 1.0.0.4:** the virtual monitor driver is now an optional MSI feature and is off by default.
   - Install with the driver: `msiexec /i RemoteDisplaySetup.msi ADDLOCAL=Main,VirtualMonitor`.
   - Remove only the driver: `msiexec /i RemoteDisplaySetup.msi REMOVE=VirtualMonitor`.
   - `driver-install` no longer adds the same driver version to the driver store twice.
7. **GitHub Actions CI:** added, then removed at the user's request. Releases are published by hand only.
8. **Modern UI with dark mode** (`app/ui_theme.h`):
   - Theme button: Auto, Light, Dark. The choice is saved as `[app] theme` and shared with the client.
   - GDI+ anti-aliased cards, toggle switches in place of checkboxes, rounded fields and drop-downs, and status
     pills.
   - The window fits a 1080p screen at 150% scaling.
   - The title bar follows the theme.

## Current state

- Everything builds with 0 errors and 0 warnings, and the MSIs in `installer/out` are current.
- **Not committed:** many files are changed or new. Nothing has been pushed.
- The private repo holds the source. Public releases go to the separate releases repo. The user removed the
  installed `RemoteDisplayHost` service on purpose.
- Test-signing mode is ON on the dev PC, because the virtual monitor driver needs it.

## Open items / ideas

- Restyle the remaining dialogs to match the theme: Trusted devices, the approval prompt, message boxes.
- Commit the work in logical commits. Don't push unless asked.
- Before a public release:
  - choose a license (the releases README has an "All rights reserved" placeholder);
  - get code signing for the exe and MSIs, so antivirus and SmartScreen don't warn;
  - get attestation signing for the virtual monitor driver, without which it only works in test-signing mode.
- Feature ideas:
  - clipboard sync;
  - adaptive bitrate and loss handling;
  - connecting over the internet (UPnP, then a relay);
  - audio;
  - file transfer;
  - a screen picker in the viewer;
  - multi-GPU capture.

## Working preferences (from this chat)

- Ask before outward actions: pushing, publishing, installing drivers. Commands that need admin are run by the
  user in an elevated PowerShell, so give full absolute paths.
- Developer-only KMDOD files stay in the private repo. Don't add them to `.gitignore`.
- Release the app (host + viewer) and the viewer-only client as separate release downloads.
