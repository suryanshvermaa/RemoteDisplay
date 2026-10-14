#pragma once
// Viewer: one window + one network/decode thread. Receives encrypted UDP, reassembles frames,
// hardware-decodes H.264 and presents it with a tearing-allowed flip swapchain.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <strmif.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "identity.h"
#include "protocol.h"
#include "session.h"

namespace rdv {

using Microsoft::WRL::ComPtr;

constexpr UINT kMsgTitle = WM_APP + 1, kMsgCursor = WM_APP + 2;

inline uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Viewer {
public:
    // Must be called on the thread that runs the message loop. When the window is closed,
    // `closedMsg` is posted to `owner` (WPARAM = Viewer*); the owner then deletes the Viewer.
    // With owner == nullptr, closing the window posts WM_QUIT instead.
    // `identity` (optional) lets the host recognise this PC as a device it already approved.
    static Viewer* Create(HINSTANCE inst, HWND owner, UINT closedMsg, const std::string& host, int port,
                          const std::array<uint8_t, rd::kKeyLen>& psk, const rd::DeviceIdentity* identity = nullptr,
                          const std::string& deviceName = "");

    ~Viewer() {
        run = false;
        if (worker.joinable()) worker.join();
    }

    void SetTitle(const std::string& t) {
        {
            std::lock_guard<std::mutex> l(m_);
            title_ = t;
        }
        if (hwnd) PostMessage(hwnd, kMsgTitle, 0, 0);
    }

    HWND hwnd = nullptr;
    const rd::DeviceIdentity* identity = nullptr;
    std::string deviceName;
    std::atomic<bool> run{true};
    std::atomic<UINT> winW{1280}, winH{720};
    std::thread worker;

    // Remote control: the UI thread produces events, the network thread sends them reliably.
    std::atomic<bool> controlAllowed{false};  // host says the user allows mouse/keyboard control
    std::atomic<UINT> videoW{0}, videoH{0};   // size of the picture being shown (for pointer mapping)
    std::atomic<bool> moveDirty{false};
    std::atomic<uint32_t> movePos{0};         // newest pointer position: x | y << 16 (normalised)
    std::mutex inMu;
    std::deque<rd::InputEvent> inQueue;       // reliable events not yet handed to the network thread

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK LlKeyboardProc(int code, WPARAM w, LPARAM l);

    // Window pixel -> 0..65535 across the picture (letterbox bars clamp to the nearest edge).
    bool MapPoint(int px, int py, uint16_t& nx, uint16_t& ny) const {
        const double vw = videoW, vh = videoH, ww = winW, wh = winH;
        if (vw < 1 || vh < 1 || ww < 1 || wh < 1) return false;
        const double k = std::min(ww / vw, wh / vh);
        const double dw = vw * k, dh = vh * k, left = (ww - dw) / 2, top = (wh - dh) / 2;
        const double fx = std::clamp((px - left) / dw, 0.0, 1.0), fy = std::clamp((py - top) / dh, 0.0, 1.0);
        nx = uint16_t(fx * 65535 + 0.5);
        ny = uint16_t(fy * 65535 + 0.5);
        return true;
    }

    void Push(const rd::InputEvent& e) {
        if (!controlAllowed) return;
        std::lock_guard<std::mutex> l(inMu);
        if (inQueue.size() < 512) inQueue.push_back(e);
    }

    void OnMouseMove(LPARAM l) {
        uint16_t nx, ny;
        if (!controlAllowed || !MapPoint(GET_X_LPARAM(l), GET_Y_LPARAM(l), nx, ny)) return;
        lastNx_ = nx;
        lastNy_ = ny;
        movePos = uint32_t(nx) | (uint32_t(ny) << 16);
        moveDirty = true;
    }

    void OnButton(int idx, bool down, LPARAM l) {
        uint16_t nx, ny;
        if (!controlAllowed || !MapPoint(GET_X_LPARAM(l), GET_Y_LPARAM(l), nx, ny)) return;
        lastNx_ = nx;
        lastNy_ = ny;
        if (down) SetCapture(hwnd);  // keep getting moves/up while dragging outside the window
        btnDown_[idx] = down;
        if (!down && !btnDown_[0] && !btnDown_[1] && !btnDown_[2] && !btnDown_[3] && !btnDown_[4]) ReleaseCapture();
        Push({uint8_t(rd::InputType::Button), uint8_t(idx | (down ? 0x80 : 0)), nx, ny, 0});
    }

    void OnWheel(int delta, bool horizontal, LPARAM l) {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};  // wheel messages carry screen coordinates
        ScreenToClient(hwnd, &pt);
        uint16_t nx, ny;
        if (!controlAllowed || !MapPoint(pt.x, pt.y, nx, ny)) return;
        Push({uint8_t(rd::InputType::Wheel), uint8_t(horizontal ? 1 : 0), nx, ny, int16_t(delta)});
    }

    void ForwardKey(UINT scan, bool extended, UINT vk, bool down) {
        if (scan == 0 || !controlAllowed) return;
        const uint32_t id = scan | (extended ? 1u << 16 : 0);
        if (down) keysDown_.insert(id); else keysDown_.erase(id);
        Push({uint8_t(rd::InputType::Key), uint8_t((down ? 0 : 1) | (extended ? 2 : 0)), uint16_t(scan), uint16_t(vk), 0});
    }

