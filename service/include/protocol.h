#pragma once
#include <cstdint>

// UDP wire format shared by host and client. Little-endian.
// One video frame = N fragments, each <= kMaxPayload so a datagram never exceeds
// a typical 1500-byte MTU (avoids IP fragmentation, which is fatal for latency).
namespace rd {

constexpr uint32_t kMagic = 0x32445352;  // 'RSD2'
constexpr uint16_t kDefaultPort = 47800;
constexpr size_t kMaxPayload = 1200;

enum class PacketType : uint8_t {
    Hello = 1,   // client -> host, every ~1s: registers/keeps alive the peer endpoint
    Video = 2,   // host -> client: one fragment of an encoded frame
    KeyReq = 3,  // client -> host: loss detected, send an IDR frame now
};

enum PacketFlags : uint8_t {
    kFlagKeyframe = 1,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t type;        // PacketType
    uint8_t flags;       // PacketFlags
    uint16_t fragIdx;
    uint16_t fragCount;
    uint16_t payloadLen;
    uint32_t frameId;    // increments by 1 per encoded frame
    uint64_t captureUs;  // host steady-clock microseconds at capture (for latency stats)
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 24);

}  // namespace rd
