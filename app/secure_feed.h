#pragma once
// The picture of the secure desktop (UAC prompt, sign-in / lock screen), handed from the secure-desktop
// helper (SYSTEM, console session, Winlogon desktop) to the worker (same session) through shared memory.
//
// The helper writes its newest BGRA frame behind a seqlock: the sequence number is odd while a write is
// in progress. The reader copies the frame and checks the sequence number afterwards; if it changed, the
// copy is thrown away and retried, so a viewer never gets half of one picture and half of the next.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "capture.h"
#include "host.h"
#include "ipc_security.h"
#include "protocol.h"

namespace rdapp {

constexpr wchar_t kSecureMapName[] = L"Local\\RemoteDisplaySecureFrame";
constexpr uint32_t kSecureMagic = 0x46534452;                // "RDSF"
constexpr size_t kSecureMaxBytes = size_t(3840) * 2160 * 4;  // room for 4K
constexpr uint64_t kSecureStaleMs = 1500;                    // helper heartbeat older than this = not running

struct SecureHeader {
    volatile LONG64 heartbeatMs;  // first, so it is 8-byte aligned for atomic access
    volatile LONG seq;            // odd while the helper is writing
    uint32_t magic;
    volatile LONG secure;         // 1 = the secure desktop is the input desktop right now
    uint32_t width, height, pitch;
    volatile LONG frameNo;        // +1 per published picture
};

inline size_t SecureMapBytes() { return sizeof(SecureHeader) + kSecureMaxBytes; }

class SecureFeedWriter {
public:
    SecureFeedWriter() = default;
    SecureFeedWriter(const SecureFeedWriter&) = delete;
    SecureFeedWriter& operator=(const SecureFeedWriter&) = delete;
    ~SecureFeedWriter() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
    }

    bool Create(const wchar_t* name = kSecureMapName) {
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, IpcSecurity(), PAGE_READWRITE, 0, DWORD(SecureMapBytes()), name);
        if (!map_) return false;
        view_ = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view_) return false;
        hdr_ = static_cast<SecureHeader*>(view_);
        hdr_->magic = kSecureMagic;
        return true;
    }

    // Called every loop, even when there is no new picture, so the worker knows the helper is alive.
    void Heartbeat(bool secure) {
        InterlockedExchange64(&hdr_->heartbeatMs, LONG64(GetTickCount64()));
        InterlockedExchange(&hdr_->secure, secure ? 1 : 0);
    }

    void Publish(const uint8_t* bgra, UINT w, UINT h, UINT pitch) {
        const size_t bytes = size_t(pitch) * h;
        if (!bgra || bytes == 0 || bytes > kSecureMaxBytes) return;
        // Take the last even value as the base. A previous helper killed mid-write can leave the counter
        // odd; counting from that would invert the parity and freeze every reader.
        const LONG base = hdr_->seq & ~1;
        InterlockedExchange(&hdr_->seq, base + 1);  // odd: a reader that sees this must retry
        MemoryBarrier();
        hdr_->width = w;
        hdr_->height = h;
        hdr_->pitch = pitch;
        memcpy(Data(), bgra, bytes);
        MemoryBarrier();
        InterlockedIncrement(&hdr_->frameNo);
        InterlockedExchange(&hdr_->seq, base + 2);  // even: consistent again
    }

private:
    uint8_t* Data() { return static_cast<uint8_t*>(view_) + sizeof(SecureHeader); }

    HANDLE map_ = nullptr;
    void* view_ = nullptr;
    SecureHeader* hdr_ = nullptr;
};

struct SecureFrameInfo {
    uint32_t width = 0, height = 0, pitch = 0;
    LONG frameNo = 0;
};

// ---------------------------------------------------------------------------------------------
// Input for the secure desktop: worker -> helper, through a ring in shared memory.
//
// Only the helper (SYSTEM, on the Winlogon desktop) can type and click on the UAC prompt and the sign-in
// screen. The ring is writable only by SYSTEM and Administrators. That keeps UAC's own guarantee: a program
// running with a normal user's rights cannot click "Yes" on a UAC prompt by writing here. In practice it
// means the secure screens are interactive when the host runs as administrator (the service's admin mode);
// without that they stay view-only.
constexpr wchar_t kSecureInputMapName[] = L"Local\\RemoteDisplaySecureInput";
constexpr uint32_t kSecureInputMagic = 0x49534452;  // "RDSI"
constexpr uint32_t kSecureInputRing = 512;

