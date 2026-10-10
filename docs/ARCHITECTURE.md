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

## Run locally

```
build\Release\rd-host.exe
build\Release\rd-client.exe
```
LAN: `rd-host --listen 0.0.0.0` and `rd-client --host <host-ip>`. **No encryption or auth yet**, so do
not expose the port beyond a trusted network until Phase 2 is done.

## Roadmap

1. **Phase 1, virtual display**: IDD (UMDF 2) driver in `driver/`, clone-mode monitor, feed its
   swapchain frames into the same encoder. Must be verified empirically (see below).
2. **Phase 2, internet**: pairing code + signaling server, STUN + UDP hole punching, relay
   fallback, authenticated key exchange and AEAD encryption of every datagram (libsodium),
   pacing of keyframe bursts, congestion control (bitrate adaptation from loss/RTT reports).
3. **Phase 3**: input return channel (SendInput; SYSTEM service for UAC/secure desktop),
   audio (WASAPI loopback + Opus), clipboard, multi-monitor, cursor as separate stream, FEC.

## Open question to verify in Phase 1

Whether frames from an IDD swapchain include windows flagged `WDA_EXCLUDEFROMCAPTURE` depends on the
Windows build. Test it with a window that sets the flag before relying on it. Capture-protected
windows must only be shown with explicit, informed user consent; the host should list them and
let the user decide.
