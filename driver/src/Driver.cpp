// RemoteDisplay virtual monitor (IddCx 1.2, UMDF 2). See Driver.h for the overall design.
#include "Driver.h"

#include <dxgi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "Edid.h"

namespace {

// Appends one line to C:\ProgramData\RemoteDisplay\driver.log. A UMDF host that dies leaves very little
// behind, so every step of the startup path writes here.
void Trace(const char* fmt, ...) {
    char line[600];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ", t.wYear, t.wMonth, t.wDay,
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n = int(strlen(line));
    line[n++] = '\r';
    line[n++] = '\n';
    HANDLE f = CreateFileW(L"C:\\ProgramData\\RemoteDisplay\\driver.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, DWORD(n), &written, nullptr);
    CloseHandle(f);
}

LONG WINAPI LogUnhandledException(EXCEPTION_POINTERS* info) {
    Trace("UNHANDLED EXCEPTION code=0x%08lx address=%p", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Stable identity for the monitor's container (groups it with its device in Settings).
const GUID kMonitorContainerId = {0x9a2f8c1e, 0x4b7d, 0x4e2a, {0x9c, 0x3f, 0x6d, 0x1b, 0x8e, 0x7a, 0x5f, 0x20}};

// Physical size of the virtual panel (about a 24-inch 16:9 monitor).
constexpr UINT16 kWidthMm = 520;
constexpr UINT16 kHeightMm = 290;

// Modes the monitor offers.
struct ModeDef {
    UINT32 width, height, refresh;
};
constexpr ModeDef kModes[] = {
    {1920, 1080, 60},
    {1280, 720, 60},
    {1024, 768, 60},
};
constexpr UINT kModeCount = sizeof(kModes) / sizeof(kModes[0]);

// A mode in the OS's signal-info form. As in Microsoft's IddCx sample there is no blanking: the total
// size equals the active size and everything else follows from width, height and refresh, so the
// numbers always agree with each other.
void FillSignalInfo(const ModeDef& m, DISPLAYCONFIG_VIDEO_SIGNAL_INFO& s, bool monitorMode) {
    ZeroMemory(&s, sizeof(s));
    s.activeSize.cx = s.totalSize.cx = m.width;
    s.activeSize.cy = s.totalSize.cy = m.height;
    s.vSyncFreq.Numerator = m.refresh;
    s.vSyncFreq.Denominator = 1;
    s.hSyncFreq.Numerator = m.refresh * m.height;
    s.hSyncFreq.Denominator = 1;
    s.pixelRate = UINT64(m.refresh) * m.width * m.height;
    // The two bit fields are set separately (writing the 32-bit union member would clear the divider).
    // The vertical-sync divider differs by kind: 0 for a monitor mode, at least 1 for a target mode
    // (IddCx.h: "has to have a zero value" for IDDCX_MONITOR_MODE, "cannot be zero" for IDDCX_TARGET_MODE).
    s.AdditionalSignalInfo.videoStandard = 255;  // D3DKMDT_VSS_OTHER: not a broadcast standard
    s.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
    s.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
}

void FillMonitorMode(const ModeDef& m, IDDCX_MONITOR_MODE_ORIGIN origin, IDDCX_MONITOR_MODE& out) {
    ZeroMemory(&out, sizeof(out));
    out.Size = sizeof(out);
    out.Origin = origin;
    FillSignalInfo(m, out.MonitorVideoSignalInfo, /*monitorMode=*/true);
}

// Shared state. The driver hosts one adapter and one monitor per device instance.
std::mutex g_mutex;
IDDCX_ADAPTER g_adapter = nullptr;
IDDCX_MONITOR g_monitor = nullptr;
SwapChainProcessor* g_swapChain = nullptr;

void StopSwapChain() {
    SwapChainProcessor* p = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        p = g_swapChain;
        g_swapChain = nullptr;
    }
    delete p;  // stops and joins the worker thread
}

// ---- adapter -----------------------------------------------------------------------------------------

NTSTATUS EvtAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs) {
    Trace("adapter init finished: status 0x%08lx", pInArgs->AdapterInitStatus);
    if (!NT_SUCCESS(pInArgs->AdapterInitStatus)) return pInArgs->AdapterInitStatus;

    // The EDID must outlive the call; the OS reads it while the monitor exists.
    static const rdvirtual::EdidBlock edid = rdvirtual::BuildVirtualEdid(kWidthMm, kHeightMm, rdvirtual::kTiming1080p60);

    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    // A connector type a real monitor would have. IddCxMonitorCreate rejected INDIRECT_VIRTUAL with
    // STATUS_INVALID_PARAMETER on this OS; Microsoft's IddCx sample uses HDMI.
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = UINT(edid.size());
    info.MonitorDescription.pData = const_cast<uint8_t*>(edid.data());
    info.MonitorContainerId = kMonitorContainerId;

    IDARG_IN_MONITORCREATE createIn = {};
    WDF_OBJECT_ATTRIBUTES monitorAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&monitorAttributes);
    createIn.ObjectAttributes = &monitorAttributes;
    createIn.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE createOut = {};
    // Use the adapter passed to this callback. The global is only set after IddCxAdapterInitAsync returns,
    // and this callback can run before that.
    const NTSTATUS created = IddCxMonitorCreate(adapter, &createIn, &createOut);
    Trace("monitor create: status 0x%08lx", created);
    if (!NT_SUCCESS(created)) return created;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_monitor = createOut.MonitorObject;
    }
    IDARG_OUT_MONITORARRIVAL arrival = {};
    const NTSTATUS arrived = IddCxMonitorArrival(createOut.MonitorObject, &arrival);
    Trace("monitor arrival: status 0x%08lx", arrived);
    return arrived;
}