    // Focus lost or window closing: never leave keys or buttons held down on the remote PC.
    void ReleaseHeld() {
        for (uint32_t id : std::set<uint32_t>(keysDown_))
            ForwardKey(id & 0xFFFF, (id >> 16) & 1, 0, false);
        keysDown_.clear();
        for (int b = 0; b < 5; ++b) {
            if (!btnDown_[b]) continue;
            btnDown_[b] = false;
            Push({uint8_t(rd::InputType::Button), uint8_t(b), lastNx_, lastNy_, 0});  // release where it was pressed last
        }
        if (GetCapture() == hwnd) ReleaseCapture();
    }

    void InstallHook() {
        if (g_hook_) return;
        g_hookOwner_ = this;
        g_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LlKeyboardProc, GetModuleHandleW(nullptr), 0);
    }
    void RemoveHook() {
        if (g_hook_ && g_hookOwner_ == this) {
            UnhookWindowsHookEx(g_hook_);
            g_hook_ = nullptr;
            g_hookOwner_ = nullptr;
        }
    }

    HCURSOR cursor_ = nullptr;
    std::set<uint32_t> keysDown_;
    bool btnDown_[5] = {};
    uint16_t lastNx_ = 0, lastNy_ = 0;
    static inline HHOOK g_hook_ = nullptr;
    static inline Viewer* g_hookOwner_ = nullptr;

    void ToggleFullscreen() {
        if (!fullscreen_) {
            prevPlace_.length = sizeof(prevPlace_);
            GetWindowPlacement(hwnd, &prevPlace_);
            prevStyle_ = GetWindowLong(hwnd, GWL_STYLE);
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLong(hwnd, GWL_STYLE, prevStyle_ & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
        } else {
            SetWindowLong(hwnd, GWL_STYLE, prevStyle_);
            SetWindowPlacement(hwnd, &prevPlace_);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        fullscreen_ = !fullscreen_;
    }

    HWND owner_ = nullptr;
    UINT closedMsg_ = 0;
    std::mutex m_;
    std::string title_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT prevPlace_ = {};
    LONG prevStyle_ = 0;
};

// ---------------------------------------------------------------------------------------------
// H.264 hardware decoder (DXVA via the Microsoft H.264 decoder MFT), output stays on the GPU.
class Decoder {
public:
    bool Init(ID3D11Device* dev) {
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr_))) return false;
        if (FAILED(mgr_->ResetDevice(dev, token))) return false;
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&mft_))))
            return false;
        ComPtr<IMFAttributes> a;
        mft_->GetAttributes(&a);
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
        a->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE);
        mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(mgr_.Get()));

        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(mft_.As(&codec))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }

        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(mft_->SetInputType(0, it.Get(), 0))) return false;
        if (!PickOutputType()) return false;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // Feeds one access unit. On success *tex/*slice point at a decoded NV12 surface and
    // `sample` keeps it alive until the next call.
    bool Decode(const uint8_t* data, size_t len, ID3D11Texture2D** tex, UINT* slice) {
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(MFCreateMemoryBuffer(DWORD(len), &mb))) return false;
        BYTE* p = nullptr;
        mb->Lock(&p, nullptr, nullptr);
        memcpy(p, data, len);
        mb->Unlock();
        mb->SetCurrentLength(DWORD(len));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(mb.Get());
        s->SetSampleTime(t_);
        t_ += 166667;
        if (FAILED(mft_->ProcessInput(0, s.Get(), 0))) return false;

        bool got = false;
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob = {};
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                PickOutputType();
                continue;
            }
            if (FAILED(hr)) break;  // NEED_MORE_INPUT
            if (ob.pSample) {
                ComPtr<IMFMediaBuffer> ob0;
                ob.pSample->GetBufferByIndex(0, &ob0);
                ComPtr<IMFDXGIBuffer> dxb;
                if (SUCCEEDED(ob0.As(&dxb))) {
                    ComPtr<ID3D11Texture2D> t;
                    if (SUCCEEDED(dxb->GetResource(IID_PPV_ARGS(&t)))) {
                        dxb->GetSubresourceIndex(slice);
                        cur_ = t;
                        *tex = cur_.Get();
                        got = true;
                    }
                }
                if (curSample_) curSample_->Release();
                curSample_ = ob.pSample;  // keep surface referenced while we render it
            }
            if (ob.pEvents) ob.pEvents->Release();
        }
        return got;
    }

    UINT width() const { return w_; }
    UINT height() const { return h_; }

private:
    bool PickOutputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub;
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w_, &h_);
                return true;
            }
        }
    }

    ComPtr<IMFDXGIDeviceManager> mgr_;
    ComPtr<IMFTransform> mft_;
    ComPtr<ID3D11Texture2D> cur_;
    IMFSample* curSample_ = nullptr;
    UINT w_ = 0, h_ = 0;
    LONGLONG t_ = 0;
};

// ---------------------------------------------------------------------------------------------
class Renderer {
public:
    Renderer(std::atomic<UINT>* w, std::atomic<UINT>* h) : winW_(w), winH_(h) {}

