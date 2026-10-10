# RemoteDisplay

Low-latency, end-to-end encrypted remote desktop for Windows, written in C++.

![Sharing](docs/ui-sharing.png)

## Use it

Build (Visual Studio 2022+ with the C++ desktop workload, CMake 3.20+):

```
cmake -S . -B build
cmake --build build --config Release
```

Run `build\Release\RemoteDisplay.exe` on both PCs.

**On the PC you want to view:** click **Start sharing**. Give the other person your address and
the pairing key shown in the window (Copy buttons are there). The first time, Windows Firewall will
ask to allow the app on private networks: allow it.

**On the PC you are sitting at:** type the other PC's address and pairing key, click **Connect**. A
viewer window opens (F11 toggles fullscreen). Recent connections are remembered.

The sharing PC gets an "Allow this connection?" prompt for each new client (can be turned off), a
Disconnect button, and the pairing key can be regenerated at any time.

Command-line tools for testing and headless use: `rd-host.exe` and `rd-client.exe`.

## Run in the background (Windows service)

In the **Background service** section of the sharing card, click **Install** (Windows asks for
administrator approval). That installs `RemoteDisplayHost`, which starts with Windows, adds a
Windows Firewall rule for UDP 47800 on private/domain networks, and keeps this PC shareable
without opening the app. **Remove** uninstalls it and deletes the rule.

How it works: screen capture cannot run inside a service (Session 0 has no desktop), so the service
only supervises. It launches `RemoteDisplay.exe --host-worker` into the signed-in user's session as
that user, and relaunches it after a crash or when the active user changes. The worker shows the
"Allow this connection?" prompt on your screen; the app shows its live status and can disconnect it.
Changing the key or the approval setting restarts the worker automatically.

Limits: sharing starts once a user is signed in (not at the login screen), a locked screen or UAC
prompt cannot be viewed yet, and if you move `RemoteDisplay.exe` you must remove and reinstall.
Logs: `%ProgramData%\RemoteDisplay\service.log` and `%APPDATA%\RemoteDisplay\worker.log`.

## Security model

- Every UDP datagram is AES-128-GCM encrypted and authenticated; replays are rejected.
- Key exchange is ephemeral ECDH P-256, bound to the 128-bit pairing key, so a wrong key never
  gets a session and a passive eavesdropper learns nothing.
- Keys are stored with Windows DPAPI (readable only by your Windows user).
- Approving each new connection on the sharing PC is on by default. For unattended access, untick
  it; then anyone with your pairing key can view this PC, so keep the key private.
- The service runs as LocalSystem but only launches the worker as your normal user; it never
  captures or streams anything itself.

## Current limits

- **View only**: no mouse/keyboard control yet.
- **Same network or a reachable address**: no relay or NAT traversal yet, so over the internet you
  need a port-forward (UDP 47800) or a VPN such as Tailscale/WireGuard.
- One viewer at a time; primary monitor only; no audio or clipboard.
- Needs a GPU with a hardware H.264 encoder (NVIDIA, AMD or Intel).
- Windows may omit windows that apps mark as capture-protected from the shared screen.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the pipeline and roadmap.
