#pragma once
// DXGI Desktop Duplication + GPU BGRA->NV12 conversion (D3D11 video processor).
// Frames never touch system memory: capture -> NV12 texture -> hardware encoder.
//
// Every attached screen on the GPU is captured and copied side by side into one desktop-sized texture,
// so one stream shows all of them. Screens that show the same area (mirrored or cloned) are taken once.
// The combined picture is scaled down on the GPU when it is larger than the encoder takes (rdcap::kMax*).
//
// Three things are kept apart on purpose:
//  - the device and the conversion pipeline (they must stay alive for the encoder),
//  - the desktop duplications (they fail while the UAC prompt or lock screen is up, and are re-created after),
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

#include "capture_layout.h"
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

    // Which screen to capture, by GDI name (e.g. \\.\DISPLAY22). Empty = every attached screen on the GPU,
    // side by side. Takes effect at the next Init() or device build.
    void SetOutputName(const std::wstring& name) { outputName_ = name; }

    bool HasDevice() const { return dev_ != nullptr; }
    uint32_t Generation() const { return generation_; }

    // Waits up to timeoutMs for a new picture. Returns the NV12 texture, or nullptr on timeout (nothing
    // changed). Sets `lost` if duplication must be re-initialised.
    ID3D11Texture2D* Grab(UINT timeoutMs, bool& lost) {
        lost = false;
        if (outs_.empty() || !outs_[0].dupl) {  // no duplication right now (secure desktop up, or never started)
            lost = true;
            return nullptr;
        }
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        bool changed = false;
        for (;;) {
            for (auto& o : outs_) {
                if (!Acquire(o, changed)) {
                    lost = true;
                    return nullptr;
                }
            }
            if (changed || GetTickCount64() >= deadline) break;
            Sleep(1);
        }
        if (!changed) return nullptr;
        for (const auto& o : outs_)
            if (!o.tex) return nullptr;  // a screen has not shown a picture yet
        for (const auto& o : outs_) {  // place each screen's newest picture in the combined desktop
            ctx_->CopySubresourceRegion(pipe_.bgra.Get(), 0, UINT(o.rect.left - rect_.left), UINT(o.rect.top - rect_.top),
                                        0, o.tex.Get(), 0, nullptr);
        }
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
    // The area the captured picture covers, in desktop coordinates (all captured screens together).
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
    UINT bgraWidth() const { return pipe_.bw; }  // combined desktop size
    UINT bgraHeight() const { return pipe_.bh; }
    size_t screenCount() const { return outs_.size(); }  // screens in the combined picture

private:
    // The textures and video processor that turn a BGRA picture into NV12. Built whole or not at all.
    struct Pipe {
        ComPtr<ID3D11Texture2D> bgra;               // combined desktop-sized BGRA picture
        UINT bw = 0, bh = 0;                        // BGRA size
        UINT ow = 0, oh = 0;                        // NV12 size (even, within the encoder limit)
        std::vector<ComPtr<ID3D11Texture2D>> ring;  // NV12 output ring
        ComPtr<ID3D11VideoDevice> vdev;
        ComPtr<ID3D11VideoContext> vctx;
        ComPtr<ID3D11VideoProcessorEnumerator> venum;
        ComPtr<ID3D11VideoProcessor> vp;
        ComPtr<ID3D11VideoProcessorInputView> inView;
    };

    // One attached screen: its duplication, and its newest picture copied into its own texture.
    struct Output {
        ComPtr<IDXGIOutput1> out1;
        RECT rect = {};                           // where the screen sits on the desktop
        ComPtr<IDXGIOutputDuplication> dupl;      // null until StartDuplication
        ComPtr<ID3D11Texture2D> tex;              // newest picture of this screen
    };

    // Creates a device on the adapter with the chosen screens and a pipeline for them. On success
    // everything is replaced at once; any duplication on the old device is dropped.
    bool BuildDevice(UINT forceW, UINT forceH) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        // The chosen screens are on the first adapter that has an attached screen (or, for a named
        // screen, the adapter that has it). Screens on other adapters are not part of the picture.
        struct Found {
            ComPtr<IDXGIOutput1> out1;
            RECT rect;
        };
        std::vector<Found> found;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> candidate;
                if (adapter->EnumOutputs(j, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC cd = {};
                candidate->GetDesc(&cd);
                if (!cd.AttachedToDesktop) continue;
                if (!outputName_.empty() && outputName_ != cd.DeviceName) continue;
                ComPtr<IDXGIOutput1> o1;
                if (FAILED(candidate.As(&o1))) continue;
                found.push_back({o1, cd.DesktopCoordinates});
            }
            if (!found.empty()) break;
            adapter.Reset();
        }
        if (!adapter || found.empty()) CAP_FAIL("no adapter with an attached output", 0);

        std::vector<RECT> rects;
        for (const auto& f : found) rects.push_back(f.rect);
        const rdcap::CompositeLayout layout = rdcap::PlanComposite(rects);
        const RECT desktop = layout.desktop;

        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        if (HRESULT e = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx); FAILED(e))
            CAP_FAIL("D3D11CreateDevice", e);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);

        const UINT w = forceW ? forceW : UINT(rdcap::RectW(desktop));
        const UINT h = forceH ? forceH : UINT(rdcap::RectH(desktop));
        UINT ow = 0, oh = 0;
        rdcap::FitInside(w, h, rdcap::kMaxEncodeW, rdcap::kMaxEncodeH, ow, oh);
        Pipe p;
        if (!MakePipe(dev.Get(), ctx.Get(), w, h, ow, oh, p)) return false;

        std::vector<Output> outs;
        for (size_t k : layout.keep) {
            Output o;
            o.out1 = found[k].out1;
            o.rect = found[k].rect;
            outs.push_back(std::move(o));
        }

        // Commit. Nothing above touched the object, so a failure leaves the previous state intact.
        outs_ = std::move(outs);  // their duplications belonged to the old device
        dev_ = dev;
        ctx_ = ctx;
        rect_ = desktop;
        pipe_ = std::move(p);
        staging_.Reset();
        ringPos_ = 0;
        last_ = nullptr;
        ++generation_;
        return true;
    }

    // Starts duplication of every screen on the current device. Returns false, leaving the device alone,
    // if it cannot (for example while the secure desktop is up). Rebuilds the device if the layout changed.
    bool StartDuplication() {
        ResetDuplications();
        if (!dev_ || outs_.empty()) return false;
        // The device was last built at another size (the secure desktop's): go back to the desktop's.
        if (pipe_.bw != UINT(rdcap::RectW(rect_)) || pipe_.bh != UINT(rdcap::RectH(rect_))) {
            if (!BuildDevice(0, 0)) return false;
        }
        bool modeChanged = false;
        if (DuplicateAll(modeChanged)) return true;
        if (!modeChanged) return false;
        // A screen changed resolution since the device was built: rebuild the layout and try again.
        ResetDuplications();
        if (!BuildDevice(0, 0)) return false;
        return DuplicateAll(modeChanged);
    }

    // Duplicates every screen in outs_. False if one fails, or if one shows another size than its area.
    bool DuplicateAll(bool& modeChanged) {
        for (auto& o : outs_) {
            ComPtr<IDXGIOutputDuplication> d;
            if (FAILED(o.out1->DuplicateOutput(dev_.Get(), &d))) return false;
            DXGI_OUTDUPL_DESC dd = {};
            d->GetDesc(&dd);
            if (dd.ModeDesc.Width != UINT(rdcap::RectW(o.rect)) || dd.ModeDesc.Height != UINT(rdcap::RectH(o.rect))) {
                modeChanged = true;
                return false;
            }
            o.dupl = d;
        }
        return true;
    }

    void ResetDuplications() {
        for (auto& o : outs_) {
            o.dupl.Reset();
            o.tex.Reset();
        }
    }

    // Takes the newest picture of one screen into its own texture, if there is a new one. False when
    // duplication has failed and must be re-initialised. `changed` is set when a picture was taken.
    bool Acquire(Output& o, bool& changed) {
        DXGI_OUTDUPL_FRAME_INFO info = {};
        ComPtr<IDXGIResource> res;
        const HRESULT hr = o.dupl->AcquireNextFrame(0, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return true;
        if (FAILED(hr)) return false;

        if (info.PointerShapeBufferSize > 0) ReadPointerShape(o.dupl.Get(), info.PointerShapeBufferSize);
        // Only the mouse pointer moved (no new picture), or nothing to copy: nothing to do.
        if (!res || (info.LastPresentTime.QuadPart == 0 && o.tex)) {
            o.dupl->ReleaseFrame();
            return true;
        }

        ComPtr<ID3D11Texture2D> desktop;
        if (FAILED(res.As(&desktop))) {
            o.dupl->ReleaseFrame();
            return false;
        }
        D3D11_TEXTURE2D_DESC fd = {};
        desktop->GetDesc(&fd);
        if (fd.Width != UINT(rdcap::RectW(o.rect)) || fd.Height != UINT(rdcap::RectH(o.rect))) {  // mode changed
            o.dupl->ReleaseFrame();
            return false;
        }
        if (!o.tex) {  // same size and format as the screen, for the copy
            D3D11_TEXTURE2D_DESC td = fd;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = 0;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &o.tex))) {
                o.dupl->ReleaseFrame();
                return false;
            }
        }
        ctx_->CopyResource(o.tex.Get(), desktop.Get());
        o.dupl->ReleaseFrame();
        changed = true;
        return true;
    }

    bool MakePipe(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h, UINT ow, UINT oh, Pipe& p) {
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
        p.ow = ow;  // NV12 needs even dimensions; rdcap::FitInside gives them
        p.oh = oh;

        // Black until the screens are copied in: an uncovered gap (a layout with holes) shows as black.
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(dev->CreateRenderTargetView(p.bgra.Get(), nullptr, &rtv))) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            ctx->ClearRenderTargetView(rtv.Get(), black);
        }

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
    void ReadPointerShape(IDXGIOutputDuplication* dupl, UINT size) {
        std::vector<uint8_t> buf(size);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
        if (FAILED(dupl->GetFramePointerShape(size, buf.data(), &required, &si))) return;
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
    std::vector<Output> outs_;
    ComPtr<ID3D11Texture2D> staging_;
    Pipe pipe_;
    size_t ringPos_ = 0;
    ID3D11Texture2D* last_ = nullptr;
    RECT rect_ = {0, 0, 1920, 1080};
    uint32_t generation_ = 0;
    std::wstring outputName_;
    std::vector<uint8_t> cursorBlob_;
    uint32_t cursorId_ = 0;
    bool cursorChanged_ = false;
};
