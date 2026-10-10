#pragma once
// Hardware H.264 encoder via Media Foundation (NVENC / AMF / QuickSync, vendor-neutral).
// Tuned for latency: CBR, no B-frames, low-latency mode, on-demand IDR, drop-if-busy.
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

class Encoder {
public:
    ~Encoder() { if (started_) MFShutdown(); }

    bool Init(ID3D11Device* dev, UINT w, UINT h, UINT fps, UINT bitrateBps) {
        w_ = w; h_ = h; fps_ = fps;
        if (FAILED(MFStartup(MF_VERSION))) return false;
        started_ = true;

        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr_))) return false;
        if (FAILED(mgr_->ResetDevice(dev, token))) return false;

        MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                             MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts,
                             &count)) || count == 0)
            return false;
        HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft_));
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) return false;

        ComPtr<IMFAttributes> attrs;
        mft_->GetAttributes(&attrs);
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(mgr_.Get()));

        mft_.As(&codec_);
        if (codec_) {
            SetU32(CODECAPI_AVLowLatencyMode, TRUE);
            SetU32(CODECAPI_AVEncCommonRealTime, TRUE);
            SetU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            SetU32(CODECAPI_AVEncCommonMeanBitRate, bitrateBps);
            SetU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            SetU32(CODECAPI_AVEncMPVGOPSize, 0xFFFFFFFF);  // IDR only on demand
            SetU32(CODECAPI_AVEncCommonQualityVsSpeed, 0);  // favour speed
        }

        ComPtr<IMFMediaType> ot;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(mft_->SetOutputType(0, ot.Get(), 0))) return false;

        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(mft_->SetInputType(0, it.Get(), 0))) return false;

        MFT_OUTPUT_STREAM_INFO si = {};
        mft_->GetOutputStreamInfo(0, &si);
        providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                          MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outBufSize_ = si.cbSize ? si.cbSize : 1 << 20;

        if (FAILED(mft_.As(&events_))) return false;
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // Encodes one NV12 texture. Returns true with `out` filled when a packet came back.
    // If the encoder is still busy with the previous frame the frame is dropped (returns false).
    bool Encode(ID3D11Texture2D* tex, bool forceKey, std::vector<uint8_t>& out, bool& isKey) {
        out.clear();
        isKey = false;
        Pump(out, isKey);
        if (needInput_ == 0) return !out.empty();

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &buf)))
            return false;
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2d;
        if (SUCCEEDED(buf.As(&b2d))) b2d->GetContiguousLength(&len);
        buf->SetCurrentLength(len ? len : w_ * h_ * 3 / 2);

        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(sampleTime_);
        s->SetSampleDuration(10'000'000LL / fps_);
        sampleTime_ += 10'000'000LL / fps_;

        if (forceKey && codec_) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            codec_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
        if (FAILED(mft_->ProcessInput(0, s.Get(), 0))) return false;
        --needInput_;

        // Low-latency hardware encode returns in a few ms; spin briefly for it.
        for (int i = 0; i < 400 && out.empty(); ++i) {
            Pump(out, isKey);
            if (out.empty()) Sleep(0);
        }
        return !out.empty();
    }

private:
    void SetU32(const GUID& api, UINT32 v) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = v;
        codec_->SetValue(&api, &var);
    }

    void Pump(std::vector<uint8_t>& out, bool& isKey) {
        ComPtr<IMFMediaEvent> ev;
        while (SUCCEEDED(events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev))) {
            MediaEventType t = MEUnknown;
            ev->GetType(&t);
            ev.Reset();
            if (t == METransformNeedInput) ++needInput_;
            else if (t == METransformHaveOutput) Drain(out, isKey);
        }
    }

    void Drain(std::vector<uint8_t>& out, bool& isKey) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ComPtr<IMFSample> own;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> mb;
            MFCreateMemoryBuffer(outBufSize_, &mb);
            MFCreateSample(&own);
            own->AddBuffer(mb.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        if (FAILED(mft_->ProcessOutput(0, 1, &ob, &status))) return;
        IMFSample* s = ob.pSample;
        if (s) {
            UINT32 clean = 0;
            if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) isKey = true;
            ComPtr<IMFMediaBuffer> mb;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&mb))) {
                BYTE* p = nullptr;
                DWORD n = 0;
                if (SUCCEEDED(mb->Lock(&p, nullptr, &n))) {
                    out.insert(out.end(), p, p + n);
                    mb->Unlock();
                }
            }
        }
        if (providesSamples_ && ob.pSample) ob.pSample->Release();
        if (ob.pEvents) ob.pEvents->Release();
    }

    ComPtr<IMFDXGIDeviceManager> mgr_;
    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFMediaEventGenerator> events_;
    UINT w_ = 0, h_ = 0, fps_ = 60;
    DWORD outBufSize_ = 0;
    bool providesSamples_ = false;
    bool started_ = false;
    int needInput_ = 0;
    LONGLONG sampleTime_ = 0;
};
