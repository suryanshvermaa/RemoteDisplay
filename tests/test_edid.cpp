// Checks the virtual monitor's EDID by decoding it independently of the code that wrote it.
#include <cstdio>
#include <cstring>

#include "Edid.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } else { printf("ok   %s\n", #c); } } while (0)

int main() {
    using namespace rdvirtual;
    const EdidBlock e = BuildVirtualEdid(520, 290, kTiming1080p60);

    CHECK(e.size() == 128, "the base block is 128 bytes");
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    CHECK(std::memcmp(e.data(), header, 8) == 0, "the fixed EDID header is present");

    unsigned sum = 0;
    for (uint8_t b : e) sum += b;
    CHECK((sum & 0xFF) == 0, "all 128 bytes add up to 0 mod 256 (sum %u)", sum);

    CHECK(e[8] == 0x48 && e[9] == 0x96, "manufacturer ID decodes to RDV");
    CHECK(e[18] == 1 && e[19] == 4, "EDID version 1.4");
    CHECK(e[126] == 0, "no extension blocks");

    // Preferred timing (descriptor 1 at 54).
    const uint8_t* d = &e[54];
    const unsigned clk10k = unsigned(d[0]) | (unsigned(d[1]) << 8);
    const unsigned hAct = unsigned(d[2]) | ((d[4] >> 4) << 8);
    const unsigned hBlk = unsigned(d[3]) | ((d[4] & 0x0F) << 8);
    const unsigned vAct = unsigned(d[5]) | ((d[7] >> 4) << 8);
    const unsigned vBlk = unsigned(d[6]) | ((d[7] & 0x0F) << 8);
    const unsigned hSyncOff = unsigned(d[8]) | (((d[11] >> 6) & 3) << 8);
    const unsigned hSyncW = unsigned(d[9]) | (((d[11] >> 4) & 3) << 8);
    const unsigned vSyncOff = (d[10] >> 4) | (((d[11] >> 2) & 3) << 4);
    const unsigned vSyncW = (d[10] & 0x0F) | ((d[11] & 3) << 4);
    CHECK(clk10k == 14850, "pixel clock is 148.5 MHz (got %u x 10 kHz)", clk10k);
    CHECK(hAct == 1920 && vAct == 1080, "preferred mode is 1920x1080 (got %ux%u)", hAct, vAct);
    CHECK(hBlk == 280 && vBlk == 45, "blanking is 280 / 45");
    CHECK(hSyncOff == 88 && hSyncW == 44, "horizontal sync is offset 88, width 44");
    CHECK(vSyncOff == 4 && vSyncW == 5, "vertical sync is offset 4, width 5");
    CHECK(hAct + hBlk == 2200 && vAct + vBlk == 1125, "total timing is 2200 x 1125");
    CHECK(clk10k * 10 == 2200u * 1125u * 60u / 1000u, "pixel clock (kHz) matches 2200 x 1125 at 60 Hz");
    CHECK(d[12] == (520 & 0xFF) && d[14] >> 4 == (520 >> 8), "image width 520 mm is encoded");

    // Descriptor 2: name. Tag 0xFC at byte 3 of the 18-byte block.
    CHECK(e[72 + 3] == 0xFC, "descriptor 2 is a monitor name");
    CHECK(std::memcmp(&e[72 + 5], "RD Virtual\x0A", 11) == 0, "name is 'RD Virtual'");

    // Descriptor 3: range limits.
    CHECK(e[90 + 3] == 0xFD, "descriptor 3 is range limits");
    CHECK(e[90 + 5] == 24 && e[90 + 6] == 75, "vertical range 24-75 Hz");
    CHECK(e[90 + 9] == 15, "pixel clock limit 150 MHz");

    // Standard timings unused.
    CHECK(e[38] == 0x01 && e[39] == 0x01, "unused standard timing slots are 0x01 0x01");

    // Different physical size changes the image-size bytes, and the checksum still holds.
    const EdidBlock big = BuildVirtualEdid(1000, 600, kTiming1080p60);
    unsigned sum2 = 0;
    for (uint8_t b : big) sum2 += b;
    CHECK((sum2 & 0xFF) == 0, "checksum holds for another physical size");
    CHECK(big[21] == 100 && big[22] == 60, "image size is written in cm (100 x 60)");

    printf(g_fail ? "%d FAILED\n" : "all EDID tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