NTSTATUS EvtAdapterCommitModes(IDDCX_ADAPTER /*adapter*/, const IDARG_IN_COMMITMODES* pInArgs) {
    Trace("commit modes: %u path(s)", pInArgs->PathCount);
    // Nothing to program: the virtual monitor has no hardware mode to set. Windows keeps the chosen
    // mode on the swap chain it assigns to us.
    return STATUS_SUCCESS;
}

// ---- monitor modes -----------------------------------------------------------------------------------

NTSTATUS EvtParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
                                    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs) {
    Trace("parse monitor description: buffer %u", pInArgs->MonitorModeBufferInputCount);
    // The EDID's preferred timing is 1920x1080, which is the first entry.
    pOutArgs->MonitorModeBufferOutputCount = kModeCount;
    pOutArgs->PreferredMonitorModeIdx = 0;
    if (pInArgs->MonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (pInArgs->MonitorModeBufferInputCount < kModeCount || pInArgs->pMonitorModes == nullptr)
        return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < kModeCount; ++i)
        FillMonitorMode(kModes[i], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR, pInArgs->pMonitorModes[i]);
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorGetDefaultModes(IDDCX_MONITOR /*monitor*/, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
                                   IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs) {
    Trace("default description modes: buffer %u", pInArgs->DefaultMonitorModeBufferInputCount);
    pOutArgs->DefaultMonitorModeBufferOutputCount = kModeCount;
    pOutArgs->PreferredMonitorModeIdx = 0;
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (pInArgs->DefaultMonitorModeBufferInputCount < kModeCount || pInArgs->pDefaultMonitorModes == nullptr)
        return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < kModeCount; ++i)
        FillMonitorMode(kModes[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER, pInArgs->pDefaultMonitorModes[i]);
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorQueryTargetModes(IDDCX_MONITOR /*monitor*/, const IDARG_IN_QUERYTARGETMODES* pInArgs,
                                    IDARG_OUT_QUERYTARGETMODES* pOutArgs) {
    Trace("query target modes: buffer %u", pInArgs->TargetModeBufferInputCount);
    pOutArgs->TargetModeBufferOutputCount = kModeCount;
    if (pInArgs->TargetModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (pInArgs->TargetModeBufferInputCount < kModeCount || pInArgs->pTargetModes == nullptr)
        return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < kModeCount; ++i) {
        IDDCX_TARGET_MODE& t = pInArgs->pTargetModes[i];
        ZeroMemory(&t, sizeof(t));
        t.Size = sizeof(t);
        FillSignalInfo(kModes[i], t.TargetVideoSignalInfo.targetVideoSignalInfo, /*monitorMode=*/false);
        t.RequiredBandwidth = t.TargetVideoSignalInfo.targetVideoSignalInfo.pixelRate;
    }
    return STATUS_SUCCESS;
}

// ---- swap chain --------------------------------------------------------------------------------------

NTSTATUS EvtMonitorAssignSwapChain(IDDCX_MONITOR /*monitor*/, const IDARG_IN_SETSWAPCHAIN* pInArgs) {
    Trace("assign swap chain");
    StopSwapChain();  // one swap chain at a time
    auto* p = new SwapChainProcessor(pInArgs->hSwapChain, pInArgs->hNextSurfaceAvailable, pInArgs->RenderAdapterLuid);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_swapChain = p;
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorUnassignSwapChain(IDDCX_MONITOR /*monitor*/) {
    Trace("unassign swap chain");
    StopSwapChain();
    return STATUS_SUCCESS;
}

// ---- device ------------------------------------------------------------------------------------------

// The adapter is created when the device powers up (D0), not in DeviceAdd: IddCx rejects an adapter
// created before the device is started (the host then fails to load the driver with INVALID_PARAMETER).
NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE /*previousState*/) {
    Trace("D0 entry");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_adapter) return STATUS_SUCCESS;  // already created by an earlier power-up
    }

    static const WCHAR kFriendly[] = L"RemoteDisplay Virtual Monitor";
    static const WCHAR kModel[] = L"RemoteDisplay Virtual";
    static const WCHAR kMaker[] = L"RemoteDisplay";
    // IddCx reads these for diagnostics and expects them to be present.
    static IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;

    // Its pipeline rate caps the total of all active modes (driver units).
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.Flags = IDDCX_ADAPTER_FLAGS_NONE;
    caps.MaxDisplayPipelineRate = 4ULL * 148500000ULL;
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = kFriendly;
    caps.EndPointDiagnostics.pEndPointModelName = kModel;
    caps.EndPointDiagnostics.pEndPointManufacturerName = kMaker;
    caps.EndPointDiagnostics.pHardwareVersion = &version;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.StaticDesktopReencodeFrameCount = 0;

    IDARG_IN_ADAPTER_INIT adapterIn = {};
    adapterIn.WdfDevice = device;
    adapterIn.pCaps = &caps;
    IDARG_OUT_ADAPTER_INIT adapterOut = {};
    const NTSTATUS status = IddCxAdapterInitAsync(&adapterIn, &adapterOut);
    Trace("adapter init async: status 0x%08lx", status);
    if (NT_SUCCESS(status)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_adapter = adapterOut.AdapterObject;
    }
    return status;
}

