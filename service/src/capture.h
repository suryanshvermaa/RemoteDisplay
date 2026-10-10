#pragma once
// DXGI Desktop Duplication + GPU BGRA->NV12 conversion (D3D11 video processor).
// Frames never touch system memory: capture -> NV12 texture -> hardware encoder.
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "protocol.h"

#define CAP_FAIL(what, hr) do { fprintf(stderr, "capture: %s failed (0x%08lx)\n", what, (unsigned long)(hr)); return false; } while (0)

using Microsoft::WRL::ComPtr;

class Capture {
public:
    bool Init() {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        // First adapter that actually owns an attached output (matters on hybrid-GPU laptops).
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            if (SUCCEEDED(adapter->EnumOutputs(0, &output))) break;
            adapter.Reset();
        }
        if (!adapter || !output) CAP_FAIL("no adapter with an attached output", 0);
        DXGI_OUTPUT_DESC od = {};
        output->GetDesc(&od);
        rect_ = od.DesktopCoordinates;

        dev_.Reset();
        ctx_.Reset();
        if (HRESULT e = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_); FAILED(e))
            CAP_FAIL("D3D11CreateDevice", e);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev_.As(&mt))) mt->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) return false;
        dupl_.Reset();
        if (HRESULT e = output1->DuplicateOutput(dev_.Get(), &dupl_); FAILED(e)) CAP_FAIL("DuplicateOutput", e);
        DXGI_OUTDUPL_DESC dd;
        dupl_->GetDesc(&dd);
        width_ = dd.ModeDesc.Width & ~1u;  // NV12 needs even dimensions
        height_ = dd.ModeDesc.Height & ~1u;
        if (!InitConverter()) CAP_FAIL("video processor setup", 0);
        return true;
    }

    // Waits up to timeoutMs for a new desktop frame. Returns the NV12 texture, or nullptr on
    // timeout (screen unchanged). Sets `lost` if duplication must be re-initialised.
    ID3D11Texture2D* Grab(UINT timeoutMs, bool& lost) {
        lost = false;
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return nullptr;
        if (FAILED(hr)) { lost = true; return nullptr; }

        if (info.PointerShapeBufferSize > 0) ReadPointerShape(info.PointerShapeBufferSize);
        if (info.LastPresentTime.QuadPart == 0) {  // only the mouse pointer moved: no new picture
            dupl_->ReleaseFrame();
            return nullptr;
        }

        ComPtr<ID3D11Texture2D> desktop;
        res.As(&desktop);
        ctx_->CopyResource(bgra_.Get(), desktop.Get());
        dupl_->ReleaseFrame();

        ID3D11Texture2D* out = nv12Ring_[ringPos_].Get();
        ringPos_ = (ringPos_ + 1) % nv12Ring_.size();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        if (FAILED(vdev_->CreateVideoProcessorOutputView(out, venum_.Get(), &ovd, &ov))) {
            lost = true;
            return nullptr;
        }
        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = inView_.Get();
        if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), ov.Get(), 0, 1, &st))) {
            lost = true;
            return nullptr;
        }
        last_ = out;
        return out;
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
    UINT width() const { return width_; }
    UINT height() const { return height_; }

private:
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

    bool InitConverter() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;

        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        // Desktop may be larger than the even-cropped size; CopyResource needs equal sizes.
        DXGI_OUTDUPL_DESC dd;
        dupl_->GetDesc(&dd);
        td.Width = dd.ModeDesc.Width;
        td.Height = dd.ModeDesc.Height;
        if (HRESULT e = dev_->CreateTexture2D(&td, nullptr, &bgra_); FAILED(e)) CAP_FAIL("create BGRA texture", e);

        td.Width = width_;
        td.Height = height_;
        td.Format = DXGI_FORMAT_NV12;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        nv12Ring_.assign(8, nullptr);
        for (auto& t : nv12Ring_)
            if (HRESULT e = dev_->CreateTexture2D(&td, nullptr, &t); FAILED(e)) CAP_FAIL("create NV12 texture", e);

        if (HRESULT e = dev_.As(&vdev_); FAILED(e)) CAP_FAIL("ID3D11VideoDevice", e);
        ComPtr<ID3D11DeviceContext> c;
        dev_->GetImmediateContext(&c);
        if (HRESULT e = c.As(&vctx_); FAILED(e)) CAP_FAIL("ID3D11VideoContext", e);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = dd.ModeDesc.Width;
        cd.InputHeight = dd.ModeDesc.Height;
        cd.OutputWidth = width_;
        cd.OutputHeight = height_;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (HRESULT e = vdev_->CreateVideoProcessorEnumerator(&cd, &venum_); FAILED(e)) CAP_FAIL("VP enumerator", e);
        if (HRESULT e = vdev_->CreateVideoProcessor(venum_.Get(), 0, &vp_); FAILED(e)) CAP_FAIL("create VP", e);

        // RGB full-range in -> BT.709 limited-range YCbCr out.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
        in.RGB_Range = 0;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
        RECT r = {0, 0, LONG(width_), LONG(height_)};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &r);
        vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &r);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (HRESULT e = vdev_->CreateVideoProcessorInputView(bgra_.Get(), venum_.Get(), &ivd, &inView_); FAILED(e)) CAP_FAIL("VP input view", e);
        return true;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ComPtr<ID3D11Texture2D> bgra_;
    std::vector<ComPtr<ID3D11Texture2D>> nv12Ring_;
    size_t ringPos_ = 0;
    ID3D11Texture2D* last_ = nullptr;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> venum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
    UINT width_ = 0, height_ = 0;
    RECT rect_ = {0, 0, 1920, 1080};
    std::vector<uint8_t> cursorBlob_;
    uint32_t cursorId_ = 0;
    bool cursorChanged_ = false;
};
