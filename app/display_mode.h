#pragma once
// Display layout while sharing: Duplicate (every screen shows the same picture) or Extend (screens side by
// side). Switching changes what the user sees on this PC, so the layout in effect before sharing is saved
// and put back when sharing stops, or at the next start if the process was killed first.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace rdapp {

// A display layout as Windows reports it. Empty if it could not be read.
struct SavedDisplays {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    bool valid() const { return !paths.empty(); }
};

// Reads the active display layout.
inline bool SaveDisplays(SavedDisplays& out) {
    out = SavedDisplays{};
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;
    out.paths.resize(pathCount);
    out.modes.resize(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, out.paths.data(), &modeCount, out.modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        out = SavedDisplays{};
        return false;
    }
    out.paths.resize(pathCount);
    out.modes.resize(modeCount);
    return out.valid();
}

// Puts a saved layout back. Returns false if Windows refuses it.
inline bool RestoreDisplays(const SavedDisplays& saved) {
    if (!saved.valid()) return false;
    // SetDisplayConfig takes non-const arrays, so pass copies.
    auto paths = saved.paths;
    auto modes = saved.modes;
    return SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()), modes.data(),
                            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES) == ERROR_SUCCESS;
}

// Duplicate: every screen shows the same picture.
inline bool ApplyDuplicate() {
    return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_CLONE) == ERROR_SUCCESS;
}

// Extend: the screens side by side.
inline bool ApplyExtend() {
    return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND) == ERROR_SUCCESS;
}

// Where the saved layout is kept while it is changed (%ProgramData%\RemoteDisplay\display-restore.bin).
inline std::wstring DisplayRestorePath() {
    wchar_t pd[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    const std::wstring dir = std::wstring(pd) + L"\\RemoteDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\display-restore.bin";
}

inline bool WriteSaved(const SavedDisplays& d, const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint32_t header[3] = {0x52445352, uint32_t(d.paths.size()), uint32_t(d.modes.size())};  // magic, counts
    DWORD w = 0;
    bool ok = WriteFile(f, header, sizeof(header), &w, nullptr) != FALSE;
    ok = ok && WriteFile(f, d.paths.data(), DWORD(d.paths.size() * sizeof(DISPLAYCONFIG_PATH_INFO)), &w, nullptr) != FALSE;
    ok = ok && WriteFile(f, d.modes.data(), DWORD(d.modes.size() * sizeof(DISPLAYCONFIG_MODE_INFO)), &w, nullptr) != FALSE;
    CloseHandle(f);
    return ok;
}

inline bool ReadSaved(const std::wstring& path, SavedDisplays& d) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint32_t header[3] = {};
    DWORD r = 0;
    bool ok = ReadFile(f, header, sizeof(header), &r, nullptr) && r == sizeof(header) && header[0] == 0x52445352;
    ok = ok && header[1] <= 64 && header[2] <= 256;  // sanity limits on a file we did not write in this run
    if (ok) {
        d.paths.resize(header[1]);
        d.modes.resize(header[2]);
        ok = ReadFile(f, d.paths.data(), DWORD(d.paths.size() * sizeof(DISPLAYCONFIG_PATH_INFO)), &r, nullptr) &&
             ReadFile(f, d.modes.data(), DWORD(d.modes.size() * sizeof(DISPLAYCONFIG_MODE_INFO)), &r, nullptr);
    }
    CloseHandle(f);
    return ok && d.valid();
}

// Holds the display change for one sharing session. Begin applies Duplicate or Extend; End (or the destructor)
// puts the layout back. The saved layout is also on disk until End, so RecoverIfNeeded can restore it after a kill.
class DisplayModeSession {
public:
    DisplayModeSession() = default;
    DisplayModeSession(const DisplayModeSession&) = delete;
    DisplayModeSession& operator=(const DisplayModeSession&) = delete;
    ~DisplayModeSession() { End(); }

    // mode: "duplicate" or "extend". Anything else leaves the layout alone. Returns true if it applied.
    bool Begin(const std::string& mode) {
        End();
        if (mode != "duplicate" && mode != "extend") return false;
        if (!SaveDisplays(saved_)) return false;
        if (!WriteSaved(saved_, DisplayRestorePath())) return false;  // no file, no change
        changed_ = mode == "duplicate" ? ApplyDuplicate() : ApplyExtend();
        if (!changed_) DeleteFileW(DisplayRestorePath().c_str());
        return changed_;
    }

    // Puts the saved layout back, if Begin changed it.
    void End() {
        if (!changed_) return;
        changed_ = false;
        RestoreDisplays(saved_);
        DeleteFileW(DisplayRestorePath().c_str());
    }

    // Call at startup: a layout left by a process that was killed mid-session is put back.
    static void RecoverIfNeeded() {
        SavedDisplays left;
        if (ReadSaved(DisplayRestorePath(), left) && RestoreDisplays(left)) DeleteFileW(DisplayRestorePath().c_str());
    }

private:
    SavedDisplays saved_;
    bool changed_ = false;
};

}  // namespace rdapp
