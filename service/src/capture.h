#pragma once
// DXGI Desktop Duplication + GPU BGRA->NV12 conversion (D3D11 video processor).
// Frames never touch system memory: capture -> NV12 texture -> hardware encoder.
//
// Three things are kept apart on purpose:
//  - the device and the conversion pipeline (they must stay alive for the encoder),
//  - the desktop duplication (it fails while the UAC prompt or lock screen is up, and is re-created after),
//  - the pictures handed in from the secure-desktop helper (UploadBgra), which need only the device.
// Everything is built into temporaries and committed only on success, so a failed attempt never leaves
// the object half-built.
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <string>
#include <vector>
#include <windows.h>

#include "protocol.h"

#define CAP_FAIL(what, hr) do { fprintf(stderr, "capture: %s failed (0x%08lx)\n", what, (unsigned long)(hr)); return false; } while (0)

using Microsoft::WRL::ComPtr;

class Capture {
public:
    // Makes sure a device exists and starts desktop duplication on it. True when Grab() can be used.
    // False does not mean the device is gone: HasDevice() stays true and UploadBgra() still works.
    bool Init() {
        if (!dev_ && !BuildDevice(0, 0)) return false;
        if (StartDuplication()) return true;
        if (dev_ && dev_->GetDeviceRemovedReason() != S_OK) return BuildDevice(0, 0) && StartDuplication();  // GPU reset
        return false;
    }

    // Re-creates the device at a new size. Anything built on the old device (the encoder) must follow:
    // Generation() changes so callers can tell.
    bool ResizeTo(UINT w, UINT h) { return BuildDevice(w, h); }

    // Which display to capture: "" = the first one, "virtual" = the RemoteDisplay virtual monitor,
    // or a GDI device name such as \\.\DISPLAY2. Takes effect at the next device build.
    void SelectOutput(const std::wstring& want) { want_ = want; }
    // The GDI device name of the output being captured (e.g. \\.\DISPLAY22).
    const std::wstring& outputName() const { return outputName_; }

    bool HasDevice() const { return dev_ != nullptr; }
    uint32_t Generation() const { return generation_; }

    // Waits up to timeoutMs for a new desktop frame. Returns the NV12 texture, or nullptr on timeout
    // (screen unchanged). Sets `lost` if duplication must be re-initialised.
    ID3D11Texture2D* Grab(UINT timeoutMs, bool& lost) {
        lost = false;
        if (!dupl_) {  // no duplication right now (secure desktop up, or never started)
            lost = true;
            return nullptr;
        }
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return nullptr;
        if (FAILED(hr)) {
            lost = true;
            return nullptr;
        }

        if (info.PointerShapeBufferSize > 0) ReadPointerShape(info.PointerShapeBufferSize);
        if (info.LastPresentTime.QuadPart == 0) {  // only the mouse pointer moved: no new picture
            dupl_->ReleaseFrame();
            return nullptr;
        }

        ComPtr<ID3D11Texture2D> desktop;
        D3D11_TEXTURE2D_DESC fd = {};
        if (FAILED(res.As(&desktop))) {
            dupl_->ReleaseFrame();
            lost = true;
            return nullptr;
        }
        desktop->GetDesc(&fd);
        if (fd.Width != pipe_.bw || fd.Height != pipe_.bh) {  // mode changed; rebuild on the next Init()
            dupl_->ReleaseFrame();
            lost = true;
            return nullptr;
        }
        ctx_->CopyResource(pipe_.bgra.Get(), desktop.Get());
        dupl_->ReleaseFrame();
        return Convert(lost);
    }

    // Uploads a BGRA picture from system memory (the secure-desktop helper's copy) and converts it the
    // same way as a desktop frame. nullptr if there is no device or the size differs from it.
    ID3D11Texture2D* UploadBgra(const uint8_t* src, UINT srcPitch, UINT w, UINT h) {
        if (!pipe_.bgra || !src || w != pipe_.bw || h != pipe_.bh) return nullptr;
        ctx_->UpdateSubresource(pipe_.bgra.Get(), 0, nullptr, src, srcPitch, 0);
        bool lost = false;
        return Convert(lost);
    }

