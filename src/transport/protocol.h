#pragma once

// During the WebRTC migration, the canonical definitions for:
//   - Serialization helpers, PeerAddress, InputEvent  ->  core/types.h
//   - PROTOCOL_MAGIC, PROTOCOL_VERSION, ControlHeader, MessageType
//                                                     ->  signaling/wire_format.h
//
// This header re-exports those and adds transport-specific definitions
// (VideoHeader, video flags, MAX_UDP_PAYLOAD).

#include "signaling/wire_format.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>

namespace omnidesk {

// Maximum UDP payload size (MTU-safe)
constexpr size_t MAX_UDP_PAYLOAD = 1200;

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

} // namespace omnidesk
