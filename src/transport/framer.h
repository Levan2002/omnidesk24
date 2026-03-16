#pragma once

#include "core/types.h"
#include "transport/protocol.h"
#include "transport/fec.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace omnidesk {

// Maximum payload per UDP fragment (MTU-safe: 1200 - VideoHeader::SIZE)
constexpr size_t MAX_FRAGMENT_PAYLOAD = MAX_UDP_PAYLOAD - VideoHeader::SIZE;

// ---- FramePacketizer ----
// Splits an EncodedPacket into MTU-sized UDP fragments with VideoHeader.
// Optionally generates FEC parity fragments.
class FramePacketizer {
public:
    FramePacketizer();
    ~FramePacketizer();

    // A single fragment ready for UDP transmission (VideoHeader + payload).
    struct Fragment {
        std::vector<uint8_t> data;  // Serialized VideoHeader + payload
        uint16_t fragId = 0;
        bool isParity = false;
    };

    // Split an encoded packet into fragments. Returns all fragments including
    // any FEC parity fragments.
    std::vector<Fragment> packetize(const EncodedPacket& packet, uint16_t fecGroupId);

    // Set the FEC protection ratio (0.0 = no FEC, 0.5 = 50% overhead).
    void setFecRatio(float ratio);

    // Get the FEC encoder for fine-grained control.
    FecEncoder& fecEncoder() { return fecEncoder_; }

private:
    FecEncoder fecEncoder_;
    uint16_t nextFecGroup_ = 0;
};

// ---- Selective ACK ----
// Bitmap-based selective ACK for reporting received fragments.
struct SelectiveAck {
    uint32_t frameId = 0;
    uint16_t fragCount = 0;
    std::vector<uint8_t> bitmap;  // 1 bit per fragment (received = 1)

    void setReceived(uint16_t fragId) {
        if (fragId >= fragCount) return;
        size_t byteIdx = fragId / 8;
        size_t bitIdx = fragId % 8;
        if (byteIdx < bitmap.size()) {
            bitmap[byteIdx] |= (1 << bitIdx);
        }
    }

    bool isReceived(uint16_t fragId) const {
        if (fragId >= fragCount) return false;
        size_t byteIdx = fragId / 8;
        size_t bitIdx = fragId % 8;
        if (byteIdx >= bitmap.size()) return false;
        return (bitmap[byteIdx] & (1 << bitIdx)) != 0;
    }

    // Serialize the SACK into a byte buffer.
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8 + bitmap.size());
        writeU32(buf.data(), frameId);
        writeU16(buf.data() + 4, fragCount);
        writeU16(buf.data() + 6, static_cast<uint16_t>(bitmap.size()));
        std::memcpy(buf.data() + 8, bitmap.data(), bitmap.size());
        return buf;
    }

    static SelectiveAck deserialize(const uint8_t* data, size_t len) {
        SelectiveAck sack;
        if (len < 8) return sack;
        sack.frameId = readU32(data);
        sack.fragCount = readU16(data + 4);
        uint16_t bitmapLen = readU16(data + 6);
        if (len >= 8u + bitmapLen) {
            sack.bitmap.assign(data + 8, data + 8 + bitmapLen);
        }
        return sack;
    }
};

// ---- FrameAssembler ----
// Reassembles UDP fragments into complete encoded frames.
// Tracks missing fragments and supports FEC recovery.
class FrameAssembler {
public:
    FrameAssembler();
    ~FrameAssembler();

    // Callback invoked when a complete frame is reassembled.
    using FrameCallback = std::function<void(const EncodedPacket& packet)>;

    // Set the callback for completed frames.
    void setFrameCallback(FrameCallback callback);

    // Add a received fragment (raw UDP data including VideoHeader).
    // Returns true if the fragment was accepted.
    bool addFragment(const uint8_t* data, size_t length);

    // Get a selective ACK for a specific frame.
    SelectiveAck getSelectiveAck(uint32_t frameId) const;

    // Get the list of missing fragment IDs for a frame.
    std::vector<uint16_t> getMissingFragments(uint32_t frameId) const;

    // Check if a frame is complete.
    bool isFrameComplete(uint32_t frameId) const;

    // Remove old incomplete frames (older than the given frame ID).
    void purgeOldFrames(uint32_t olderThan);

    // Get the last completed frame ID.
    uint32_t lastCompletedFrameId() const { return lastCompletedFrameId_; }

private:
    struct PartialFrame {
        uint32_t frameId = 0;
        uint16_t fragCount = 0;
        uint16_t fecGroup = 0;
        uint8_t flags = 0;
        uint8_t rectCount = 0;
        std::map<uint16_t, std::vector<uint8_t>> fragments;  // fragId -> payload
        bool complete = false;
    };

    bool tryAssemble(PartialFrame& frame);

    mutable std::mutex mutex_;
    std::map<uint32_t, PartialFrame> pendingFrames_;
    FrameCallback frameCallback_;
    uint32_t lastCompletedFrameId_ = 0;
};

} // namespace omnidesk
