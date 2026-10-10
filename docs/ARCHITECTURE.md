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

## Components

- `service/src/host.h`: `RunHost()` capture -> encode -> send, stoppable, reports `HostStatus`
  (approval gate, disconnect, 1 s encrypted keepalive, same-IP re-pair grace of 2 minutes).
- `client/src/viewer.h`: `Viewer` window + network/decode thread.
- `app/main.cpp`: the GUI (`RemoteDisplay.exe`) combining both.
- `app/service.h`: install/uninstall (elevated helper), and the service that supervises the worker.
- `app/worker.h`: `--host-worker`, the headless host in the user session; approval prompt, log.
- `app/shared.h`: same-user IPC (shared-memory status, kick event, "host active" lock).
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
3. **Phase 3**: input return channel (SendInput; SYSTEM service for UAC/secure desktop),
   audio (WASAPI loopback + Opus), clipboard, multi-monitor, cursor as separate stream, FEC.

## Open question to verify in Phase 1

Whether frames from an IDD swapchain include windows flagged `WDA_EXCLUDEFROMCAPTURE` depends on the
Windows build. Test it with a window that sets the flag before relying on it. Capture-protected
windows must only be shown with explicit, informed user consent; the host should list them and
let the user decide.
