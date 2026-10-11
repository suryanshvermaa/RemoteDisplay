#pragma once
// RemoteDisplay virtual monitor: an IddCx (indirect display) UMDF driver.
//
// The driver adds one monitor to Windows. Desktop frames for that monitor are composed by Windows as for
// any display; the driver accepts each frame and releases it again. The streaming service captures the
// monitor with DXGI Desktop Duplication, like any other output, so the driver itself never copies pixels.
//
// Build and install: see driver/README.md.

#define UMDF_VERSION_MAJOR 2
#define UMDF_VERSION_MINOR 33

#include <windows.h>
#include <wdf.h>
#include <IddCx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

// Processes one swap chain: waits for each frame Windows presents to the monitor and hands it back.
class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, HANDLE nextSurfaceAvailable, LUID renderAdapter);
    ~SwapChainProcessor();
    SwapChainProcessor(const SwapChainProcessor&) = delete;
    SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

    void Stop();

private:
    void Run();

    IDDCX_SWAPCHAIN swapChain_;
    HANDLE nextSurfaceAvailable_;
    LUID renderAdapter_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