enum class SecureInputKind : uint32_t { Event = 0, Move = 1, ReleaseAll = 2 };

struct SecureInputEntry {
    uint32_t kind;       // SecureInputKind
    rd::InputEvent ev;   // Event: the event; Move: x and y
};

struct SecureInputRing {
    uint32_t magic;
    volatile LONG head;  // next slot the worker writes (only the worker changes it)
    volatile LONG tail;  // next slot the helper reads (only the helper changes it)
    SecureInputEntry entries[kSecureInputRing];
};

inline SECURITY_ATTRIBUTES* SecureInputSecurity() {
    static SECURITY_ATTRIBUTES sa = [] {
        SECURITY_ATTRIBUTES a = {sizeof(a)};
        PSECURITY_DESCRIPTOR sd = nullptr;
        // SYSTEM and Administrators only, at High integrity: no write-up from a normal user's process.
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NW;;;HI)", SDDL_REVISION_1, &sd,
                                                                 nullptr))
            a.lpSecurityDescriptor = sd;
        return a;
    }();
    return sa.lpSecurityDescriptor ? &sa : nullptr;
}

class SecureInputReader {  // the helper's end
public:
    SecureInputReader() = default;
    SecureInputReader(const SecureInputReader&) = delete;
    SecureInputReader& operator=(const SecureInputReader&) = delete;
    ~SecureInputReader() {
        if (ring_) UnmapViewOfFile(ring_);
        if (map_) CloseHandle(map_);
    }
    bool Create(const wchar_t* name = kSecureInputMapName) {
        if (!SecureInputSecurity()) return false;  // never fall back to an open ACL
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, SecureInputSecurity(), PAGE_READWRITE, 0, sizeof(SecureInputRing), name);
        if (!map_) return false;
        ring_ = static_cast<SecureInputRing*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!ring_) return false;
        ring_->magic = kSecureInputMagic;
        return true;
    }
    // The next queued entry, or false if there is none.
    bool Pop(SecureInputEntry& out) {
        if (!ring_ || ring_->tail == ring_->head) return false;
        MemoryBarrier();
        out = ring_->entries[uint32_t(ring_->tail) % kSecureInputRing];
        MemoryBarrier();
        InterlockedIncrement(&ring_->tail);
        return true;
    }
    // Throws away everything queued (input that arrived while the secure desktop was not up).
    void Clear() {
        if (ring_) InterlockedExchange(&ring_->tail, ring_->head);
    }

private:
    HANDLE map_ = nullptr;
    SecureInputRing* ring_ = nullptr;
};

class SecureInputWriter {  // the worker's end
public:
    SecureInputWriter() = default;
    SecureInputWriter(const SecureInputWriter&) = delete;
    SecureInputWriter& operator=(const SecureInputWriter&) = delete;
    ~SecureInputWriter() {
        if (ring_) UnmapViewOfFile(ring_);
        if (map_) CloseHandle(map_);
    }
    // False when the worker has no right to write (a normal-rights worker) or the helper is not running.
    bool Push(SecureInputKind kind, const rd::InputEvent& ev, const wchar_t* name = kSecureInputMapName) {
        if (!Open(name)) return false;
        const uint32_t head = uint32_t(ring_->head);
        if (head - uint32_t(ring_->tail) >= kSecureInputRing) return false;  // full: the helper is not keeping up
        ring_->entries[head % kSecureInputRing] = {uint32_t(kind), ev};
        MemoryBarrier();
        InterlockedIncrement(&ring_->head);
        return true;
    }

private:
    bool Open(const wchar_t* name) {
        if (ring_) return true;
        const uint64_t now = GetTickCount64();
        if (tried_ && now - lastTry_ < 1000) return false;
        tried_ = true;
        lastTry_ = now;
        map_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!map_) return false;
        ring_ = static_cast<SecureInputRing*>(MapViewOfFile(map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        if (!ring_ || ring_->magic != kSecureInputMagic) {
            if (ring_) UnmapViewOfFile(ring_);
            ring_ = nullptr;
            CloseHandle(map_);
            map_ = nullptr;
            return false;
        }
        return true;
    }
    HANDLE map_ = nullptr;
    SecureInputRing* ring_ = nullptr;
    bool tried_ = false;
    uint64_t lastTry_ = 0;
};

class SecureFeedReader : public rdh::SecureSource {
public:
    explicit SecureFeedReader(const wchar_t* name = kSecureMapName) : name_(name) {}
    SecureFeedReader(const SecureFeedReader&) = delete;
    SecureFeedReader& operator=(const SecureFeedReader&) = delete;
    ~SecureFeedReader() override {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
    }