    bool Init(HWND hwnd) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_)))
            return false;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev_.As(&mt))) mt->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        dev_.As(&dxgiDev);
        dxgiDev->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIFactory5> f5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&f5)))
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        tearing_ = tearing;

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = *winW_;
        sd.Height = *winH_;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (FAILED(factory->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &sc_)))
            return false;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        sw_ = sd.Width;
        sh_ = sd.Height;

        if (FAILED(dev_.As(&vdev_))) return false;
        ComPtr<ID3D11DeviceContext> c;
        dev_->GetImmediateContext(&c);
        return SUCCEEDED(c.As(&vctx_));
    }

    ID3D11Device* device() const { return dev_.Get(); }

    void Present(ID3D11Texture2D* nv12, UINT slice, UINT srcW, UINT srcH) {
        UINT ww = *winW_, wh = *winH_;
        if (ww && wh && (ww != sw_ || wh != sh_)) {
            sc_->ResizeBuffers(0, ww, wh, DXGI_FORMAT_UNKNOWN,
                               tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
            sw_ = ww;
            sh_ = wh;
            vp_.Reset();
        }
        if (!vp_ || srcW != vpSrcW_ || srcH != vpSrcH_) BuildProcessor(srcW, srcH);
        if (!vp_) return;

        ComPtr<ID3D11Texture2D> back;
        sc_->GetBuffer(0, IID_PPV_ARGS(&back));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        if (FAILED(vdev_->CreateVideoProcessorOutputView(back.Get(), venum_.Get(), &ovd, &ov))) return;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        if (FAILED(vdev_->CreateVideoProcessorInputView(nv12, venum_.Get(), &ivd, &iv))) return;

        // Letterbox: keep aspect ratio.
        double sx = double(sw_) / srcW, sy = double(sh_) / srcH, k = sx < sy ? sx : sy;
        LONG dw = LONG(srcW * k), dh = LONG(srcH * k);
        RECT dst = {LONG(sw_ - dw) / 2, LONG(sh_ - dh) / 2, LONG(sw_ - dw) / 2 + dw,
                    LONG(sh_ - dh) / 2 + dh};
        RECT src = {0, 0, LONG(srcW), LONG(srcH)};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
        vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
        RECT full = {0, 0, LONG(sw_), LONG(sh_)};
        vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);

        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = iv.Get();
        if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), ov.Get(), 0, 1, &st))) return;
        sc_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }

private:
    void BuildProcessor(UINT srcW, UINT srcH) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = srcW;
        cd.InputHeight = srcH;
        cd.OutputWidth = sw_;
        cd.OutputHeight = sh_;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        vp_.Reset();
        venum_.Reset();
        if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &venum_))) return;
        if (FAILED(vdev_->CreateVideoProcessor(venum_.Get(), 0, &vp_))) return;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};  // BT.709 limited-range YCbCr in
        in.YCbCr_Matrix = 1;
        in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};  // RGB full range out
        vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
        vpSrcW_ = srcW;
        vpSrcH_ = srcH;
    }

    std::atomic<UINT>* winW_;
    std::atomic<UINT>* winH_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> sc_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> venum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    UINT sw_ = 0, sh_ = 0, vpSrcW_ = 0, vpSrcH_ = 0;
    bool tearing_ = false;
};

// ---------------------------------------------------------------------------------------------
// Builds a Windows cursor from the host's shape blob (CursorBlobHeader + BGRA pixels).
inline HCURSOR MakeCursor(const uint8_t* blob, size_t n) {
    if (n < sizeof(rd::CursorBlobHeader)) return nullptr;
    rd::CursorBlobHeader ch;
    memcpy(&ch, blob, sizeof(ch));
    if (ch.w == 0 || ch.h == 0 || ch.w > 256 || ch.h > 256 || n < sizeof(ch) + size_t(ch.w) * ch.h * 4) return nullptr;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = ch.w;
    bi.bV5Height = -LONG(ch.h);  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(ch.w, ch.h, 1, 1, nullptr);
    HCURSOR cur = nullptr;
    if (color && mask && bits) {
        memcpy(bits, blob + sizeof(ch), size_t(ch.w) * ch.h * 4);
        ICONINFO ii = {FALSE, DWORD(std::clamp<int>(ch.hotX, 0, ch.w - 1)), DWORD(std::clamp<int>(ch.hotY, 0, ch.h - 1)), mask, color};
        cur = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ii));
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return cur;
}

// ---------------------------------------------------------------------------------------------
// Software path, for a PC whose GPU has no Direct3D 11 (for example while a display-only driver owns it).
// Microsoft's software H.264 decoder writes NV12 into system memory; GDI draws it. Slower than the GPU path,
// but the picture still shows.
class SoftDecoder {
public:
    bool Init() {
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_))))
            return false;
        ComPtr<IMFAttributes> a;
        if (SUCCEEDED(mft_->GetAttributes(&a)) && a) a->SetUINT32(MF_LOW_LATENCY, TRUE);
        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(mft_->SetInputType(0, it.Get(), 0))) return false;
        PickOutputType();  // may only succeed once the first packet has arrived; Decode retries
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // One access unit in. Returns true with the newest NV12 picture (w x h, tightly packed) in `nv12`.
    bool Decode(const uint8_t* data, size_t len, std::vector<uint8_t>& nv12, UINT& w, UINT& h) {
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(MFCreateMemoryBuffer(DWORD(len), &mb))) return false;
        BYTE* p = nullptr;
        mb->Lock(&p, nullptr, nullptr);
        memcpy(p, data, len);
        mb->Unlock();
        mb->SetCurrentLength(DWORD(len));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(mb.Get());
        s->SetSampleTime(t_);
        t_ += 166667;
        HRESULT ir = mft_->ProcessInput(0, s.Get(), 0);
        if (ir == MF_E_NOTACCEPTING) {  // pictures are waiting: take them, then offer the packet again
            Drain(nv12, w, h);
            ir = mft_->ProcessInput(0, s.Get(), 0);
        }
        if (FAILED(ir)) return false;
        return Drain(nv12, w, h);
    }

