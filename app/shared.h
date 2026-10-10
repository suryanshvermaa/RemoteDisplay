#pragma once
// Tiny same-user IPC between the background worker and the GUI: the worker publishes its status
// in a named shared-memory block and listens for a "kick" event. Both live in the interactive
// user's session, so the Local\ namespace is shared and the default access rules (same user)
// apply, which is exactly the trust boundary we want.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "common.h"
#include "host.h"
#include "ipc_security.h"

namespace rdapp {

// The host asks the saved config who is trusted, and writes newly trusted devices into it.
class ConfigTrust : public rdh::TrustStore {
public:
    bool IsTrusted(const rd::DeviceId& id) override { return Config().IsTrusted(id); }
    void Remember(const rd::DeviceId& id, const std::string& name) override { Config().AddTrusted(id, name); }
};

constexpr wchar_t kStatusMapName[] = L"Local\\RemoteDisplayStatus";
constexpr wchar_t kKickEventName[] = L"Local\\RemoteDisplayKick";
constexpr wchar_t kRestartEventName[] = L"Local\\RemoteDisplayRestart";
constexpr wchar_t kHostActiveName[] = L"Local\\RemoteDisplayHostActive";
constexpr uint32_t kStatusMagic = 0x53445248;  // 'HRDS'

struct SharedStatus {
    uint32_t magic;
    int32_t state;  // rdh::HostStatus::State
    float fps;
    float mbps;
    uint32_t pid;
    uint32_t controlling;  // viewer used mouse/keyboard recently
    uint64_t heartbeatMs;  // GetTickCount64() at last update
    char peer[48];
    char error[96];
};

// Holding this object means "a host is running in this session". It stops two hosts (GUI and
// background worker) from fighting over the UDP port. Released when the handle closes.
class HostActiveLock {
public:
    HostActiveLock() = default;
    HostActiveLock(const HostActiveLock&) = delete;
    HostActiveLock& operator=(const HostActiveLock&) = delete;
    ~HostActiveLock() { Release(); }
    bool Acquire() {
        if (h_) return true;
        HANDLE h = CreateEventW(IpcSecurity(), TRUE, FALSE, kHostActiveName);
        if (!h) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(h);
            return false;
        }
        h_ = h;
        return true;
    }
    void Release() {
        if (h_) CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

class StatusWriter {
public:
    ~StatusWriter() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
        if (kick_) CloseHandle(kick_);
        if (restart_) CloseHandle(restart_);
    }
    bool Create() {
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, IpcSecurity(), PAGE_READWRITE, 0, sizeof(SharedStatus), kStatusMapName);
        if (!map_) return false;
        view_ = static_cast<SharedStatus*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedStatus)));
        kick_ = CreateEventW(IpcSecurity(), FALSE, FALSE, kKickEventName);  // auto-reset
        restart_ = CreateEventW(IpcSecurity(), FALSE, FALSE, kRestartEventName);
        return view_ && kick_ && restart_;
    }
    void Publish(rdh::HostStatus& st) {
        SharedStatus s = {};
        s.magic = kStatusMagic;
        s.state = st.state;
        s.fps = st.fps;
        s.mbps = st.mbps;
        s.pid = GetCurrentProcessId();
        s.controlling = st.controlling ? 1 : 0;
        s.heartbeatMs = GetTickCount64();
        strncpy_s(s.peer, st.Peer().c_str(), _TRUNCATE);
        strncpy_s(s.error, st.Error().c_str(), _TRUNCATE);
        memcpy(view_, &s, sizeof(s));
    }
    bool KickRequested() { return WaitForSingleObject(kick_, 0) == WAIT_OBJECT_0; }
    bool RestartRequested() { return restart_ && WaitForSingleObject(restart_, 0) == WAIT_OBJECT_0; }

private:
    HANDLE map_ = nullptr, kick_ = nullptr, restart_ = nullptr;
    SharedStatus* view_ = nullptr;
};

class StatusReader {
public:
    // True if a worker published a heartbeat within the last 3 seconds.
    static bool Read(SharedStatus& out) {
        HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, kStatusMapName);
        if (!m) return false;
        auto* v = static_cast<SharedStatus*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(SharedStatus)));
        bool ok = false;
        if (v) {
            memcpy(&out, v, sizeof(out));
            ok = out.magic == kStatusMagic && GetTickCount64() - out.heartbeatMs < 3000;
            UnmapViewOfFile(v);
        }
        CloseHandle(m);
        return ok;
    }
    static void Kick() {
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kKickEventName);
        if (e) {
            SetEvent(e);
            CloseHandle(e);
        }
    }
    // Ends the worker so the service relaunches it (used after the key/approval setting changes).
    static void RestartWorker(uint32_t pid) {
        // An event, not TerminateProcess: a normal-rights process may not terminate an elevated worker.
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kRestartEventName)) {
            SetEvent(e);
            CloseHandle(e);
            return;
        }
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {  // older worker without the event
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }
};

}  // namespace rdapp