    // Copies the picture last written to the BGRA texture into system memory (secure-desktop helper only).
    bool ReadBgra(std::vector<uint8_t>& out, UINT& pitch) {
        if (!pipe_.bgra) return false;
        if (!staging_) {
            D3D11_TEXTURE2D_DESC td = {};
            pipe_.bgra->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) return false;
        }
        ctx_->CopyResource(staging_.Get(), pipe_.bgra.Get());
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        pitch = m.RowPitch;
        out.resize(size_t(pitch) * pipe_.bh);
        for (UINT y = 0; y < pipe_.bh; ++y)
            memcpy(out.data() + size_t(y) * pitch, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, pitch);
        ctx_->Unmap(staging_.Get(), 0);
        return true;
    }

    ID3D11Texture2D* Last() const { return last_; }
    const RECT& desktopRect() const { return rect_; }

    // The pointer is not part of the captured picture; its shape is tracked separately so the
    // viewer can show it as its own local cursor. True once per shape change.
    bool TakeCursorChange() {
        bool c = cursorChanged_;
        cursorChanged_ = false;
        return c;
    }
    const std::vector<uint8_t>& CursorBlob() const { return cursorBlob_; }  // CursorBlobHeader + BGRA
    uint32_t CursorId() const { return cursorId_; }
    ID3D11Device* device() const { return dev_.Get(); }
    UINT width() const { return pipe_.ow; }     // NV12 (encoder) size, even
    UINT height() const { return pipe_.oh; }
    UINT bgraWidth() const { return pipe_.bw; }  // desktop size
    UINT bgraHeight() const { return pipe_.bh; }