private:
    // Takes every picture the decoder has ready. True if at least one came back (the last one is kept).
    bool Drain(std::vector<uint8_t>& nv12, UINT& w, UINT& h) {
        bool got = false;
        for (int guard = 0; guard < 16; ++guard) {
            if (!ready_ && !PickOutputType()) break;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(outBytes_, &buf))) break;
            ComPtr<IMFSample> own;
            MFCreateSample(&own);
            own->AddBuffer(buf.Get());
            MFT_OUTPUT_DATA_BUFFER ob = {};
            ob.pSample = own.Get();
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {  // the picture size changed: pick the new output type
                ready_ = false;
                continue;
            }
            if (FAILED(hr)) break;  // MF_E_TRANSFORM_NEED_MORE_INPUT
            if (CopyNv12(buf.Get(), nv12)) {
                w = w_;
                h = h_;
                got = true;
            }
        }
        return got;
    }

    bool PickOutputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub = {};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w_, &h_);
            UINT32 stride = 0;
            stride_ = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride >= w_ ? stride : w_;
            MFT_OUTPUT_STREAM_INFO si = {};
            mft_->GetOutputStreamInfo(0, &si);
            outBytes_ = si.cbSize ? si.cbSize : DWORD(stride_) * h_ * 2;
            ready_ = true;
            return true;
        }
    }

    // NV12 with the decoder's row stride -> tightly packed NV12.
    bool CopyNv12(IMFMediaBuffer* buf, std::vector<uint8_t>& out) {
        BYTE* p = nullptr;
        DWORD n = 0;
        if (FAILED(buf->Lock(&p, nullptr, &n))) return false;
        const size_t w = w_, h = h_, stride = stride_;
        out.resize(w * h * 3 / 2);
        for (size_t row = 0; row < h; ++row) memcpy(&out[row * w], p + row * stride, w);
        for (size_t row = 0; row < h / 2; ++row) memcpy(&out[w * h + row * w], p + stride * h + row * stride, w);
        buf->Unlock();
        return true;
    }

    ComPtr<IMFTransform> mft_;
    bool ready_ = false;
    UINT w_ = 0, h_ = 0;
    UINT stride_ = 0;
    DWORD outBytes_ = 0;
    LONGLONG t_ = 0;
};

// Draws the software path's NV12 pictures with GDI, letterboxed like the GPU renderer.
class SoftRenderer {
public:
    void Present(HWND hwnd, const std::vector<uint8_t>& nv12, UINT w, UINT h, UINT ww, UINT wh) {
        if (!hwnd || !w || !h || !ww || !wh || nv12.size() < size_t(w) * h * 3 / 2) return;
        bgra_.resize(size_t(w) * h * 4);
        ToBgra(nv12.data(), w, h, bgra_.data());

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = LONG(w);
        bi.bmiHeader.biHeight = -LONG(h);  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        const double k = (std::min)(double(ww) / w, double(wh) / h);
        const LONG dw = LONG(w * k), dh = LONG(h * k);
        const LONG dx = (LONG(ww) - dw) / 2, dy = (LONG(wh) - dh) / 2;
        HDC dc = GetDC(hwnd);
        if (!dc) return;
        PatBlt(dc, 0, 0, LONG(ww), LONG(wh), BLACKNESS);  // the bars around the picture
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, LONG(w), LONG(h), bgra_.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }

private:
    // BT.709 limited-range YCbCr (as the encoder writes it) -> BGRA.
    static void ToBgra(const uint8_t* nv12, UINT w, UINT h, uint8_t* out) {
        const uint8_t* uvPlane = nv12 + size_t(w) * h;
        auto clamp = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (UINT j = 0; j < h; ++j) {
            for (UINT i = 0; i < w; ++i) {
                const int c = nv12[size_t(j) * w + i] - 16;
                const uint8_t* uv = uvPlane + size_t(j / 2) * w + (i & ~1u);
                const int d = uv[0] - 128, e = uv[1] - 128;
                uint8_t* p = out + (size_t(j) * w + i) * 4;
                p[0] = clamp((298 * c + 541 * d + 128) >> 8);             // B
                p[1] = clamp((298 * c - 55 * d - 136 * e + 128) >> 8);    // G
                p[2] = clamp((298 * c + 459 * e + 128) >> 8);             // R
                p[3] = 255;
            }
        }
    }

    std::vector<uint8_t> bgra_;
};

