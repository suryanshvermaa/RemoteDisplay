// Secure-desktop picture path.
//  1) The shared picture: exact copies with a concurrent writer, no torn frames (no GPU needed).
//  2) The real helper: RemoteDisplay.exe --secure-helper --force captures this desktop, and the
//     picture reaches a reader in this process. Forced mode is for testing; the service never passes it.
// The secure desktop itself (UAC / lock screen) cannot be entered from a test; see tests/secure_desktop_manual.md.
#include <algorithm>
#include <cstdio>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

#include "secure_feed.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } else { printf("ok   %s\n", #c); } } while (0)

static const wchar_t kTestMap[] = L"Local\\RemoteDisplaySecureFrameTest";

int main() {
    using namespace rdapp;

    // ---- 1) shared picture: writer and reader in one process, many frames ------------------------
    {
        SecureFeedWriter w;
        CHECK(w.Create(kTestMap), "writer creates the shared picture");
        SecureFeedReader r(kTestMap);
        w.Heartbeat(true);
        CHECK(r.Active(), "reader sees a live helper that reports the secure desktop");
        w.Heartbeat(false);
        CHECK(!r.Active(), "reader sees the helper report the normal desktop");
        w.Heartbeat(true);

        const UINT W = 64, H = 32, P = W * 4;
        std::vector<uint8_t> px(size_t(P) * H);
        std::atomic<bool> done{false};
        std::thread writer([&] {
            for (uint32_t n = 1; n <= 600; ++n) {
                std::fill(px.begin(), px.end(), uint8_t(n));  // every byte of frame n is n
                w.Publish(px.data(), W, H, P);
                std::this_thread::yield();
            }
            done = true;
        });

        int got = 0, torn = 0, wrongSize = 0;
        std::vector<uint8_t> buf;
        SecureFrameInfo info;
        // Read while the writer runs, until the final frame has come through.
        for (int spins = 0; spins < 2000000; ++spins) {
            if (r.ReadLatest(buf, info, false)) {
                ++got;
                if (info.width != W || info.height != H || info.pitch != P) ++wrongSize;
                const uint8_t v = buf[0];
                for (uint8_t b : buf)
                    if (b != v) { ++torn; break; }
                if (info.frameNo == 600) break;
            }
        }
        writer.join();
        CHECK(got > 0, "reader got at least one frame (got %d)", got);
        CHECK(torn == 0, "no frame was torn across two writes (torn %d of %d)", torn, got);
        CHECK(wrongSize == 0, "every frame has the size it was published with");
        CHECK(!r.ReadLatest(buf, info, false), "nothing new after the last frame was read");
        CHECK(r.ReadLatest(buf, info, true), "a forced read returns the current frame again");
        CHECK(info.frameNo == 600, "the last published frame number is 600 (got %ld)", long(info.frameNo));
    }

    // ---- 2) the real helper captures this desktop ------------------------------------------------
    {
        HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, kSecureMapName);
        if (probe) {
            CloseHandle(probe);
            printf("SKIP real helper: a secure-desktop helper is already running; stop it and rerun\n");
        } else {
            wchar_t self[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring exe = self;
            exe = exe.substr(0, exe.find_last_of(L'\\') + 1) + L"RemoteDisplay.exe";
            std::wstring cmd = L"\"" + exe + L"\" --secure-helper --force";
            STARTUPINFOW si = {sizeof(si)};
            PROCESS_INFORMATION pi = {};
            const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CHECK(started, "helper process starts");
            if (started) {
                CloseHandle(pi.hThread);
                SecureFeedReader r;  // default name: the one the helper publishes to
                std::vector<uint8_t> buf;
                SecureFrameInfo info;
                bool gotFrame = false;
                for (int i = 0; i < 200 && !gotFrame; ++i) {  // up to ~10 s
                    Sleep(50);
                    gotFrame = r.ReadLatest(buf, info, true);
                }
                CHECK(gotFrame, "the helper publishes a frame");
                CHECK(r.Active(), "the helper reports itself alive on the secure desktop (forced)");

                Capture cap;
                const bool capOk = cap.Init();
                CHECK(capOk, "a capture pipeline starts in this process");
                if (gotFrame && capOk) {
                    CHECK(info.width == cap.bgraWidth() && info.height == cap.bgraHeight(),
                          "helper frame is %ux%u, matching this desktop %ux%u", info.width, info.height, cap.bgraWidth(), cap.bgraHeight());
                    bool anyColour = false;
                    for (size_t i = 0; i < buf.size() && !anyColour; i += 4)
                        anyColour = buf[i] || buf[i + 1] || buf[i + 2];
                    CHECK(anyColour, "the picture has real content, not a blank frame");
                    CHECK(r.Upload(cap, true) != nullptr, "the picture uploads into the worker's pipeline");
                }
                TerminateProcess(pi.hProcess, 0);
                WaitForSingleObject(pi.hProcess, 3000);
                CloseHandle(pi.hProcess);
            }
        }
    }

    // ---- 3) a helper killed mid-write must not freeze the picture for the next helper ----------
    {
        SecureFeedWriter w;
        const wchar_t name[] = L"Local\\RemoteDisplaySecureFrameTestTorn";
        CHECK(w.Create(name), "writer creates a picture for the torn-write test");
        HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        CHECK(m != nullptr, "the test can open the same picture directly");
        if (m) {
            void* v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            auto* hdr = static_cast<SecureHeader*>(v);
            InterlockedIncrement(&hdr->seq);  // what a helper killed mid-write leaves behind: odd
            const UINT W = 16, H = 8, P = W * 4;
            std::vector<uint8_t> px(size_t(P) * H, 0x5A);
            w.Publish(px.data(), W, H, P);  // the next helper publishes
            SecureFeedReader r(name);
            std::vector<uint8_t> buf;
            SecureFrameInfo info;
            CHECK(r.ReadLatest(buf, info, true), "a reader gets frames again after a killed writer");
            CHECK(buf.size() == px.size() && buf[0] == 0x5A, "and the frame is the one published");
            CHECK((hdr->seq & 1) == 0, "the counter is even (stable) again after the write");
            UnmapViewOfFile(v);
            CloseHandle(m);
        }
    }

    // ---- 4) a capture that never started must not crash, and must not pretend to have a picture ----
    {
        Capture c;
        bool lost = false;
        CHECK(!c.HasDevice(), "a fresh capture has no device");
        CHECK(c.Grab(0, lost) == nullptr && lost, "Grab on a capture that never started reports 'lost' and does not crash");
        std::vector<uint8_t> px(16, 0), out;
        UINT pitch = 0;
        CHECK(c.UploadBgra(px.data(), 8, 2, 2) == nullptr, "UploadBgra without a device returns nothing");
        CHECK(!c.ReadBgra(out, pitch), "ReadBgra without a device returns false");
    }

    // ---- 5) resizing the device keeps the picture path working at the new size -------------------
    {
        Capture c;
        if (c.Init() || c.HasDevice()) {
            const uint32_t gen = c.Generation();
            CHECK(c.ResizeTo(640, 360), "the device can be rebuilt at a new size");
            CHECK(c.Generation() != gen, "the generation changes, so the encoder is rebuilt");
            CHECK(c.bgraWidth() == 640 && c.bgraHeight() == 360, "the pipeline now has the new size");
            std::vector<uint8_t> px(size_t(640) * 4 * 360, 0x80);
            CHECK(c.UploadBgra(px.data(), 640 * 4, 640, 360) != nullptr, "a 640x360 picture uploads at that size");
            CHECK(c.UploadBgra(px.data(), 640 * 4, 1280, 720) == nullptr, "a picture of another size is refused");
        } else {
            printf("SKIP resize test: no device on this machine\n");
        }
    }

    printf(g_fail ? "%d FAILED\n" : "all secure-picture tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
