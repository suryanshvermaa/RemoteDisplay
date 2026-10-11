#pragma once
// EDID (Extended Display Identification Data) for the virtual monitor.
//
// Windows needs a valid EDID to show the monitor in Settings and to offer its modes. The block is built
// here, in plain C++ with no driver dependency, so the bytes can be unit-tested on any machine.
//
// Layout (EDID 1.4, one 128-byte base block):
//   0-7    header            8-9   manufacturer ("RDV")      10-11 product code
//   12-15  serial            16-17 week / year                18-19 version / revision
//   20     video input       21-22 image size (cm)            23    gamma
//   24     features          25-34 chromaticity (sRGB)        35-37 established timings
//   38-53  standard timings  54-71 detailed timing 1 (preferred mode)
//   72-125 three descriptors (name, range limits, filler)    126   extension count
//   127    checksum: all 128 bytes add up to 0 mod 256
#include <array>
#include <cstdint>

namespace rdvirtual {

using EdidBlock = std::array<uint8_t, 128>;

// Timing for one mode, in the units EDID uses.
struct Timing {
    uint32_t pixelClockKhz;  // e.g. 148500
    uint32_t hActive, hBlank, hSyncOffset, hSyncWidth;
    uint32_t vActive, vBlank, vSyncOffset, vSyncWidth;
};

// 1920x1080@60 (CEA-861 "1080p60"): 2200 x 1125 total, 148.5 MHz.
constexpr Timing kTiming1080p60 = {148500, 1920, 280, 88, 44, 1080, 45, 4, 5};

// Builds the base block for a monitor of the given physical size (mm) and preferred timing.
EdidBlock BuildVirtualEdid(uint16_t widthMm, uint16_t heightMm, const Timing& preferred);

// Writes one 18-byte detailed timing descriptor.
void WriteDetailedTiming(uint8_t* out, const Timing& t, uint16_t widthMm, uint16_t heightMm);

}  // namespace rdvirtual