inline void Worker(Viewer* v, std::string hostIp, int port, std::array<uint8_t, rd::kKeyLen> psk) {
    const std::string base = "RemoteDisplay - " + hostIp;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // GPU path first (hardware decoder, Direct3D renderer). Without Direct3D 11 on this PC, fall back to the
    // software decoder and GDI so the picture still shows.
    Renderer r(&v->winW, &v->winH);
    Decoder dec;
    const bool gpu = r.Init(v->hwnd) && dec.Init(r.device());
    SoftDecoder sdec;
    SoftRenderer srend;
    std::vector<uint8_t> softPic;
    if (!gpu) {
        if (!sdec.Init()) {
            v->SetTitle(base + "  -  video decoder or renderer unavailable");
            return;
        }
        v->SetTitle(base + "  -  software video (no Direct3D 11 on this GPU)");
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rb = 8 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rb), sizeof(rb));
    DWORD to = 2;  // ms; lets us send keepalives and input while idle
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to), sizeof(to));
    sockaddr_in host = {};
    host.sin_family = AF_INET;
    host.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, hostIp.c_str(), &host.sin_addr);

    // Reassembly state for the frame currently arriving.
    uint32_t curId = 0;
    bool haveCur = false, curKey = false;
    uint16_t curCount = 0, curGot = 0;
    size_t curBytes = 0;
    std::vector<uint8_t> curBuf;
    std::vector<bool> curSeen;

    bool needKey = true;  // can't decode P-frames until we've seen an IDR
    uint32_t lastDecoded = 0;
    bool haveDecoded = false;
    uint64_t lastHello = 0, lastKeyReq = 0;
    uint64_t statStart = NowUs(), statFrames = 0, statBytes = 0, decUsSum = 0;

    // Remote control: events get consecutive sequence numbers and are resent until the host acks.
    uint32_t inAcked = 0;                 // everything below this was applied by the host
    std::deque<rd::InputEvent> unacked;   // seq = inAcked + index
    uint64_t lastInputSend = 0, lastMoveSend = 0;
    bool inputFresh = false;
    // Cursor shape reassembly.
    uint32_t cId = 0;
    uint16_t cCount = 0, cGot = 0;
    std::vector<uint8_t> cBuf;
    std::vector<bool> cSeen;

    // Secure session state: handshake until the host proves it knows the pairing key.
    rd::Session sess;
    std::unique_ptr<rd::ClientHandshake> hs;
    uint8_t hs1Wire[sizeof(rd::WireHeader) + rd::kHs1Len];
    bool connected = false;
    uint64_t lastHs = 0, lastRx = NowUs(), lastIdent = 0;
    // What the window title says. Only the status changes, so the title is not rewritten every loop.
    const uint64_t startUs = NowUs();
    bool hostSeen = false;  // some handshake answer came back: the device exists and is reachable
    std::string lastStatus;
    auto setStatus = [&](const std::string& s) {
        if (s == lastStatus) return;
        lastStatus = s;
        v->SetTitle(base + "  -  " + s);
    };
    bool gotHostData = false;  // the host answered after we connected: it has seen our Identify

    auto sendCtrl = [&](rd::PacketType t) {
        if (!connected) return;
        rd::PacketHeader ch = {};
        ch.magic = rd::kMagic;
        ch.type = uint8_t(t);
        uint8_t wire[sizeof(rd::WireHeader) + sizeof(ch) + rd::kTagLen];
        size_t wn = sess.Seal(reinterpret_cast<uint8_t*>(&ch), sizeof(ch), wire);
        if (wn) sendto(sock, reinterpret_cast<char*>(wire), int(wn), 0,
                       reinterpret_cast<sockaddr*>(&host), sizeof(host));
    };
    auto sendInner = [&](rd::PacketType t, uint32_t frameId, const void* body, size_t n) {
        uint8_t plain[sizeof(rd::PacketHeader) + 256], wire[sizeof(plain) + sizeof(rd::WireHeader) + rd::kTagLen];
        rd::PacketHeader ph = {};
        ph.magic = rd::kMagic;
        ph.type = uint8_t(t);
        ph.frameId = frameId;
        ph.payloadLen = uint16_t(n);
        memcpy(plain, &ph, sizeof(ph));
        if (n) memcpy(plain + sizeof(ph), body, n);
        size_t wn = sess.Seal(plain, sizeof(ph) + n, wire);
        if (wn) sendto(sock, reinterpret_cast<char*>(wire), int(wn), 0, reinterpret_cast<sockaddr*>(&host), sizeof(host));
    };
    auto sendIdentify = [&]() {
        uint8_t body[rd::kIdentifyLen];
        if (v->identity && v->identity->BuildIdentify(sess, v->deviceName, body))
            sendInner(rd::PacketType::Identify, 0, body, sizeof(body));
    };
    auto resetInput = [&]() {
        inAcked = 0;
        unacked.clear();
        inputFresh = false;
        v->controlAllowed = false;
        v->moveDirty = false;
        std::lock_guard<std::mutex> l(v->inMu);
        v->inQueue.clear();
    };
    auto pumpInput = [&]() {
        if (!connected || !v->controlAllowed) return;
        {
            std::lock_guard<std::mutex> l(v->inMu);
            while (!v->inQueue.empty()) {
                unacked.push_back(v->inQueue.front());
                v->inQueue.pop_front();
                inputFresh = true;
            }
        }
        if (v->moveDirty && NowUs() - lastMoveSend > 3'000) {  // throttled; stays dirty until sent
            v->moveDirty = false;
            const uint32_t pos = v->movePos;
            rd::MoveBody mb = {uint16_t(pos & 0xFFFF), uint16_t(pos >> 16)};
            sendInner(rd::PacketType::InputMove, 0, &mb, sizeof(mb));
            lastMoveSend = NowUs();
        }
        // Send new events at once; otherwise resend the unacked window every 30 ms until acked.
        if (!unacked.empty() && (inputFresh || NowUs() - lastInputSend > 30'000)) {
            uint8_t body[sizeof(rd::InputBody) + rd::kMaxInputEventsPerPacket * sizeof(rd::InputEvent)];
            const size_t cnt = std::min(unacked.size(), rd::kMaxInputEventsPerPacket);
            rd::InputBody ib = {inAcked, uint8_t(cnt)};
            memcpy(body, &ib, sizeof(ib));
            for (size_t i = 0; i < cnt; ++i) memcpy(body + sizeof(ib) + i * sizeof(rd::InputEvent), &unacked[i], sizeof(rd::InputEvent));
            sendInner(rd::PacketType::Input, 0, body, sizeof(ib) + cnt * sizeof(rd::InputEvent));
            lastInputSend = NowUs();
            inputFresh = unacked.size() > cnt;  // more waiting behind this packet
        }
    };
    auto requestKey = [&]() {
        needKey = true;
        if (NowUs() - lastKeyReq > 100'000) {  // at most 10 requests/sec
            sendCtrl(rd::PacketType::KeyReq);
            lastKeyReq = NowUs();
        }
    };

    uint8_t wirebuf[sizeof(rd::WireHeader) + sizeof(rd::PacketHeader) + rd::kMaxPayload + rd::kTagLen + 64];
    std::vector<char> pkt(sizeof(rd::PacketHeader) + rd::kMaxPayload + 64);
    while (v->run) {
        if (connected && NowUs() - lastRx > 3'000'000) {  // host gone or restarted: re-pair
            connected = false;
            sess = rd::Session();
            hs.reset();
            haveCur = false;
            needKey = true;
            resetInput();
            hostSeen = false;
            setStatus("reconnecting...");
        }
        if (!connected && !hostSeen && NowUs() - startUs > 8'000'000) {
            setStatus("device not found: no answer from " + hostIp + " (check the address, that RemoteDisplay is sharing there, and the firewall)");
        }
        if (!connected && NowUs() - lastHs > 300'000) {
            if (!hs) {  // new ephemeral keys; retransmit the SAME Hs1 until Hs2 arrives
                hs = std::make_unique<rd::ClientHandshake>();
                rd::WireHeader oh = {rd::kWireMagic, uint8_t(rd::WireType::Hs1), {}, 0};
                memcpy(hs1Wire, &oh, sizeof(oh));
                if (!hs->Begin(hs1Wire + sizeof(oh))) { hs.reset(); continue; }
            }
            sendto(sock, reinterpret_cast<char*>(hs1Wire), sizeof(hs1Wire), 0,
                   reinterpret_cast<sockaddr*>(&host), sizeof(host));
            lastHs = NowUs();
        }
        if (connected && NowUs() - lastHello > 1'000'000) {
            sendCtrl(rd::PacketType::Hello);
            lastHello = NowUs();
        }
        if (connected && !gotHostData && v->identity && NowUs() - lastIdent > 300'000) {  // until the host answers
            sendIdentify();
            lastIdent = NowUs();
        }
        pumpInput();

        int wn = recv(sock, reinterpret_cast<char*>(wirebuf), int(sizeof(wirebuf)), 0);
        if (wn < int(sizeof(rd::WireHeader))) continue;
        rd::WireHeader wh;
        memcpy(&wh, wirebuf, sizeof(wh));
        if (wh.magic != rd::kWireMagic) continue;
        if (wh.type == uint8_t(rd::WireType::Hs2)) {
            if (!connected && hs && wn == int(sizeof(wh) + rd::kHs2Len)) {
                hostSeen = true;
                if (!hs->Finish(psk.data(), wirebuf + sizeof(wh), sess)) {
                    // The host answered, but could not prove it knows this pairing key.
                    setStatus("pairing key is incorrect: the device was found, but this key does not match it");
                    continue;
                }
                connected = true;
                lastRx = NowUs();
                gotHostData = false;
                if (v->identity) {  // the first authenticated packet says which device this is
                    sendIdentify();
                    lastIdent = NowUs();
                } else {
                    sendCtrl(rd::PacketType::Hello);  // first authenticated packet activates the host session
                }
                lastHello = NowUs();
                needKey = true;
                resetInput();
                setStatus("connected");
            }
            continue;
        }
        if (wh.type != uint8_t(rd::WireType::Data) || !connected) continue;
        size_t plen = 0;
        if (!sess.Open(wirebuf, size_t(wn), reinterpret_cast<uint8_t*>(pkt.data()), plen)) continue;
        lastRx = NowUs();
        gotHostData = true;
        int n = int(plen);
        if (n < int(sizeof(rd::PacketHeader))) continue;
        rd::PacketHeader h;
        memcpy(&h, pkt.data(), sizeof(h));
        if (h.magic != rd::kMagic) continue;
        if (h.type == uint8_t(rd::PacketType::Hello)) {  // host keepalive, carries what the user allows
            const bool allowed = (h.flags & rd::kFlagControlAllowed) != 0;
            v->controlAllowed = allowed;
            if (h.flags & rd::kFlagDenied) setStatus("refused by the other PC");
            else if (h.flags & rd::kFlagAwaitingApproval) setStatus("waiting for approval on the other PC");
            else setStatus(allowed ? "connected, remote control on" : "connected, view only");
            continue;
        }
        if (h.type == uint8_t(rd::PacketType::InputAck)) {
            while (int32_t(h.frameId - inAcked) > 0 && !unacked.empty()) {
                unacked.pop_front();
                ++inAcked;
            }
            continue;
        }
        if (h.type == uint8_t(rd::PacketType::Cursor)) {
            if (h.fragCount == 0 || h.fragIdx >= h.fragCount || h.payloadLen > rd::kMaxPayload ||
                n < int(sizeof(h) + h.payloadLen))
                continue;
            if (h.frameId != cId || h.fragCount != cCount) {  // a different shape: start over
                cId = h.frameId;
                cCount = h.fragCount;
                cGot = 0;
                cBuf.assign(size_t(cCount) * rd::kMaxPayload, 0);
                cSeen.assign(cCount, false);
            }
            if (cSeen[h.fragIdx]) continue;
            memcpy(&cBuf[size_t(h.fragIdx) * rd::kMaxPayload], pkt.data() + sizeof(h), h.payloadLen);
            cSeen[h.fragIdx] = true;
            if (h.fragIdx + 1 == cCount) cBuf.resize(size_t(h.fragIdx) * rd::kMaxPayload + h.payloadLen);
            if (++cGot == cCount) {
                cGot = 0;  // later repeats of the same shape rebuild it, which is harmless
                std::fill(cSeen.begin(), cSeen.end(), false);
                if (HCURSOR cur = MakeCursor(cBuf.data(), cBuf.size()); cur && v->hwnd)
                    PostMessage(v->hwnd, kMsgCursor, 0, reinterpret_cast<LPARAM>(cur));
                cBuf.resize(size_t(cCount) * rd::kMaxPayload);
            }
            continue;
        }
        if (h.type != uint8_t(rd::PacketType::Video)) continue;
        if (h.fragCount == 0 || h.fragIdx >= h.fragCount || h.payloadLen > rd::kMaxPayload ||
            n < int(sizeof(h) + h.payloadLen))
            continue;

        if (!haveCur || h.frameId != curId) {
            if (haveCur && int32_t(h.frameId - curId) < 0) continue;  // stale
            if (haveCur && curGot < curCount) requestKey();           // previous frame incomplete
            haveCur = true;
            curId = h.frameId;
            curCount = h.fragCount;
            curGot = 0;
            curBytes = 0;
            curKey = (h.flags & rd::kFlagKeyframe) != 0;
            curBuf.assign(size_t(curCount) * rd::kMaxPayload, 0);
            curSeen.assign(curCount, false);
        }
        if (h.fragCount != curCount || curSeen[h.fragIdx]) continue;
        memcpy(&curBuf[size_t(h.fragIdx) * rd::kMaxPayload], pkt.data() + sizeof(h), h.payloadLen);
        curSeen[h.fragIdx] = true;
        curBytes += h.payloadLen;
        if (++curGot < curCount) continue;

        // Frame complete.
        haveCur = false;
        if (haveDecoded && curId != lastDecoded + 1 && !curKey) requestKey();
        if (needKey && !curKey) continue;
        if (curKey) needKey = false;

        uint64_t t0 = NowUs();
        if (gpu) {
            ID3D11Texture2D* tex = nullptr;
            UINT slice = 0;
            if (dec.Decode(curBuf.data(), curBytes, &tex, &slice) && tex) {
                v->videoW = dec.width();
                v->videoH = dec.height();
                r.Present(tex, slice, dec.width(), dec.height());
                ++statFrames;
            }
        } else {
            UINT sw = 0, sh = 0;
            if (sdec.Decode(curBuf.data(), curBytes, softPic, sw, sh)) {
                v->videoW = sw;
                v->videoH = sh;
                srend.Present(v->hwnd, softPic, sw, sh, v->winW, v->winH);
                ++statFrames;
            }
        }
        decUsSum += NowUs() - t0;
        lastDecoded = curId;
        haveDecoded = true;
        statBytes += curBytes;

        if (NowUs() - statStart >= 1'000'000) {
            double sec = (NowUs() - statStart) / 1e6;
            char title[160];
            snprintf(title, sizeof(title), "  -  %s  -  %.0f fps  %.1f Mbps  decode %.1f ms",
                     v->controlAllowed ? "controlling" : "view only", statFrames / sec, statBytes * 8 / sec / 1e6,
                     statFrames ? decUsSum / 1000.0 / statFrames : 0.0);
            v->SetTitle(base + title);
            statStart = NowUs();
            statFrames = statBytes = decUsSum = 0;
        }
    }
    closesocket(sock);
    MFShutdown();
}


