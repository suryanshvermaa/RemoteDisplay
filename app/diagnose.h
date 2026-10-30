#pragma once
// RemoteDisplay.exe --diagnose
// Writes %APPDATA%\RemoteDisplay\diagnostics.txt: every GPU and its screens, whether Windows lets this program
// duplicate each screen, the hardware H.264 encoders and which GPU each belongs to, and what the real capture
// and encoder setup does. Meant for PCs where sharing does not start (for example laptops with two GPUs).
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "capture.h"
#include "common.h"
#include "encoder.h"

namespace rdapp {

inline int RunDiagnose() {
    using Microsoft::WRL::ComPtr;
    const std::wstring path = UserDataDir() + L"\\diagnostics.txt";
    FILE* f = _wfopen(path.c_str(), L"w, ccs=UTF-8");
    if (!f) return 1;
    auto hr = [](HRESULT h) {
        wchar_t b[32];
        swprintf_s(b, L"0x%08lX", static_cast<unsigned long>(h));
        return std::wstring(b);
    };

    // Windows build and this program's GPU preference (Settings > Display > Graphics).
    {
        using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW vi = {sizeof(vi)};
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&vi);
        fwprintf(f, L"Windows %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        wchar_t exe[MAX_PATH] = {}, pref[128] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        DWORD n = sizeof(pref);
        const bool has = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", exe,
                                      RRF_RT_REG_SZ, nullptr, pref, &n) == ERROR_SUCCESS;
        fwprintf(f, L"Program: %s\nGPU preference: %s\n\n", exe, has ? pref : L"(none: Windows decides)");
    }

    // GPUs, their screens, and whether each screen can be duplicated from this process.
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad = {};
        adapter->GetDesc1(&ad);
        fwprintf(f, L"GPU %u: %s  (vendor 0x%04X, LUID %08lX:%08lX, %llu MB video memory%s)\n", i, ad.Description,
                 ad.VendorId, static_cast<unsigned long>(ad.AdapterLuid.HighPart), ad.AdapterLuid.LowPart,
                 static_cast<unsigned long long>(ad.DedicatedVideoMemory >> 20),
                 (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L", software" : L"");
        ComPtr<ID3D11Device> dev;
        const HRESULT dh = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                             D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        fwprintf(f, L"  Direct3D 11 device: %s\n", SUCCEEDED(dh) ? L"ok" : (L"failed " + hr(dh)).c_str());
        bool anyOutput = false;
        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(j, &out) == DXGI_ERROR_NOT_FOUND) break;
            anyOutput = true;
            DXGI_OUTPUT_DESC od = {};
            out->GetDesc(&od);
            const RECT& r = od.DesktopCoordinates;
            fwprintf(f, L"  Screen %s: %ldx%ld at %ld,%ld%s\n", od.DeviceName, r.right - r.left, r.bottom - r.top, r.left,
                     r.top, od.AttachedToDesktop ? L"" : L" (not attached)");
            ComPtr<IDXGIOutput1> out1;
            if (dev && SUCCEEDED(out.As(&out1))) {
                ComPtr<IDXGIOutputDuplication> dup;
                const HRESULT e = out1->DuplicateOutput(dev.Get(), &dup);
                fwprintf(f, L"    duplication: %s%s\n", SUCCEEDED(e) ? L"ok" : (L"failed " + hr(e)).c_str(),
                         e == DXGI_ERROR_UNSUPPORTED ? L"  <- this program runs on another GPU than this screen" : L"");
            }
        }
        if (!anyOutput) fwprintf(f, L"  (no screens wired to this GPU)\n");
        fwprintf(f, L"\n");
    }

    // Hardware H.264 encoders and the GPU each belongs to.
    MFStartup(MF_VERSION);
    {
        MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &count);
        fwprintf(f, L"Hardware H.264 encoders: %u\n", count);
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t name[128] = L"?";
            UINT32 nl = 0;
            acts[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 128, &nl);
            UINT8 blob[sizeof(LUID)] = {};
            UINT32 bn = 0;
            LUID l = {};
            if (SUCCEEDED(acts[i]->GetBlob(MFT_ENUM_ADAPTER_LUID, blob, sizeof(blob), &bn)) && bn == sizeof(LUID))
                memcpy(&l, blob, sizeof(l));
            fwprintf(f, L"  %s  (GPU LUID %08lX:%08lX)\n", name, static_cast<unsigned long>(l.HighPart), l.LowPart);
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }

    // What sharing itself does: the real capture and encoder setup.
    {
        Capture cap;
        const bool dup = cap.Init();
        fwprintf(f, L"\nCapture: device %s, duplication %s, %zu screen(s), %ux%u\n", cap.HasDevice() ? L"ok" : L"none",
                 dup ? L"ok" : (cap.DuplicationUnsupported() ? L"refused (two-GPU laptop: GDI fallback is used)" : L"unavailable"),
                 cap.screenCount(), cap.bgraWidth(), cap.bgraHeight());
        if (cap.HasDevice()) {
            Encoder enc;
            const bool ok = enc.Init(cap.device(), cap.width(), cap.height(), 30, 8'000'000);
            fwprintf(f, L"Encoder: %s\n", ok ? (L"ok, " + enc.Name()).c_str() : L"none accepted this GPU's frames (GDI + software encoder is used)");
        }
    }
    MFShutdown();
    fclose(f);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

}  // namespace rdapp
