// EDID base block for the virtual monitor. See Edid.h for the layout.
#include "Edid.h"

#include <cmath>
#include <cstring>

namespace rdvirtual {
namespace {

// Chromaticity is stored as 10-bit values, the two low bits packed into a shared byte.
uint16_t Chroma10(double v) { return uint16_t(std::lround(v * 1024.0)); }

// Compressed three-letter manufacturer ID: each letter as 1..26 in 5 bits, top bit zero.
void WriteManufacturer(uint8_t* b, char a, char c, char d) {
    const uint16_t id = uint16_t(((a - 'A' + 1) << 10) | ((c - 'A' + 1) << 5) | (d - 'A' + 1));
    b[0] = uint8_t(id >> 8);
    b[1] = uint8_t(id & 0xFF);
}

void WriteText(uint8_t* descriptor, const char* text) {
    // Text descriptor: 13 bytes; a 0x0A terminator then spaces.
    size_t n = std::strlen(text);
    if (n > 12) n = 12;
    std::memcpy(descriptor, text, n);
    descriptor[n] = 0x0A;
    for (size_t i = n + 1; i < 13; ++i) descriptor[i] = 0x20;
}

}  // namespace

void WriteDetailedTiming(uint8_t* out, const Timing& t, uint16_t widthMm, uint16_t heightMm) {
    const uint32_t clk10k = t.pixelClockKhz / 10;
    out[0] = uint8_t(clk10k & 0xFF);
    out[1] = uint8_t(clk10k >> 8);
    out[2] = uint8_t(t.hActive & 0xFF);
    out[3] = uint8_t(t.hBlank & 0xFF);
    out[4] = uint8_t(((t.hActive >> 8) & 0x0F) << 4 | ((t.hBlank >> 8) & 0x0F));
    out[5] = uint8_t(t.vActive & 0xFF);
    out[6] = uint8_t(t.vBlank & 0xFF);
    out[7] = uint8_t(((t.vActive >> 8) & 0x0F) << 4 | ((t.vBlank >> 8) & 0x0F));
    out[8] = uint8_t(t.hSyncOffset & 0xFF);
    out[9] = uint8_t(t.hSyncWidth & 0xFF);
    out[10] = uint8_t(((t.vSyncOffset & 0x0F) << 4) | (t.vSyncWidth & 0x0F));
    out[11] = uint8_t((((t.hSyncOffset >> 8) & 3) << 6) | (((t.hSyncWidth >> 8) & 3) << 4) |
                      (((t.vSyncOffset >> 4) & 3) << 2) | ((t.vSyncWidth >> 4) & 3));
    out[12] = uint8_t(widthMm & 0xFF);
    out[13] = uint8_t(heightMm & 0xFF);
    out[14] = uint8_t(((widthMm >> 8) & 0x0F) << 4 | ((heightMm >> 8) & 0x0F));
    out[15] = 0;  // horizontal border
    out[16] = 0;  // vertical border
    // Digital, separate sync, positive H and V polarity (the CEA-861 1080p60 convention).
    out[17] = 0x1E;
}

EdidBlock BuildVirtualEdid(uint16_t widthMm, uint16_t heightMm, const Timing& preferred) {
    EdidBlock e{};
    // Header
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(e.data(), header, 8);
    WriteManufacturer(&e[8], 'R', 'D', 'V');
    e[10] = 0x01;  // product code (little endian)
    e[11] = 0x00;
    e[12] = 0x01;  // serial number: any fixed value
    e[16] = 0x01;  // manufactured: week 1
    e[17] = 36;    // year 2026 (1990 + 36)
    e[18] = 1;     // EDID version 1
    e[19] = 4;     // revision 4
    e[20] = 0xA5;  // digital input, 8 bits per colour, DisplayPort interface
    e[21] = uint8_t(widthMm / 10);   // image size in cm
    e[22] = uint8_t(heightMm / 10);
    e[23] = 120;   // gamma 2.2: (2.2 * 100) - 100
    e[24] = 0x02;  // preferred timing is the first detailed timing

    // sRGB primaries and white point.
    const uint16_t rx = Chroma10(0.640), ry = Chroma10(0.330);
    const uint16_t gx = Chroma10(0.300), gy = Chroma10(0.600);
    const uint16_t bx = Chroma10(0.150), by = Chroma10(0.060);
    const uint16_t wx = Chroma10(0.3127), wy = Chroma10(0.3290);
    e[25] = uint8_t(((rx & 3) << 6) | ((ry & 3) << 4) | ((gx & 3) << 2) | (gy & 3));
    e[26] = uint8_t(((bx & 3) << 6) | ((by & 3) << 4) | ((wx & 3) << 2) | (wy & 3));
    e[27] = uint8_t(rx >> 2);
    e[28] = uint8_t(ry >> 2);
    e[29] = uint8_t(gx >> 2);
    e[30] = uint8_t(gy >> 2);
    e[31] = uint8_t(bx >> 2);
    e[32] = uint8_t(by >> 2);
    e[33] = uint8_t(wx >> 2);
    e[34] = uint8_t(wy >> 2);

    // Established timings: none. Standard timings: unused slots are 0x01 0x01.
    for (int i = 38; i < 54; i += 2) {
        e[i] = 0x01;
        e[i + 1] = 0x01;
    }

    // Descriptor 1: the preferred detailed timing.
    WriteDetailedTiming(&e[54], preferred, widthMm, heightMm);

    // Descriptor 2: monitor name.
    uint8_t* name = &e[72];
    std::memset(name, 0, 18);
    name[3] = 0xFC;
    WriteText(&name[5], "RD Virtual");

    // Descriptor 3: range limits (vertical 24-75 Hz, horizontal 30-90 kHz, pixel clock up to 150 MHz).
    uint8_t* range = &e[90];
    std::memset(range, 0, 18);
    range[3] = 0xFD;
    range[5] = 24;   // min vertical Hz
    range[6] = 75;   // max vertical Hz
    range[7] = 30;   // min horizontal kHz
    range[8] = 90;   // max horizontal kHz
    range[9] = 15;   // max pixel clock in 10 MHz steps (150 MHz)
    range[10] = 0x00;
    std::memset(&range[11], 0x20, 7);
    range[11] = 0x0A;
    for (int i = 12; i < 18; ++i) range[i] = 0x20;

    // Descriptor 4: dummy (unused).
    uint8_t* dummy = &e[108];
    std::memset(dummy, 0, 18);
    dummy[3] = 0x10;

    e[126] = 0;  // no extension blocks

    uint32_t sum = 0;
    for (int i = 0; i < 127; ++i) sum += e[i];
    e[127] = uint8_t((256 - (sum & 0xFF)) & 0xFF);
    return e;
}

}  // namespace rdvirtual