    // Sends input to the helper, which types and clicks on the secure desktop. False if this worker may not
    // (it does not run as administrator): input there is then dropped, and the screen stays view-only.
    bool ForwardInput(int kind, const rd::InputEvent& ev) override {
        const bool ok = input_.Push(SecureInputKind(kind), ev);
        if (!ok && !denyLogged_ && kind == int(SecureInputKind::Event)) {
            denyLogged_ = true;
            if (FILE* f = _wfopen(HelperLogPathForFeed().c_str(), L"a")) {
                fputs("input for the secure desktop was not delivered: the host is not running as administrator\n", f);
                fclose(f);
            }
        }
        return ok;
    }

    // True while the helper is alive and says the secure desktop is the input desktop.
    bool Active() override {
        if (!Open()) return false;
        const uint64_t hb = uint64_t(hdr_->heartbeatMs);
        return GetTickCount64() - hb < kSecureStaleMs && hdr_->secure != 0;
    }

    ID3D11Texture2D* Upload(Capture& cap, bool needFrame) override {
        SecureFrameInfo info;
        if (!ReadLatest(buf_, info, needFrame)) return nullptr;
        if (info.width != cap.bgraWidth() || info.height != cap.bgraHeight()) {
            // The secure desktop has another size than our device: rebuild the device at that size.
            // The host sees the generation change and rebuilds the encoder before the next frame.
            cap.ResizeTo(info.width, info.height);
            return nullptr;
        }
        return cap.UploadBgra(buf_.data(), info.pitch, info.width, info.height);
    }

    // Copies the newest picture. Returns false if there is none, or if it is the one already read
    // (unless `any`), or if writes kept colliding with the copy.
    bool ReadLatest(std::vector<uint8_t>& out, SecureFrameInfo& info, bool any) {
        if (!Open()) return false;
        for (int tries = 0; tries < 8; ++tries) {
            const LONG s1 = hdr_->seq;
            if (s1 & 1) {  // helper is writing
                Sleep(0);
                continue;
            }
            MemoryBarrier();
            const LONG frame = hdr_->frameNo;
            if (frame == lastFrame_ && !any) return false;
            info.width = hdr_->width;
            info.height = hdr_->height;
            info.pitch = hdr_->pitch;
            info.frameNo = frame;
            const size_t bytes = size_t(info.pitch) * info.height;
            if (bytes == 0 || bytes > kSecureMaxBytes) return false;
            out.resize(bytes);
            memcpy(out.data(), static_cast<uint8_t*>(view_) + sizeof(SecureHeader), bytes);
            MemoryBarrier();
            if (hdr_->seq == s1) {
                lastFrame_ = frame;
                return true;
            }
        }
        return false;
    }

private:
    // Opening is retried at most once a second, so a worker that runs without the helper stays cheap.
    bool Open() {
        if (view_) return true;
        const uint64_t now = GetTickCount64();
        if (tried_ && now - lastTry_ < 1000) return false;
        tried_ = true;
        lastTry_ = now;
        map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name_);
        if (!map_) return false;
        view_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0);
        if (!view_) {
            CloseHandle(map_);
            map_ = nullptr;
            return false;
        }
        hdr_ = static_cast<SecureHeader*>(view_);
        return true;
    }

    static std::wstring HelperLogPathForFeed() {
        wchar_t pd[MAX_PATH] = {};
        GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
        return std::wstring(pd) + L"\\RemoteDisplay\\secure-helper.log";
    }

    const wchar_t* name_;
    HANDLE map_ = nullptr;
    void* view_ = nullptr;
    SecureHeader* hdr_ = nullptr;
    bool tried_ = false;
    uint64_t lastTry_ = 0;
    LONG lastFrame_ = 0;
    std::vector<uint8_t> buf_;
    SecureInputWriter input_;
    bool denyLogged_ = false;
};

}  // namespace rdapp
