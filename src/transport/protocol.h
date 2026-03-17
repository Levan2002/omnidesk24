#pragma once

#include "core/types.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif

namespace omnidesk {

// ---- Serialization helpers (network byte order) ----

inline void writeU16(uint8_t* buf, uint16_t val) {
    uint16_t net = htons(val);
    std::memcpy(buf, &net, 2);
}

inline void writeU32(uint8_t* buf, uint32_t val) {
    uint32_t net = htonl(val);
    std::memcpy(buf, &net, 4);
}

inline uint16_t readU16(const uint8_t* buf) {
    uint16_t net;
    std::memcpy(&net, buf, 2);
    return ntohs(net);
}

inline uint32_t readU32(const uint8_t* buf) {
    uint32_t net;
    std::memcpy(&net, buf, 4);
    return ntohl(net);
}

// Wire format magic number: "OMND"
constexpr uint32_t PROTOCOL_MAGIC = 0x4F4D4E44;

// Current protocol version
constexpr uint16_t PROTOCOL_VERSION = 1;

// Maximum UDP payload size (MTU-safe)
constexpr size_t MAX_UDP_PAYLOAD = 1200;

// Message types for control channel
enum class MessageType : uint16_t {
    HELLO            = 0x0001,
    AUTH             = 0x0002,
    CONFIG           = 0x0003,
    KEY_FRAME_REQ    = 0x0004,
    INPUT_EVENT      = 0x0005,
    CLIPBOARD        = 0x0006,
    CURSOR_UPDATE    = 0x0007,
    QUALITY_REPORT   = 0x0008,
    CONNECT_REQUEST  = 0x0009,
    CONNECT_ACCEPT   = 0x000A,
    CONNECT_REJECT   = 0x000B,
    HEARTBEAT        = 0x000C,
    BYE              = 0x000D,
    VIDEO_DATA       = 0x000E,
    RELAY_DATA       = 0x000F,  // Data relayed through signaling server
};

// Control channel header (12 bytes), prefixed to every TCP message
struct ControlHeader {
    uint32_t magic   = PROTOCOL_MAGIC;
    uint16_t version = PROTOCOL_VERSION;
    uint16_t type    = 0;
    uint32_t length  = 0;  // payload length following this header

    static constexpr size_t SIZE = 12;

    void serialize(uint8_t* buf) const {
        writeU32(buf + 0, magic);
        writeU16(buf + 4, version);
        writeU16(buf + 6, type);
        writeU32(buf + 8, length);
    }

    static ControlHeader deserialize(const uint8_t* buf) {
        ControlHeader h;
        h.magic   = readU32(buf + 0);
        h.version = readU16(buf + 4);
        h.type    = readU16(buf + 6);
        h.length  = readU32(buf + 8);
        return h;
    }

    bool valid() const {
        return magic == PROTOCOL_MAGIC && version == PROTOCOL_VERSION;
    }
};

// Video data header (12 bytes), prepended to each UDP video fragment
struct VideoHeader {
    uint32_t frameId   = 0;
    uint16_t fragId    = 0;
    uint16_t fragCount = 0;
    uint16_t fecGroup  = 0;
    uint8_t  flags     = 0;
    uint8_t  rectCount = 0;

    static constexpr size_t SIZE = 12;

    void serialize(uint8_t* buf) const {
        writeU32(buf + 0, frameId);
        writeU16(buf + 4, fragId);
        writeU16(buf + 6, fragCount);
        writeU16(buf + 8, fecGroup);
        buf[10] = flags;
        buf[11] = rectCount;
    }

    static VideoHeader deserialize(const uint8_t* buf) {
        VideoHeader h;
        h.frameId   = readU32(buf + 0);
        h.fragId    = readU16(buf + 4);
        h.fragCount = readU16(buf + 6);
        h.fecGroup  = readU16(buf + 8);
        h.flags     = buf[10];
        h.rectCount = buf[11];
        return h;
    }
};

// Video header flags
constexpr uint8_t FLAG_KEYFRAME            = 0x01;
constexpr uint8_t FLAG_TEMPORAL_LAYER_MASK = 0x06;
constexpr uint8_t FLAG_HAS_DIRTY_RECTS    = 0x08;

inline uint8_t getTemporalLayer(uint8_t flags) {
    return (flags & FLAG_TEMPORAL_LAYER_MASK) >> 1;
}

inline uint8_t setTemporalLayer(uint8_t flags, uint8_t layer) {
    return (flags & ~FLAG_TEMPORAL_LAYER_MASK) | ((layer & 0x03) << 1);
}

// Input event types
enum class InputType : uint8_t {
    MOUSE_MOVE    = 0,
    MOUSE_DOWN    = 1,
    MOUSE_UP      = 2,
    MOUSE_SCROLL  = 3,
    KEY_DOWN      = 4,
    KEY_UP        = 5,
};

// Input event struct for mouse/keyboard events
struct InputEvent {
    InputType type     = InputType::MOUSE_MOVE;
    int32_t   x        = 0;
    int32_t   y        = 0;
    uint8_t   button   = 0;   // mouse button (0=left, 1=right, 2=middle)
    uint32_t  scancode = 0;   // keyboard scan code
    bool      pressed  = false;

    static constexpr size_t SIZE = 16;

    void serialize(uint8_t* buf) const {
        buf[0] = static_cast<uint8_t>(type);
        buf[1] = pressed ? 1 : 0;
        buf[2] = button;
        buf[3] = 0; // padding
        writeU32(buf + 4, static_cast<uint32_t>(x));
        writeU32(buf + 8, static_cast<uint32_t>(y));
        writeU32(buf + 12, scancode);
    }

    static InputEvent deserialize(const uint8_t* buf) {
        InputEvent ev;
        ev.type     = static_cast<InputType>(buf[0]);
        ev.pressed  = buf[1] != 0;
        ev.button   = buf[2];
        ev.x        = static_cast<int32_t>(readU32(buf + 4));
        ev.y        = static_cast<int32_t>(readU32(buf + 8));
        ev.scancode = readU32(buf + 12);
        return ev;
    }
};

// Peer address for signaling and UDP communication
struct PeerAddress {
    std::string host;
    uint16_t port = 0;

    bool valid() const { return !host.empty() && port != 0; }
    bool operator==(const PeerAddress& o) const {
        return host == o.host && port == o.port;
    }
    std::string toString() const { return host + ":" + std::to_string(port); }
};

} // namespace omnidesk
