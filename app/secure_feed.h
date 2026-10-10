#pragma once
// The picture of the secure desktop (UAC prompt, sign-in / lock screen), handed from the secure-desktop
// helper (SYSTEM, console session, Winlogon desktop) to the worker (same session) through shared memory.
//
// The helper writes its newest BGRA frame behind a seqlock: the sequence number is odd while a write is
// in progress. The reader copies the frame and checks the sequence number afterwards; if it changed, the
// copy is thrown away and retried, so a viewer never gets half of one picture and half of the next.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "capture.h"
#include "host.h"
#include "ipc_security.h"

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
        InterlockedIncrement(&hdr_->seq);  // odd: a reader that sees this must retry
        MemoryBarrier();
        hdr_->width = w;
        hdr_->height = h;
        hdr_->pitch = pitch;
        memcpy(Data(), bgra, bytes);
        MemoryBarrier();
        InterlockedIncrement(&hdr_->frameNo);
        InterlockedIncrement(&hdr_->seq);  // even: consistent again
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

class SecureFeedReader : public rdh::SecureSource {
public:
    explicit SecureFeedReader(const wchar_t* name = kSecureMapName) : name_(name) {}
    SecureFeedReader(const SecureFeedReader&) = delete;
    SecureFeedReader& operator=(const SecureFeedReader&) = delete;
    ~SecureFeedReader() override {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
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

    const wchar_t* name_;
    HANDLE map_ = nullptr;
    void* view_ = nullptr;
    SecureHeader* hdr_ = nullptr;
    bool tried_ = false;
    uint64_t lastTry_ = 0;
    LONG lastFrame_ = 0;
    std::vector<uint8_t> buf_;
};

}  // namespace rdapp
