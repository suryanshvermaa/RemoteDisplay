// rd-client: UDP -> reassemble -> hardware H.264 decode -> D3D11 present (tearing allowed).
//   rd-client --key <pairing-key> [--host <ip>] [--port <n>]
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "session.h"

using Microsoft::WRL::ComPtr;

namespace {

uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_run{true};
std::atomic<UINT> g_winW{1280}, g_winH{720};
HWND g_hwnd = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) {
                g_winW = LOWORD(l);
                g_winH = HIWORD(l);
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h, m, w, l);
}

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
        sd.Width = g_winW;
        sd.Height = g_winH;
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
        UINT ww = g_winW, wh = g_winH;
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
void Worker(std::string hostIp, int port, std::array<uint8_t, rd::kKeyLen> psk) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    Renderer r;
    Decoder dec;
    if (!r.Init(g_hwnd) || !dec.Init(r.device())) {
        MessageBoxA(nullptr, "Renderer/decoder init failed", "rd-client", MB_ICONERROR);
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rb = 8 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rb), sizeof(rb));
    DWORD to = 4;  // ms; lets us send keepalives while idle
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

    // Secure session state: handshake until the host proves it knows the pairing key.
    rd::Session sess;
    std::unique_ptr<rd::ClientHandshake> hs;
    uint8_t hs1Wire[sizeof(rd::WireHeader) + rd::kHs1Len];
    bool connected = false;
    uint64_t lastHs = 0, lastRx = NowUs();

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
    auto requestKey = [&]() {
        needKey = true;
        if (NowUs() - lastKeyReq > 100'000) {  // at most 10 requests/sec
            sendCtrl(rd::PacketType::KeyReq);
            lastKeyReq = NowUs();
        }
    };

    uint8_t wirebuf[sizeof(rd::WireHeader) + sizeof(rd::PacketHeader) + rd::kMaxPayload + rd::kTagLen + 64];
    std::vector<char> pkt(sizeof(rd::PacketHeader) + rd::kMaxPayload + 64);
    while (g_run) {
        if (connected && NowUs() - lastRx > 3'000'000) {  // host gone or restarted: re-pair
            connected = false;
            sess = rd::Session();
            hs.reset();
            haveCur = false;
            needKey = true;
            SetWindowTextA(g_hwnd, "rd-client  reconnecting...");
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

        int wn = recv(sock, reinterpret_cast<char*>(wirebuf), int(sizeof(wirebuf)), 0);
        if (wn < int(sizeof(rd::WireHeader))) continue;
        rd::WireHeader wh;
        memcpy(&wh, wirebuf, sizeof(wh));
        if (wh.magic != rd::kWireMagic) continue;
        if (wh.type == uint8_t(rd::WireType::Hs2)) {
            if (!connected && hs && wn == int(sizeof(wh) + rd::kHs2Len) &&
                hs->Finish(psk.data(), wirebuf + sizeof(wh), sess)) {
                connected = true;
                lastRx = NowUs();
                sendCtrl(rd::PacketType::Hello);  // first authenticated packet activates the host session
                lastHello = NowUs();
                needKey = true;
                SetWindowTextA(g_hwnd, "rd-client  connected");
            }
            continue;
        }
        if (wh.type != uint8_t(rd::WireType::Data) || !connected) continue;
        size_t plen = 0;
        if (!sess.Open(wirebuf, size_t(wn), reinterpret_cast<uint8_t*>(pkt.data()), plen)) continue;
        lastRx = NowUs();
        int n = int(plen);
        if (n < int(sizeof(rd::PacketHeader))) continue;
        rd::PacketHeader h;
        memcpy(&h, pkt.data(), sizeof(h));
        if (h.magic != rd::kMagic || h.type != uint8_t(rd::PacketType::Video)) continue;
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

        ID3D11Texture2D* tex = nullptr;
        UINT slice = 0;
        uint64_t t0 = NowUs();
        bool ok = dec.Decode(curBuf.data(), curBytes, &tex, &slice);
        decUsSum += NowUs() - t0;
        lastDecoded = curId;
        haveDecoded = true;
        if (ok && tex) {
            r.Present(tex, slice, dec.width(), dec.height());
            ++statFrames;
        }
        statBytes += curBytes;

        if (NowUs() - statStart >= 1'000'000) {
            double sec = (NowUs() - statStart) / 1e6;
            char title[160];
            snprintf(title, sizeof(title), "rd-client  %.0f fps  %.1f Mbps  decode %.1f ms",
                     statFrames / sec, statBytes * 8 / sec / 1e6,
                     statFrames ? decUsSum / 1000.0 / statFrames : 0.0);
            SetWindowTextA(g_hwnd, title);
            statStart = NowUs();
            statFrames = statBytes = decUsSum = 0;
        }
    }
    MFShutdown();
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int show) {
    std::string hostIp = "127.0.0.1";
    int port = rd::kDefaultPort;
    std::string keyText;
    int argc = __argc;
    char** argv = __argv;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--host") hostIp = argv[i + 1];
        else if (a == "--port") port = atoi(argv[i + 1]);
        else if (a == "--key") keyText = argv[i + 1];
    }
    std::array<uint8_t, rd::kKeyLen> psk{};
    if (!rd::ParseKey(keyText, psk.data())) {
        MessageBoxA(nullptr, "Usage: rd-client --key <pairing key shown by rd-host> [--host <ip>] [--port <n>]",
                    "rd-client", MB_ICONERROR);
        return 1;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = "rdclient";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("rdclient", "rd-client", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                           inst, nullptr);

    std::thread worker(Worker, hostIp, port, psk);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_run = false;
    worker.join();
    return 0;
}
