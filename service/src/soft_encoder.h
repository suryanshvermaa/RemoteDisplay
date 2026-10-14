#pragma once
// Software H.264 encoder (Microsoft's CPU encoder transform) for the GDI capture path. It takes BGRA
// pictures from system memory, converts them to NV12 (BT.709, limited range) at the encoder size, and
// returns H.264 packets. Slower than the hardware encoder; used only when the GPU has no Direct3D 11.
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <strmif.h>
#include <codecapi.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "capture_layout.h"

using Microsoft::WRL::ComPtr;

namespace rdsoft {

// Converts a BGRA picture (sw x sh, `pitch` bytes per row, top-down) to NV12 at ow x oh, with nearest
// neighbour scaling. ow and oh must be even; dst must hold ow * oh * 3 / 2 bytes.
inline void BgraToNv12(const uint8_t* src, UINT sw, UINT sh, UINT pitch, uint8_t* dst, UINT ow, UINT oh) {
    uint8_t* yPlane = dst;
    uint8_t* uvPlane = dst + size_t(ow) * oh;
    for (UINT j = 0; j < oh; ++j) {
        const uint8_t* row = src + size_t(j * sh / oh) * pitch;
        for (UINT i = 0; i < ow; ++i) {
            const uint8_t* p = row + size_t(i * sw / ow) * 4;  // BGRA
            const int b = p[0], g = p[1], r = p[2];
            yPlane[size_t(j) * ow + i] = uint8_t(((47 * r + 157 * g + 16 * b) >> 8) + 16);
            if ((i & 1) == 0 && (j & 1) == 0) {  // one U,V pair per 2x2 block (taken from its top-left pixel)
                const int u = 128 + ((-26 * r - 87 * g + 112 * b) >> 8);
                const int v = 128 + ((112 * r - 102 * g - 10 * b) >> 8);
                uint8_t* uv = uvPlane + size_t(j / 2) * ow + i;
                uv[0] = uint8_t(u < 16 ? 16 : u > 240 ? 240 : u);
                uv[1] = uint8_t(v < 16 ? 16 : v > 240 ? 240 : v);
            }
        }
    }
}

}  // namespace rdsoft

class SoftEncoder {
public:
    ~SoftEncoder() {
        if (started_) MFShutdown();
    }

    // Encodes pictures of srcW x srcH, scaled down to fit 1920x1080 (the software encoder is slow above that).
    bool Init(UINT srcW, UINT srcH, UINT fps, UINT bitrateBps) {
        rdcap::FitInside(srcW, srcH, 1920, 1080, w_, h_);
        fps_ = fps ? fps : 30;
        if (!started_) {
            if (FAILED(MFStartup(MF_VERSION))) return false;
            started_ = true;
        }

        MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                             &in, &out, &acts, &count)) || count == 0) {
            fprintf(stderr, "soft encoder: no software H.264 encoder found\n");
            return false;
        }
        HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft_));
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) return false;

        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        mft_.As(&codec_);

        ComPtr<IMFMediaType> ot;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w_, h_);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(mft_->SetOutputType(0, ot.Get(), 0))) return false;

        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w_, h_);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(mft_->SetInputType(0, it.Get(), 0))) return false;

        MFT_OUTPUT_STREAM_INFO si = {};
        mft_->GetOutputStreamInfo(0, &si);
        providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outBufSize_ = si.cbSize ? si.cbSize : 1 << 20;

        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        sampleTime_ = 0;
        return true;
    }

    UINT width() const { return w_; }
    UINT height() const { return h_; }

    // Encodes one BGRA picture. Returns true with `out` filled when a packet came back. The software
    // encoder may hold a frame back; then this returns false and the caller simply sends nothing.
    bool Encode(const uint8_t* bgra, UINT sw, UINT sh, UINT pitch, bool forceKey, std::vector<uint8_t>& out, bool& isKey) {
        out.clear();
        isKey = false;
        if (!mft_) return false;

        // A fresh buffer per frame: the encoder may still read the previous one.
        const DWORD frameBytes = DWORD(w_) * h_ * 3 / 2;
        ComPtr<IMFMediaBuffer> input;
        if (FAILED(MFCreateMemoryBuffer(frameBytes, &input))) return false;
        BYTE* p = nullptr;
        DWORD maxLen = 0;
        if (FAILED(input->Lock(&p, &maxLen, nullptr))) return false;
        rdsoft::BgraToNv12(bgra, sw, sh, pitch, p, w_, h_);
        input->Unlock();
        input->SetCurrentLength(frameBytes);

        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(input.Get());
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

        Drain(out, isKey);
        return !out.empty();
    }

private:
    // Takes whatever packets the encoder has ready. Stops when it needs more input.
    void Drain(std::vector<uint8_t>& out, bool& isKey) {
        for (int guard = 0; guard < 8; ++guard) {
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
            const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (FAILED(hr)) return;
            IMFSample* s = ob.pSample;
            if (s) {
                UINT32 clean = 0;
                if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) isKey = true;
                ComPtr<IMFMediaBuffer> mb;
                if (SUCCEEDED(s->ConvertToContiguousBuffer(&mb))) {
                    BYTE* bytes = nullptr;
                    DWORD n = 0;
                    if (SUCCEEDED(mb->Lock(&bytes, nullptr, &n))) {
                        out.insert(out.end(), bytes, bytes + n);
                        mb->Unlock();
                    }
                }
            }
            if (providesSamples_ && ob.pSample) ob.pSample->Release();
            if (ob.pEvents) ob.pEvents->Release();
        }
    }

    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codec_;
    UINT w_ = 0, h_ = 0, fps_ = 30;
    DWORD outBufSize_ = 0;
    bool providesSamples_ = false;
    bool started_ = false;
    LONGLONG sampleTime_ = 0;
};
