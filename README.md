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

The sharing PC gets an "Allow this device to view and control your PC?" prompt for each new device
with three choices: **Allow once**, **Always allow this device**, or **Deny** (Deny is the default
button). It also has a Disconnect button, an **Allow mouse and keyboard control** switch (off = view
only), and the pairing key can be regenerated at any time. The status line says "viewing" or
"controlling".

**Trusted devices.** Every viewer has its own permanent device key (stored with Windows DPAPI). If
you choose *Always allow this device*, that device connects later with no prompt, even after
restarts, as long as it also has your pairing key. The host remembers only the device's public-key
fingerprint, and the device proves itself by signing each session, so nothing that was recorded on
the network can be replayed. *Allow once* and *Deny* remember nothing. Click **Trusted devices...**
to see and remove them. Generating a new pairing key also locks every device out until it has the
new key. (Reconnecting from the same IP within 20 seconds of a dropped connection skips the prompt
too, so a network blip does not nag you.)

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

In the **Run in background** section of the sharing card, click **Install service** (Windows asks
for administrator approval). That installs `RemoteDisplayHost`, which starts with Windows, adds a
Windows Firewall rule for UDP 47800 on private/domain networks, and keeps this PC shareable
without opening the app. **Uninstall service** (greyed out until it is installed; it asks to
confirm) stops and deletes the service and removes the firewall rule; the app and your settings
stay. From a command prompt, as administrator: `RemoteDisplay.exe --service-ctl uninstall`.

### Administrator rights (optional)

Windows refuses mouse and keyboard input from a normal program into an **elevated** window (Task
Manager run as admin, Registry Editor, installers). To drive those remotely, tick **Run as
administrator** (one UAC prompt) or click **Restart as admin** to relaunch elevated right now. It
also applies to the other two ways of running:

- **Start with Windows** then uses a Scheduled Task with "highest privileges" (created once, no
  prompt at each sign-in) instead of the normal startup entry, which Windows can never start elevated.
- **The service** launches its worker with your own account's full-rights token (your account must be
  an administrator; no password is stored or needed).

Untick it to go back; it only changes the start-up entry and the service's worker. Still not
possible even with admin rights: the **UAC consent screen** and the **lock screen** run on Windows'
secure desktop, which needs a separate SYSTEM-level component that is not built yet. Running the
whole app as administrator means a connected viewer can do anything an administrator can do on this
PC, so only enable it on a PC where you accept that, and keep the pairing key private.

How it works: screen capture cannot run inside a service (Session 0 has no desktop), so the service
only supervises. It launches `RemoteDisplay.exe --host-worker` into the signed-in user's session as
that user, and relaunches it after a crash or when the active user changes. The worker shows the
"Allow this connection?" prompt on your screen; the app shows its live status and can disconnect it.
Changing the key or the approval setting restarts the worker automatically.

Limits: sharing starts once a user is signed in (not at the login screen), a locked screen or UAC
prompt cannot be viewed yet, and if you move `RemoteDisplay.exe` you must remove and reinstall.
Logs: `%ProgramData%\RemoteDisplay\service.log` and `%APPDATA%\RemoteDisplay\worker.log`.

### Secure desktop (UAC prompt, sign-in and lock screen): opt-in

Windows draws the UAC prompt, the sign-in screen and the lock screen on a separate "secure desktop"
that ordinary programs cannot see. RemoteDisplay can show it to a viewer, but only when all of these
hold:

1. **The Windows service is installed** (Install service). The service runs the helper; without it
   the secure desktop cannot be seen, and the viewer sees a frozen picture.
2. **An administrator allowed it**: tick **Allow viewing UAC and sign-in screens (admin)** and accept
   the UAC prompt. A connected viewer cannot change this. It is stored machine-wide.
3. **A viewer is connected**, and the desktop is actually the secure one.

What happens then:

- A SYSTEM helper, started on the Winlogon desktop by the service, captures the secure desktop and
  hands its picture to the worker through shared memory. The viewer sees the UAC prompt or sign-in
  screen. When the normal desktop returns, the stream switches back automatically.
- **View only.** Mouse and keyboard input is received and acknowledged, but never sent to the secure
  desktop. You cannot type a password into a UAC prompt remotely.
- The pointer is not shown on those screens (the viewer's own pointer is used).
- Every switch in or out of the secure desktop is written to `%ProgramData%\RemoteDisplayudit.log`
  with the time, Windows user and process. The helper logs to `secure-helper.log` in the same folder.

Turning consent off stops the helper within a few seconds.

To check it works on your PC, follow [tests/secure_desktop_manual.md](tests/secure_desktop_manual.md).

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

- **UAC prompt and lock screen**: not viewable or controllable (secure desktop), nor Ctrl+Alt+Del;
  admin windows work once you enable administrator rights (see above).
- No clipboard or file transfer, no audio; characters typed with an IME or dead keys are not
  forwarded as text (keys are sent by scancode).
- **Same network or a reachable address**: no relay or NAT traversal yet, so over the internet you
  need a port-forward (UDP 47800) or a VPN such as Tailscale/WireGuard.
- One viewer at a time; primary monitor only; no audio or clipboard.
- Needs a GPU with a hardware H.264 encoder (NVIDIA, AMD or Intel).
- Windows may omit windows that apps mark as capture-protected from the shared screen.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the pipeline and roadmap.
