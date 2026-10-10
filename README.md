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

**Controlling the remote PC:** click inside the viewer and use your mouse, wheel and keyboard as
normal. The cursor you see is the remote PC's own cursor shape. The Windows key, Alt+Tab, Alt+Esc
and Ctrl+Esc go to the remote PC while the viewer is focused. Keys are sent as hardware scancodes,
so the remote PC's keyboard layout decides the characters. Keys and mouse buttons are released on
the remote PC if the viewer loses focus, closes or disconnects. Because you can open Windows
Settings and other apps this way, you can change the remote PC's settings.

The sharing PC gets an "Allow this connection?" prompt for each new client (can be turned off), a
Disconnect button, an **Allow mouse and keyboard control** switch (off = view only), and the
pairing key can be regenerated at any time. The status line says "viewing" or "controlling".

Command-line tools for testing and headless use: `rd-host.exe` and `rd-client.exe`.

## Run it hidden in the background

There are three ways to run RemoteDisplay on the PC you want to share. Pick one:

| | Window | Admin needed | Starts | Best for |
|---|---|---|---|---|
| **Normal app** | yes | no | when you open it | trying it out |
| **Start with Windows** | hidden; tray icon optional | no | when you sign in | most people, like Parsec/AnyDesk |
| **Windows service** | none | once, to install | when Windows boots (shares once you are signed in) | always-on PCs |

**Start with Windows (recommended):** tick **Start with Windows** under *Run in background*. It
adds a per-user startup entry (`RemoteDisplay.exe --background`), so after you sign in the app starts
with **no window on screen**, begins sharing, and puts a small icon in the system tray (grey = not
sharing, green = sharing, amber = someone connecting or connected). Click the icon to open the window,
right-click for *Start/Stop sharing, Disconnect viewer, Exit*. Closing the window while sharing just
hides it to the tray. Only one copy runs per user: launching it again brings the running one forward.
Untick the box (or Settings > Apps > Startup) to undo it.

**Fully invisible (no window, no tray icon):** untick **Show tray icon** (it asks you to confirm).
RemoteDisplay then runs with nothing at all on screen, like a game-streaming host. To get back in,
launch RemoteDisplay again from the Start menu or a shortcut: it opens the running instance instead
of starting a second one. To stop it, open it and click **Stop sharing**, or close its window and
choose *No: stop sharing and exit*. While the icon is hidden, closing the window while sharing always
asks first, so it never keeps sharing invisibly by accident. The "Allow this connection?" prompt
still appears when someone connects (unless you turned it off), so with approval on you are never
connected to without seeing it. With approval off (unattended access) there is no on-screen sign at
all, so keep the pairing key private.

### Windows service (optional, needs admin)

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

- **Elevated windows**: input cannot reach admin (UAC-elevated) windows, UAC prompts, the lock screen
  or Ctrl+Alt+Del; that needs a SYSTEM-level helper (not built yet).
- No clipboard or file transfer, no audio; characters typed with an IME or dead keys are not
  forwarded as text (keys are sent by scancode).
- **Same network or a reachable address**: no relay or NAT traversal yet, so over the internet you
  need a port-forward (UDP 47800) or a VPN such as Tailscale/WireGuard.
- One viewer at a time; primary monitor only; no audio or clipboard.
- Needs a GPU with a hardware H.264 encoder (NVIDIA, AMD or Intel).
- Windows may omit windows that apps mark as capture-protected from the shared screen.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the pipeline and roadmap.
