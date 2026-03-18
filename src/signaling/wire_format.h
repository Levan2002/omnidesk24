#pragma once

#include "core/types.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>

namespace omnidesk {

// Wire format magic number: "OMND"
constexpr uint32_t PROTOCOL_MAGIC = 0x4F4D4E44;

// Current protocol version
constexpr uint16_t PROTOCOL_VERSION = 1;

// Message types for control channel (ControlHeader.type field).
// The signaling layer only uses a subset (HELLO, CONNECT_*, HEARTBEAT,
// BYE, RELAY_DATA) but the full enum is defined here so that transport
// and session code can reference all message types from a single
// canonical definition during the WebRTC migration.
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

} // namespace omnidesk
