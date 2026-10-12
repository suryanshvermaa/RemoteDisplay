#pragma once
#include <cstddef>
#include <cstdint>

// UDP wire format shared by host and client. Little-endian.
// One video frame = N fragments, each <= kMaxPayload so a datagram never exceeds
// a typical 1500-byte MTU (avoids IP fragmentation, which is fatal for latency).
namespace rd {

constexpr uint32_t kMagic = 0x32445352;  // 'RSD2'
constexpr uint16_t kDefaultPort = 47800;
// LAN discovery: a plain UDP query to the host port, answered by a host that is sharing with its computer
// name. Nothing secret is sent in either direction.
inline constexpr char kDiscoverQuery[4] = {'R', 'D', 'Q', '1'};
inline constexpr char kDiscoverReply[4] = {'R', 'D', 'A', '1'};
constexpr size_t kMaxPayload = 1200;

enum class PacketType : uint8_t {
    Hello = 1,      // both ways, ~1/s: keepalive (host -> client also carries capability flags)
    Video = 2,      // host -> client: one fragment of an encoded frame
    KeyReq = 3,     // client -> host: loss detected, send an IDR frame now
    Input = 4,      // client -> host: reliable input events (buttons, wheel, keys); see InputBody
    InputMove = 5,  // client -> host: latest mouse position (unreliable, newest wins)
    InputAck = 6,   // host -> client: frameId = next input sequence number it expects
    Cursor = 7,     // host -> client: one fragment of the cursor shape (frameId = shape id)
    Identify = 8,   // client -> host: device public key + signature over this session (identity.h)
};

enum PacketFlags : uint8_t {
    kFlagKeyframe = 1,
    kFlagControlAllowed = 2,  // on host keepalives: the user allows mouse/keyboard control
    kFlagAwaitingApproval = 4,  // on host keepalives: the connection waits for the user to allow it
    kFlagDenied = 8,            // on host keepalives: the user refused this connection
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

// ---- input ------------------------------------------------------------------------------------
// Positions are normalised to 0..65535 across the shared screen so they survive any window size.
enum class InputType : uint8_t { Button = 1, Wheel = 2, Key = 3 };

struct InputEvent {  // 8 bytes
    uint8_t type;    // InputType
    uint8_t flags;   // Button: index 0..4 (L,R,M,X1,X2) | 0x80 when pressed
                     // Wheel:  bit0 = horizontal
                     // Key:    bit0 = key up, bit1 = extended key
    uint16_t x;      // Button/Wheel: position;  Key: hardware scancode
    uint16_t y;      // Button/Wheel: position;  Key: virtual-key code (informational)
    int16_t v;       // Wheel: delta (120 = one notch)
};

struct InputBody {   // payload of PacketType::Input, followed by `count` InputEvents
    uint32_t firstSeq;  // sequence number of the first event; the rest follow consecutively
    uint8_t count;
};

struct MoveBody {    // payload of PacketType::InputMove
    uint16_t x, y;
};

struct CursorBlobHeader {  // start of the reassembled PacketType::Cursor blob, then w*h BGRA pixels
    int16_t hotX, hotY;
    uint16_t w, h;
};
#pragma pack(pop)

constexpr size_t kMaxInputEventsPerPacket = 16;

static_assert(sizeof(PacketHeader) == 24);
static_assert(sizeof(InputEvent) == 8);
static_assert(sizeof(InputBody) == 5);

}  // namespace rd