inline LRESULT CALLBACK Viewer::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams));
        return DefWindowProc(h, m, w, l);
    }
    Viewer* v = reinterpret_cast<Viewer*>(GetWindowLongPtr(h, GWLP_USERDATA));
    if (v) {
        switch (m) {
            case WM_SIZE:
                if (w != SIZE_MINIMIZED) {
                    v->winW = LOWORD(l);
                    v->winH = HIWORD(l);
                }
                return 0;
            case kMsgTitle: {
                std::lock_guard<std::mutex> lk(v->m_);
                SetWindowTextA(h, v->title_.c_str());
                return 0;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const bool down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
                if (w == VK_F11) {  // local shortcut: fullscreen (never forwarded)
                    if (down && !(l & (1 << 30))) v->ToggleFullscreen();
                } else if (w == VK_ESCAPE && v->fullscreen_ && !v->controlAllowed) {
                    if (down) v->ToggleFullscreen();
                } else {
                    v->ForwardKey((l >> 16) & 0xFF, (l >> 24) & 1, UINT(w), down);
                }
                return 0;  // also keeps Alt/F10 from opening the window menu
            }
            case WM_CHAR:
            case WM_SYSCHAR:
            case WM_DEADCHAR:
                return 0;  // the host turns scancodes into characters itself
            case WM_MOUSEMOVE: v->OnMouseMove(l); return 0;
            case WM_LBUTTONDOWN: v->OnButton(0, true, l); return 0;
            case WM_LBUTTONUP: v->OnButton(0, false, l); return 0;
            case WM_RBUTTONDOWN: v->OnButton(1, true, l); return 0;
            case WM_RBUTTONUP: v->OnButton(1, false, l); return 0;
            case WM_MBUTTONDOWN: v->OnButton(2, true, l); return 0;
            case WM_MBUTTONUP: v->OnButton(2, false, l); return 0;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                v->OnButton(GET_XBUTTON_WPARAM(w) == XBUTTON1 ? 3 : 4, m == WM_XBUTTONDOWN, l);
                return TRUE;
            case WM_MOUSEWHEEL: v->OnWheel(GET_WHEEL_DELTA_WPARAM(w), false, l); return 0;
            case WM_MOUSEHWHEEL: v->OnWheel(GET_WHEEL_DELTA_WPARAM(w), true, l); return 0;
            case WM_SETCURSOR:
                if (LOWORD(l) == HTCLIENT) {
                    SetCursor(v->cursor_ ? v->cursor_ : LoadCursor(nullptr, IDC_ARROW));
                    return TRUE;
                }
                break;
            case kMsgCursor: {
                HCURSOR fresh = reinterpret_cast<HCURSOR>(l), old = v->cursor_;
                v->cursor_ = fresh;
                POINT pt;
                RECT rc;
                GetCursorPos(&pt);
                GetClientRect(h, &rc);
                ScreenToClient(h, &pt);
                if (PtInRect(&rc, pt)) SetCursor(fresh);
                if (old) DestroyCursor(old);
                return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(w) == WA_INACTIVE) {
                    v->ReleaseHeld();
                    v->RemoveHook();
                } else {
                    v->InstallHook();
                }
                return 0;
            case WM_KILLFOCUS:
                v->ReleaseHeld();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_DESTROY:
                v->ReleaseHeld();
                v->RemoveHook();
                if (v->cursor_) { DestroyCursor(v->cursor_); v->cursor_ = nullptr; }
                SetWindowLongPtr(h, GWLP_USERDATA, 0);
                v->run = false;
                v->hwnd = nullptr;
                if (v->owner_) PostMessage(v->owner_, v->closedMsg_, reinterpret_cast<WPARAM>(v), 0);
                else PostQuitMessage(0);
                return 0;
        }
    }
    return DefWindowProc(h, m, w, l);
}

