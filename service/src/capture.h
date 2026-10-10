#pragma once
// DXGI Desktop Duplication + GPU BGRA->NV12 conversion (D3D11 video processor).
// Frames never touch system memory: capture -> NV12 texture -> hardware encoder.
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vector>

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
        if (!adapter || !output) return false;

        dev_.Reset();
        ctx_.Reset();
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_)))
            return false;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev_.As(&mt))) mt->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) return false;
        dupl_.Reset();
        if (FAILED(output1->DuplicateOutput(dev_.Get(), &dupl_))) return false;
        DXGI_OUTDUPL_DESC dd;
        dupl_->GetDesc(&dd);
        width_ = dd.ModeDesc.Width & ~1u;  // NV12 needs even dimensions
        height_ = dd.ModeDesc.Height & ~1u;
        return InitConverter();
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
    ID3D11Device* device() const { return dev_.Get(); }
    UINT width() const { return width_; }
    UINT height() const { return height_; }

private:
    bool InitConverter() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;

        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // Desktop may be larger than the even-cropped size; CopyResource needs equal sizes.
        DXGI_OUTDUPL_DESC dd;
        dupl_->GetDesc(&dd);
        td.Width = dd.ModeDesc.Width;
        td.Height = dd.ModeDesc.Height;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &bgra_))) return false;

        td.Width = width_;
        td.Height = height_;
        td.Format = DXGI_FORMAT_NV12;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        nv12Ring_.assign(8, nullptr);
        for (auto& t : nv12Ring_)
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t))) return false;

        if (FAILED(dev_.As(&vdev_))) return false;
        ComPtr<ID3D11DeviceContext> c;
        dev_->GetImmediateContext(&c);
        if (FAILED(c.As(&vctx_))) return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = dd.ModeDesc.Width;
        cd.InputHeight = dd.ModeDesc.Height;
        cd.OutputWidth = width_;
        cd.OutputHeight = height_;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &venum_))) return false;
        if (FAILED(vdev_->CreateVideoProcessor(venum_.Get(), 0, &vp_))) return false;

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
        return SUCCEEDED(vdev_->CreateVideoProcessorInputView(bgra_.Get(), venum_.Get(), &ivd, &inView_));
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
};
