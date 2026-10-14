// Tests for the GDI capture path: BGRA -> NV12 conversion, GDI screen grab, and the software H.264 encoder.
// Needs a desktop session (the screen grab) and Windows' software H.264 encoder (Media Foundation).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gdi_capture.h"
#include "soft_encoder.h"

static int fails = 0;

#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) printf("  ok   %s\n", what);                         \
        else { printf("  FAIL %s (line %d)\n", what, __LINE__); ++fails; } \
    } while (0)

// One BGRA pixel with the given colour (0-255 each).
static void Px(std::vector<uint8_t>& img, size_t idx, uint8_t r, uint8_t g, uint8_t b) {
    img[idx * 4 + 0] = b;
    img[idx * 4 + 1] = g;
    img[idx * 4 + 2] = r;
    img[idx * 4 + 3] = 255;
}

static bool Near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

// Feeds the H.264 packets to Microsoft's software decoder and counts the pictures it returns.
static size_t DecodeCount(const std::vector<std::vector<uint8_t>>& stream, UINT w, UINT h) {
    MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                         &acts, &count)) || count == 0) {
        printf("  info no software decoder\n");
        return 0;
    }
    ComPtr<IMFTransform> dec;
    HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&dec));
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return 0;

    ComPtr<IMFMediaType> it;
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
    if (FAILED(dec->SetInputType(0, it.Get(), 0))) return 0;

    // Pick the NV12 output type the decoder offers.
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> ot;
        if (FAILED(dec->GetOutputAvailableType(0, i, &ot))) return 0;
        GUID sub = {};
        ot->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            if (FAILED(dec->SetOutputType(0, ot.Get(), 0))) return 0;
            break;
        }
    }
    dec->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    dec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    MFT_OUTPUT_STREAM_INFO si = {};
    dec->GetOutputStreamInfo(0, &si);
    const DWORD outBytes = si.cbSize ? si.cbSize : DWORD(w) * h * 3 / 2;

    size_t pictures = 0;
    // Takes every picture the decoder has ready.
    auto drain = [&]() {
        for (;;) {
            ComPtr<IMFMediaBuffer> ob;
            MFCreateMemoryBuffer(outBytes, &ob);
            ComPtr<IMFSample> os;
            MFCreateSample(&os);
            os->AddBuffer(ob.Get());
            MFT_OUTPUT_DATA_BUFFER db = {};
            db.pSample = os.Get();
            DWORD status = 0;
            const HRESULT r = dec->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) db.pEvents->Release();
            if (r == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(r)) {
                printf("  info decoder ProcessOutput 0x%08lx after %zu pictures\n", (unsigned long)r, pictures);
                break;
            }
            ++pictures;
        }
    };
    LONGLONG t = 0;
    for (const auto& pkt : stream) {
        ComPtr<IMFMediaBuffer> mb;
        MFCreateMemoryBuffer(DWORD(pkt.size()), &mb);
        BYTE* p = nullptr;
        mb->Lock(&p, nullptr, nullptr);
        memcpy(p, pkt.data(), pkt.size());
        mb->Unlock();
        mb->SetCurrentLength(DWORD(pkt.size()));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(mb.Get());
        s->SetSampleTime(t);
        t += 10'000'000LL / 15;
        HRESULT ir = dec->ProcessInput(0, s.Get(), 0);
        if (ir == MF_E_NOTACCEPTING) {  // output is waiting: take it, then offer the packet again
            drain();
            ir = dec->ProcessInput(0, s.Get(), 0);
        }
        if (FAILED(ir)) {
            printf("  info decoder ProcessInput 0x%08lx\n", (unsigned long)ir);
            continue;
        }
        drain();
    }
    const size_t beforeDrain = pictures;
    dec->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);  // ask the decoder for the pictures it still holds
    drain();
    printf("  info decoded %zu pictures (%zu after the final drain) from %zu packets\n", pictures, pictures - beforeDrain, stream.size());
    return pictures;
}

