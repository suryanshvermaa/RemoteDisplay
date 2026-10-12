#pragma once
// Adds and removes the RemoteDisplay virtual monitor (the IddCx driver in driver\).
//
// The monitor is a root-enumerated device with hardware ID root\RemoteDisplayVirtual, created with
// SetupAPI, the same way devcon creates devices. Both calls change the system's devices, so they need
// administrator rights and run only in the elevated helper (admin.h).
#include <windows.h>
#include <newdev.h>
#include <setupapi.h>

#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

namespace rdapp {

constexpr wchar_t kVirtualMonitorHwId[] = L"root\\RemoteDisplayVirtual";

// The Display device setup class.
inline const GUID& DisplayClassGuid() {
    static const GUID g = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    return g;
}

inline std::wstring LastErrorText(const wchar_t* what) {
    return std::wstring(what) + L" (error " + std::to_wstring(GetLastError()) + L")";
}

// Creates the device and installs the driver from `infPath` (a driver package INF). Returns false with
// `err` set if anything fails. A reboot may be needed after the first install; `rebootNeeded` says so.
inline bool AddVirtualMonitor(const std::wstring& infPath, std::wstring& err, bool& rebootNeeded) {
    rebootNeeded = false;
    HDEVINFO set = SetupDiCreateDeviceInfoList(&DisplayClassGuid(), nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        err = LastErrorText(L"SetupDiCreateDeviceInfoList");
        return false;
    }
    bool ok = false;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    do {
        // The device is named after its class, as devcon does: the instance ID becomes ROOT\DISPLAY\<n>.
        // (Naming it by the hardware ID is rejected with ERROR_INVALID_DEVINST_NAME.)
        if (!SetupDiCreateDeviceInfoW(set, L"Display", &DisplayClassGuid(), L"RemoteDisplay Virtual Monitor", nullptr,
                                      DICD_GENERATE_ID, &dev)) {
            err = LastErrorText(L"SetupDiCreateDeviceInfo");
            break;
        }
        // The hardware ID list is multi-string: each string ends with NUL and the list ends with another NUL.
        std::wstring ids = kVirtualMonitorHwId;
        ids.push_back(L'\0');
        ids.push_back(L'\0');
        if (!SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids.data()),
                                               DWORD(ids.size() * sizeof(wchar_t)))) {
            err = LastErrorText(L"SetupDiSetDeviceRegistryProperty");
            break;
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev)) {
            err = LastErrorText(L"SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
            break;
        }
        // First let Windows find the package in the driver store (pnputil /add-driver put it there).
        // If that fails, install straight from the INF file.
        BOOL reboot = FALSE;
        BOOL installed = UpdateDriverForPlugAndPlayDevicesW(nullptr, kVirtualMonitorHwId, nullptr, 0, &reboot);
        const DWORD storeErr = installed ? 0 : GetLastError();
        if (!installed) installed = UpdateDriverForPlugAndPlayDevicesW(nullptr, kVirtualMonitorHwId, infPath.c_str(), INSTALLFLAG_FORCE, &reboot);
        if (!installed) {
            err = L"UpdateDriverForPlugAndPlayDevices failed: from the driver store (error " + std::to_wstring(storeErr) +
                  L"), from " + infPath + L" (error " + std::to_wstring(GetLastError()) + L")";
            // Do not leave an empty device behind.
            SetupDiCallClassInstaller(DIF_REMOVE, set, &dev);
            break;
        }
        rebootNeeded = reboot != FALSE;
        ok = true;
    } while (false);
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

// True if the device's hardware ID list contains the virtual monitor's ID.
inline bool HasVirtualMonitorHwId(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, &type, nullptr, 0, &size);
    if (size == 0) return false;
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, &type, reinterpret_cast<PBYTE>(buf.data()), size, &size))
        return false;
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, kVirtualMonitorHwId) == 0) return true;
    return false;
}

// True if a virtual monitor device already exists (so an install or repair does not add a second one).
inline bool HasVirtualMonitorDevice() {
    HDEVINFO set = SetupDiGetClassDevsW(&DisplayClassGuid(), nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) found = HasVirtualMonitorHwId(set, dev);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// Removes every virtual monitor device. Returns true if none is left (also when none existed).
inline bool RemoveVirtualMonitor(std::wstring& err) {
    HDEVINFO set = SetupDiGetClassDevsW(&DisplayClassGuid(), nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) {
        err = LastErrorText(L"SetupDiGetClassDevs");
        return false;
    }
    bool ok = true;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    // Removing a device shifts the indexes, so after each removal start again from the top.
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev);) {
        if (!HasVirtualMonitorHwId(set, dev)) {
            ++i;
            continue;
        }
        if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dev)) {
            i = 0;
        } else {
            err = LastErrorText(L"SetupDiCallClassInstaller(DIF_REMOVE)");
            ok = false;
            ++i;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

// GDI name of the display showing the virtual monitor (e.g. \\.\DISPLAY22), or empty if there is none.
// Matched on the monitor's name from Windows' display configuration ("... (RD Virtual)").
inline std::wstring FindVirtualMonitorDisplay() {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return L"";
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return L"";
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
        if (wcsstr(target.monitorFriendlyDeviceName, L"RD Virtual") == nullptr) continue;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        return source.viewGdiDeviceName;
    }
    return L"";
}

// Runs a command with no window and returns its standard output.
inline std::string RunCapture(const std::wstring& command) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return "";
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = command;
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(readPipe);
    return out;
}

// Deletes every driver-store package of the virtual monitor (all versions), so an uninstall leaves nothing
// behind. Returns the number of packages removed; `failed` is set if one could not be removed.
inline int RemoveDriverPackages(bool& failed) {
    failed = false;
    int removed = 0;
    const std::string text = RunCapture(L"pnputil /enum-drivers");
    // pnputil prints one block per package: "Published Name:  oemN.inf" then "Original Name:  <name>".
    size_t pos = 0;
    std::string published;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const auto valueOf = [](const std::string& l) {
            const size_t colon = l.find(':');
            if (colon == std::string::npos) return std::string();
            std::string v = l.substr(colon + 1);
            const size_t first = v.find_first_not_of(" \t\r");
            const size_t last = v.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string() : v.substr(first, last - first + 1);
        };
        if (line.find("Published Name") != std::string::npos) published = valueOf(line);
        if (line.find("Original Name") != std::string::npos) {
            std::string original = valueOf(line);
            for (auto& c : original) c = char(tolower(c));
            if (original == "remotedisplayvirtual.inf" && !published.empty()) {
                const std::string out = RunCapture(L"pnputil /delete-driver " + std::wstring(published.begin(), published.end()) + L" /uninstall /force");
                if (out.find("deleted successfully") != std::string::npos) ++removed; else failed = true;
            }
        }
    }
    return removed;
}

}  // namespace rdapp
