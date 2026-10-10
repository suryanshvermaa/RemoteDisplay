# RemoteDisplay architecture

## Pipeline (Phase 0, implemented)

```
host:   DXGI Desktop Duplication -> D3D11 video processor (BGRA->NV12, GPU)
        -> Media Foundation hardware H.264 (CBR, no B-frames, low-latency, IDR on demand)
        -> UDP fragments (<=1200 B payload)
client: UDP -> reassemble -> MF H.264 decoder (DXVA, GPU) -> D3D11 video processor
        -> flip-model swapchain, Present(0, ALLOW_TEARING)
```

Latency design choices:
- No frame ever touches system memory on the host; no copies on the client.
- UDP, never TCP: a lost packet must not stall newer frames.
- Loss recovery = client requests an IDR (`KeyReq`), no retransmit wait. Rate-limited to 10/s.
- Encoder is drop-if-busy: a late frame is discarded rather than queued.
- Idle screen sends nothing; a requested IDR re-encodes the last frame.

## Run

```
build\Release\rd-host.exe                      # prints a pairing key
build\Release\rd-client.exe --key <that key>
```
LAN or internet: `rd-host --listen 0.0.0.0` and `rd-client --host <host-ip> --key <key>`.
The host binds loopback by default. Every datagram is authenticated and encrypted (see
`service/include/session.h`): ECDH P-256 handshake bound to the 128-bit pairing key, AES-128-GCM per
packet, sliding-window replay protection. A client with the wrong key never gets a session.
Known limits: a 128-bit key must be copied by hand (a short-code PAKE is a later improvement),
the host serves one client at a time, and new-handshake requests are rate-limited but unauthenticated.

## Input and cursor

- Viewer to host: `Input` (reliable: buttons, wheel, keys; consecutive sequence numbers, host acks
  `InputAck`, viewer resends the unacked window every 30 ms, host applies strictly in order and
  ignores duplicates) and `InputMove` (unreliable, newest position wins). Positions are normalised
  0..65535 over the picture, so window size and letterboxing do not matter. The host injects with
  `SendInput` (`service/src/inject.h`) and releases everything it pressed if the viewer goes away.
- Input is accepted only in the Streaming state (i.e. after approval) and only if control is allowed.
- Host to viewer: the captured picture has no pointer, so the cursor shape is sent as `Cursor`
  fragments (on change, to new viewers, and every 2 s) and shown as the viewer's local cursor, which
  keeps it lag-free. Cursor-only updates no longer cost a video frame.

## Components

- `service/src/host.h`: `RunHost()` capture -> encode -> send, stoppable, reports `HostStatus`
  (approval gate, disconnect, 1 s encrypted keepalive, same-IP re-pair grace of 2 minutes).
- `client/src/viewer.h`: `Viewer` window + network/decode thread.
- `app/main.cpp`: the GUI (`RemoteDisplay.exe`) combining both, plus the tray icon, close-to-tray,
  single-instance rule and `--background` (hidden, auto-share) mode used by the startup entry. The tray icon is optional (`[app] tray=0` in config.ini); the single-instance relaunch is how a fully hidden instance is reopened.
- `app/service.h`: install/uninstall (elevated helper), and the service that supervises the worker.
- `app/worker.h`: `--host-worker`, the headless host in the user session; approval prompt, log.
- `app/shared.h`: same-user IPC (shared-memory status, kick/restart events, "host active" lock).
- `app/ipc_security.h`: elevation helpers: an explicit ACL for shared objects (so a normal-rights window
  can use an elevated copy's objects), message filters for the shell and second launches, and the
  machine-wide "elevate the service's worker" flag.
- `app/admin.h`: administrator mode: elevated Scheduled Task (full XML: no 72 h limit, runs on battery),
  the HKLM flag, restart-elevated.
- `service/include/identity.h`: per-device ECDSA identity; `Identify` signs the session's handshake
  transcript so the host can recognise devices the user chose to trust.
- `rd-host` / `rd-client`: thin CLI wrappers.

Process model with the service installed:

```
Windows boot -> RemoteDisplayHost (LocalSystem, Session 0)
                  | watches active console session, WTSQueryUserToken + CreateProcessAsUser
                  v
        RemoteDisplay.exe --host-worker  (user session) -- UDP 47800 --> viewers
                  ^ status (shared memory) / kick (event)
        RemoteDisplay.exe (GUI, same user)
```

## Roadmap

1. **Phase 1, virtual display**: IDD (UMDF 2) driver in `driver/`, clone-mode monitor, feed its
   swapchain frames into the same encoder. Must be verified empirically (see below).
2. **Phase 2, internet**: DONE encryption + authentication. TODO signaling server, STUN + UDP hole
   punching, relay fallback, pacing of keyframe bursts, congestion control (bitrate adaptation
   from loss/RTT reports).
3. **Phase 3**: DONE input return + cursor. TODO SYSTEM-level helper for elevated windows/UAC/lock
   screen, audio (WASAPI loopback + Opus), clipboard, multi-monitor, FEC.

## Open question to verify in Phase 1

Whether frames from an IDD swapchain include windows flagged `WDA_EXCLUDEFROMCAPTURE` depends on the
Windows build. Test it with a window that sets the flag before relying on it. Capture-protected
windows must only be shown with explicit, informed user consent; the host should list them and
let the user decide.