int main() {
    printf("BGRA -> NV12 conversion\n");
    {
        // 2x2 picture: red, white, black, blue (row by row). Converted 1:1 to 2x2 NV12. The one chroma pair
        // comes from the top-left pixel, which is red.
        std::vector<uint8_t> img(2 * 2 * 4);
        Px(img, 0, 255, 0, 0);
        Px(img, 1, 255, 255, 255);
        Px(img, 2, 0, 0, 0);
        Px(img, 3, 0, 0, 255);
        std::vector<uint8_t> nv12(2 * 2 * 3 / 2);
        rdsoft::BgraToNv12(img.data(), 2, 2, 2 * 4, nv12.data(), 2, 2);
        const uint8_t* y = nv12.data();
        const uint8_t* uv = nv12.data() + 4;
        CHECK(Near(y[0], 16 + ((47 * 255) >> 8), 0), "red luma uses BT.709 weights");
        CHECK(Near(y[1], 235, 1), "white luma is 235 (limited range)");
        CHECK(Near(y[2], 16, 0), "black luma is 16");
        CHECK(Near(y[3], 16 + ((16 * 255) >> 8), 0), "blue luma uses BT.709 weights");
        CHECK(uv[1] > 180, "red chroma V is high");
        CHECK(uv[0] < 128, "red chroma U is low");
        // White and black alone: chroma stays neutral.
        std::vector<uint8_t> grey(2 * 2 * 4);
        Px(grey, 0, 255, 255, 255);
        Px(grey, 1, 255, 255, 255);
        Px(grey, 2, 255, 255, 255);
        Px(grey, 3, 255, 255, 255);
        rdsoft::BgraToNv12(grey.data(), 2, 2, 2 * 4, nv12.data(), 2, 2);
        CHECK(Near(nv12[4], 128, 2) && Near(nv12[5], 128, 2), "white chroma is neutral");
    }
    {
        // Nearest-neighbour scaling: 4x4 picture, left half red, right half blue, down to 2x2.
        const UINT sw = 4, sh = 4;
        std::vector<uint8_t> img(sw * sh * 4);
        for (UINT j = 0; j < sh; ++j)
            for (UINT i = 0; i < sw; ++i) Px(img, j * sw + i, i < 2 ? 255 : 0, 0, i < 2 ? 0 : 255);
        std::vector<uint8_t> nv12(2 * 2 * 3 / 2);
        rdsoft::BgraToNv12(img.data(), sw, sh, sw * 4, nv12.data(), 2, 2);
        CHECK(nv12[0] > 60 && nv12[1] < 40, "scaled picture keeps the left red, right blue");
    }

    printf("GDI screen grab\n");
    GdiCapture gdi;
    CHECK(gdi.Init(), "GDI capture initialises on the desktop");
    CHECK(gdi.width() > 0 && gdi.height() > 0, "virtual desktop has a size");
    CHECK(gdi.Grab(), "BitBlt of the desktop succeeds");
    CHECK(gdi.Pitch() == gdi.width() * 4, "pitch is 4 bytes per pixel");
    printf("  info screen %ux%u\n", gdi.width(), gdi.height());

    printf("software H.264 encoder\n");
    {
        SoftEncoder enc;
        const bool ok = enc.Init(gdi.width(), gdi.height(), 15, 4'000'000);
        CHECK(ok, "software encoder initialises");
        if (ok) {
            std::vector<uint8_t> out;
            bool isKey = false;
            size_t packets = 0, bytes = 0;
            bool firstKey = false;
            std::vector<std::vector<uint8_t>> stream;
            for (int f = 0; f < 30; ++f) {
                gdi.Grab();
                const bool got = enc.Encode(gdi.Bgra(), gdi.width(), gdi.height(), gdi.Pitch(), f == 0, out, isKey);
                if (got) {
                    if (packets == 0) firstKey = isKey;
                    ++packets;
                    bytes += out.size();
                    stream.push_back(out);
                }
            }
            CHECK(packets > 0, "encoder returns H.264 packets");
            CHECK(firstKey, "the first packet is a keyframe");
            printf("  info %zu packets, %zu bytes for 30 frames at %ux%u\n", packets, bytes, enc.width(), enc.height());

            printf("decode check (Microsoft software H.264 decoder)\n");
            CHECK(DecodeCount(stream, enc.width(), enc.height()) > 0, "the packets decode back to pictures");
        }
    }

    printf(fails ? "%d FAILED\n" : "all GDI path tests passed\n", fails);
    return fails ? 1 : 0;
}