private:
    // The textures and video processor that turn a BGRA picture into NV12. Built whole or not at all.
    struct Pipe {
        ComPtr<ID3D11Texture2D> bgra;               // desktop-sized BGRA picture
        UINT bw = 0, bh = 0;                        // BGRA size
        UINT ow = 0, oh = 0;                        // NV12 size (even)
        std::vector<ComPtr<ID3D11Texture2D>> ring;  // NV12 output ring
        ComPtr<ID3D11VideoDevice> vdev;
        ComPtr<ID3D11VideoContext> vctx;
        ComPtr<ID3D11VideoProcessorEnumerator> venum;
        ComPtr<ID3D11VideoProcessor> vp;
        ComPtr<ID3D11VideoProcessorInputView> inView;
    };

    bool OutputMatches(IDXGIOutput* o) const {
        DXGI_OUTPUT_DESC d = {};
        if (FAILED(o->GetDesc(&d)) || !d.AttachedToDesktop) return false;
        if (want_ != L"virtual") return _wcsicmp(d.DeviceName, want_.c_str()) == 0;
        // The virtual monitor's EDID gives it the monitor ID RDV0001.
        DISPLAY_DEVICEW mon = {sizeof(mon)};
        for (DWORD k = 0; EnumDisplayDevicesW(d.DeviceName, k, &mon, 0); ++k) {
            if (wcsstr(mon.DeviceID, L"RDV0001")) return true;
            mon.cb = sizeof(mon);
        }
        return false;
    }

    // Creates a device on the output's adapter and a pipeline at w x h (0 = desktop size). On success
    // everything is replaced at once; any duplication on the old device is dropped.
    bool BuildDevice(UINT forceW, UINT forceH) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        // The requested output (see SelectOutput), else the first attached output on any adapter.
        ComPtr<IDXGIAdapter1> firstAdapter;
        ComPtr<IDXGIOutput> firstOutput;
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND && !output; ++i) {
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
                if (!firstOutput) {
                    firstAdapter = a;
                    firstOutput = o;
                }
                if (!want_.empty() && OutputMatches(o.Get())) {
                    adapter = a;
                    output = o;
                    break;
                }
                o.Reset();
            }
            a.Reset();
        }
        if (!output) {
            adapter = firstAdapter;
            output = firstOutput;
        }
        if (!adapter || !output) CAP_FAIL("no adapter with an attached output", 0);
        DXGI_OUTPUT_DESC od = {};
        output->GetDesc(&od);
        const RECT rect = od.DesktopCoordinates;
        outputName_ = od.DeviceName;

        ComPtr<IDXGIOutput1> out1;
        if (FAILED(output.As(&out1))) return false;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        if (HRESULT e = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx); FAILED(e))
            CAP_FAIL("D3D11CreateDevice", e);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);

        const UINT w = forceW ? forceW : UINT(rect.right - rect.left);
        const UINT h = forceH ? forceH : UINT(rect.bottom - rect.top);
        Pipe p;
        if (!MakePipe(dev.Get(), ctx.Get(), w, h, p)) return false;

        // Commit. Nothing above touched the object, so a failure leaves the previous state intact.
        dupl_.Reset();  // a duplication belongs to the device it was made on
        dev_ = dev;
        ctx_ = ctx;
        output1_ = out1;
        rect_ = rect;
        pipe_ = std::move(p);
        staging_.Reset();
        ringPos_ = 0;
        last_ = nullptr;
        ++generation_;
        return true;
    }

    // Starts duplication on the current device. Returns false, leaving the device alone, if it cannot
    // (for example while the secure desktop is up). Rebuilds the device if the mode changed.
    bool StartDuplication() {
        dupl_.Reset();
        if (!dev_ || !output1_) return false;
        ComPtr<IDXGIOutputDuplication> d;
        if (FAILED(output1_->DuplicateOutput(dev_.Get(), &d))) return false;
        DXGI_OUTDUPL_DESC dd = {};
        d->GetDesc(&dd);
        if (dd.ModeDesc.Width != pipe_.bw || dd.ModeDesc.Height != pipe_.bh) {  // resolution changed
            d.Reset();
            if (!BuildDevice(dd.ModeDesc.Width, dd.ModeDesc.Height)) return false;
            if (FAILED(output1_->DuplicateOutput(dev_.Get(), &d))) return false;
        }
        dupl_ = d;
        return true;
    }

    bool MakePipe(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h, Pipe& p) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // RENDER_TARGET: video processor input view
        if (HRESULT e = dev->CreateTexture2D(&td, nullptr, &p.bgra); FAILED(e)) CAP_FAIL("create BGRA texture", e);
        p.bw = w;
        p.bh = h;
        p.ow = w & ~1u;  // NV12 needs even dimensions
        p.oh = h & ~1u;

        td.Width = p.ow;
        td.Height = p.oh;
        td.Format = DXGI_FORMAT_NV12;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        p.ring.assign(8, nullptr);
        for (auto& t : p.ring)
            if (HRESULT e = dev->CreateTexture2D(&td, nullptr, &t); FAILED(e)) CAP_FAIL("create NV12 texture", e);

        if (HRESULT e = dev->QueryInterface(IID_PPV_ARGS(&p.vdev)); FAILED(e)) CAP_FAIL("ID3D11VideoDevice", e);
        if (HRESULT e = ctx->QueryInterface(IID_PPV_ARGS(&p.vctx)); FAILED(e)) CAP_FAIL("ID3D11VideoContext", e);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = w;
        cd.InputHeight = h;
        cd.OutputWidth = p.ow;
        cd.OutputHeight = p.oh;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (HRESULT e = p.vdev->CreateVideoProcessorEnumerator(&cd, &p.venum); FAILED(e)) CAP_FAIL("VP enumerator", e);
        if (HRESULT e = p.vdev->CreateVideoProcessor(p.venum.Get(), 0, &p.vp); FAILED(e)) CAP_FAIL("create VP", e);

        // RGB full-range in -> BT.709 limited-range YCbCr out.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
        in.RGB_Range = 0;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        p.vctx->VideoProcessorSetStreamColorSpace(p.vp.Get(), 0, &in);
        p.vctx->VideoProcessorSetOutputColorSpace(p.vp.Get(), &out);
        RECT r = {0, 0, LONG(p.ow), LONG(p.oh)};
        p.vctx->VideoProcessorSetStreamSourceRect(p.vp.Get(), 0, TRUE, &r);
        p.vctx->VideoProcessorSetStreamDestRect(p.vp.Get(), 0, TRUE, &r);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (HRESULT e = p.vdev->CreateVideoProcessorInputView(p.bgra.Get(), p.venum.Get(), &ivd, &p.inView); FAILED(e))
            CAP_FAIL("VP input view", e);
        return true;
    }

    // BGRA (pipe_.bgra) -> NV12 through the video processor, into the next ring texture.
    ID3D11Texture2D* Convert(bool& lost) {
        ID3D11Texture2D* out = pipe_.ring[ringPos_].Get();
        ringPos_ = (ringPos_ + 1) % pipe_.ring.size();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        if (FAILED(pipe_.vdev->CreateVideoProcessorOutputView(out, pipe_.venum.Get(), &ovd, &ov))) {
            lost = true;
            return nullptr;
        }
        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = pipe_.inView.Get();
        if (FAILED(pipe_.vctx->VideoProcessorBlt(pipe_.vp.Get(), ov.Get(), 0, 1, &st))) {
            lost = true;
            return nullptr;
        }
        last_ = out;
        return out;
    }

    // The pointer is not part of the captured picture; its shape is tracked separately.
    void ReadPointerShape(UINT size) {
        std::vector<uint8_t> buf(size);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
        if (FAILED(dupl_->GetFramePointerShape(size, buf.data(), &required, &si))) return;
        const UINT w = si.Width;
        UINT h = si.Height;
        if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;  // AND mask above XOR mask
        if (w == 0 || h == 0 || w > 256 || h > 256) return;

        std::vector<uint8_t> bgra(size_t(w) * h * 4, 0);
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                uint8_t* d = &bgra[(size_t(y) * w + x) * 4];
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    const uint8_t bit = uint8_t(0x80 >> (x & 7));
                    const bool andBit = (buf[size_t(y) * si.Pitch + x / 8] & bit) != 0;
                    const bool xorBit = (buf[size_t(y + h) * si.Pitch + x / 8] & bit) != 0;
                    if (andBit && !xorBit) continue;  // transparent
                    const uint8_t v = (!andBit && xorBit) ? 255 : 0;  // white / black (invert approximated as black)
                    d[0] = d[1] = d[2] = v;
                    d[3] = 255;
                } else {
                    const uint8_t* sp = &buf[size_t(y) * si.Pitch + size_t(x) * 4];
                    d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2];
                    // MASKED_COLOR uses alpha as an XOR mask; treat it as opaque (an approximation).
                    d[3] = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ? sp[3] : 255;
                }
            }
        }
        rd::CursorBlobHeader ch = {int16_t(si.HotSpot.x), int16_t(si.HotSpot.y), uint16_t(w), uint16_t(h)};
        cursorBlob_.resize(sizeof(ch) + bgra.size());
        memcpy(cursorBlob_.data(), &ch, sizeof(ch));
        memcpy(cursorBlob_.data() + sizeof(ch), bgra.data(), bgra.size());
        ++cursorId_;
        cursorChanged_ = true;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutput1> output1_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ComPtr<ID3D11Texture2D> staging_;
    Pipe pipe_;
    size_t ringPos_ = 0;
    ID3D11Texture2D* last_ = nullptr;
    RECT rect_ = {0, 0, 1920, 1080};
    uint32_t generation_ = 0;
    std::wstring want_, outputName_;
    std::vector<uint8_t> cursorBlob_;
    uint32_t cursorId_ = 0;
    bool cursorChanged_ = false;
};
