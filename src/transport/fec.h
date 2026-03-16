#pragma once

#include "core/types.h"
#include "transport/protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace omnidesk {

// A single FEC-managed packet (either data or parity)
struct FecPacket {
    std::vector<uint8_t> data;
    uint16_t index = 0;       // Packet index within the FEC group
    uint16_t groupId = 0;     // FEC group identifier
    bool isParity = false;    // true if this is a parity (repair) packet
};

// FecEncoder: takes K data packets, produces N-K parity packets using XOR.
// Parity packets use interleaved grouping so each parity covers a subset of
// data packets, enabling recovery of one missing packet per parity group.
class FecEncoder {
public:
    FecEncoder();
    ~FecEncoder();

    // Set the protection ratio (parity packets / data packets).
    // E.g., 0.2 means 20% overhead: 5 data -> 1 parity.
    void setProtectionRatio(float ratio);

    // Adapt the protection ratio based on observed packet loss rate.
    void adaptToLossRate(float lossRate);

    // Encode: given K data packets, produce parity packets.
    // groupId is used to tag the FEC group for the decoder.
    std::vector<FecPacket> encode(const std::vector<FecPacket>& dataPackets,
                                   uint16_t groupId);

    float protectionRatio() const { return ratio_; }

    void reset();

private:
    // XOR all packets together (must be same length; caller pads)
    std::vector<uint8_t> xorPackets(const std::vector<const uint8_t*>& packets,
                                     size_t packetLen) const;

    float ratio_ = 0.1f;      // Default: 10% parity overhead
    float minRatio_ = 0.05f;  // Minimum: 5%
    float maxRatio_ = 0.5f;   // Maximum: 50%
};

// FecDecoder: reassembles data packets, recovers missing ones from parity.
class FecDecoder {
public:
    FecDecoder();
    ~FecDecoder();

    // Set expected packet counts for this FEC group.
    void setExpectedCount(uint16_t dataCount, uint16_t parityCount);

    // Add a received packet (data or parity).
    void addPacket(const FecPacket& packet);

    // Try to recover missing data packets. Returns true if all data is available.
    bool tryRecover();

    // Get all data packets (including recovered ones), in index order.
    std::vector<FecPacket> getDataPackets() const;

    // Check if a specific data packet has been received or recovered.
    bool hasPacket(uint16_t index) const;

    // Number of data packets received so far.
    uint16_t receivedDataCount() const;

    // Number of parity packets received so far.
    uint16_t receivedParityCount() const;

    // True if all data packets are available (received or recovered).
    bool isComplete() const;

    void reset();

private:
    struct FecGroup {
        uint16_t expectedDataCount = 0;
        uint16_t expectedParityCount = 0;
        std::vector<FecPacket> dataPackets;
        std::vector<FecPacket> parityPackets;
        std::vector<bool> dataReceived;
        std::vector<bool> parityReceived;
        bool recovered = false;
    };

    FecGroup group_;
    size_t maxPacketLen_ = 0;
};

} // namespace omnidesk