NTSTATUS EvtDriverDeviceAdd(WDFDRIVER /*driver*/, PWDFDEVICE_INIT deviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDeviceD0Entry = EvtDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPower);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished = EvtAdapterInitFinished;
    config.EvtIddCxAdapterCommitModes = EvtAdapterCommitModes;
    config.EvtIddCxParseMonitorDescription = EvtParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = EvtMonitorGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes = EvtMonitorQueryTargetModes;
    config.EvtIddCxMonitorAssignSwapChain = EvtMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = EvtMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &config);
    Trace("device add: IddCxDeviceInitConfig 0x%08lx", status);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    Trace("device add: WdfDeviceCreate 0x%08lx", status);
    if (!NT_SUCCESS(status)) return status;

    status = IddCxDeviceInitialize(device);
    Trace("device add: IddCxDeviceInitialize 0x%08lx", status);
    return status;
}

}  // namespace

// ---- swap chain processor ----------------------------------------------------------------------------

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, HANDLE nextSurfaceAvailable, LUID renderAdapter)
    : swapChain_(swapChain), nextSurfaceAvailable_(nextSurfaceAvailable), renderAdapter_(renderAdapter),
      worker_([this] { Run(); }) {}

SwapChainProcessor::~SwapChainProcessor() { Stop(); }

void SwapChainProcessor::Stop() {
    stop_ = true;
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

void SwapChainProcessor::Run() {
    ProcessFrames();
    // IddCx expects the driver to delete the swap-chain object when it stops processing it, on every path.
    Trace("swap chain processing ended; deleting the swap chain object");
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
}

void SwapChainProcessor::ProcessFrames() {
    // The device must be on the adapter that renders this swap chain.
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Trace("swap chain: CreateDXGIFactory1 failed");
        return;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == renderAdapter_.LowPart && desc.AdapterLuid.HighPart == renderAdapter_.HighPart) break;
        adapter.Reset();
    }
    if (!adapter) return;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.As(&dxgiDevice))) return;

    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = dxgiDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(swapChain_, &setDevice))) return;

    while (!stop_) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER out = {};
        const HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(swapChain_, &out);
        if (hr == E_PENDING) {  // no frame yet: wait for the OS to signal one (or re-check stop_)
            WaitForSingleObject(nextSurfaceAvailable_, 100);
            continue;
        }
        if (FAILED(hr)) break;  // the swap chain is going away

        // The frame is not kept: the streaming service captures the monitor with desktop duplication.
        if (out.MetaData.pSurface) out.MetaData.pSurface->Release();
        IddCxSwapChainFinishedProcessingFrame(swapChain_);
    }
}

// ---- driver entry ------------------------------------------------------------------------------------

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath) {
    SetUnhandledExceptionFilter(LogUnhandledException);
    Trace("DriverEntry (built %s %s)", __DATE__, __TIME__);
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    Trace("WdfDriverCreate: status 0x%08lx", status);
    return status;
}