// Win key, Alt+Tab, Alt+Esc, Ctrl+Esc and the Menu key are normally eaten by Windows before the
// window sees them. While the viewer is focused and control is on, they go to the remote PC instead.
inline LRESULT CALLBACK Viewer::LlKeyboardProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
        Viewer* v = g_hookOwner_;
        if (v && v->hwnd && v->controlAllowed && !(k->flags & LLKHF_INJECTED) && GetForegroundWindow() == v->hwnd) {
            const bool alt = (k->flags & LLKHF_ALTDOWN) != 0;
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool grab = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN || k->vkCode == VK_APPS ||
                              (k->vkCode == VK_TAB && alt) || (k->vkCode == VK_ESCAPE && (alt || ctrl));
            if (grab) {
                const bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
                v->ForwardKey(k->scanCode, (k->flags & LLKHF_EXTENDED) != 0, k->vkCode, down);
                return 1;  // swallow: do not let the local Start menu / task switcher react
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

inline Viewer* Viewer::Create(HINSTANCE inst, HWND owner, UINT closedMsg, const std::string& host, int port,
                              const std::array<uint8_t, rd::kKeyLen>& psk, const rd::DeviceIdentity* identity,
                              const std::string& deviceName) {
    static const bool registered = [inst] {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = "rdviewer";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        return RegisterClassA(&wc) != 0;
    }();
    if (!registered) return nullptr;

    auto* v = new Viewer;
    v->owner_ = owner;
    v->closedMsg_ = closedMsg;
    v->identity = identity;
    v->deviceName = deviceName;
    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const std::string title = "RemoteDisplay - " + host + "  -  connecting...";
    HWND h = CreateWindowA("rdviewer", title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, v);
    if (!h) {
        delete v;
        return nullptr;
    }
    v->hwnd = h;
    v->worker = std::thread(Worker, v, host, port, psk);
    return v;
}

}  // namespace rdv
