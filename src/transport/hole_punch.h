#pragma once

#include "core/types.h"
#include "transport/udp_channel.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omnidesk {

// HolePuncher: attempts UDP hole punching to establish a direct P2P connection
// between two peers behind NATs. Sends simultaneous UDP packets to each other's
// public and local addresses.
class HolePuncher {
public:
    HolePuncher();
    ~HolePuncher();

    HolePuncher(const HolePuncher&) = delete;
    HolePuncher& operator=(const HolePuncher&) = delete;

    // Configuration
    static constexpr int TIMEOUT_PER_ATTEMPT_MS = 5000;  // 5 seconds per attempt
    static constexpr int MAX_ATTEMPTS = 3;
    static constexpr int PUNCH_INTERVAL_MS = 100;  // Send punch packet every 100ms

    // Magic bytes for punch packets (to distinguish from real data)
    static constexpr uint32_t PUNCH_MAGIC = 0x504E4348;  // "PNCH"
    static constexpr uint32_t PUNCH_ACK_MAGIC = 0x50414B;  // "PAK\0"

    // Result of hole punching attempt
    struct Result {
        bool success = false;
        PeerAddress connectedAddr;  // The address we successfully connected through
        std::unique_ptr<UdpChannel> channel;
    };

    // Attempt hole punching. Provide the peer's public and local addresses.
    // The localPort is our local UDP port to use (0 = auto-assign).
    // Returns a Result with a connected UdpChannel on success, or nullptr on failure.
    Result punch(const PeerAddress& peerPublic,
                 const PeerAddress& peerLocal,
                 uint16_t localPort = 0);

    // Set a unique session token so both peers can identify each other.
    void setSessionToken(uint64_t token) { sessionToken_ = token; }

    // Get the number of attempts made in the last punch() call.
    int lastAttemptCount() const { return lastAttemptCount_; }

private:
    // Punch packet format: PUNCH_MAGIC (4) + sessionToken (8)
    struct PunchPacket {
        uint32_t magic = PUNCH_MAGIC;
        uint64_t sessionToken = 0;

        void serialize(uint8_t* buf) const;
        static PunchPacket deserialize(const uint8_t* buf);
        static constexpr size_t SIZE = 12;
    };

    // Punch ACK packet
    struct PunchAckPacket {
        uint32_t magic = PUNCH_ACK_MAGIC;
        uint64_t sessionToken = 0;

        void serialize(uint8_t* buf) const;
        static PunchAckPacket deserialize(const uint8_t* buf);
        static constexpr size_t SIZE = 12;
    };

    bool trySingleAttempt(UdpChannel& channel,
                          const PeerAddress& peerPublic,
                          const PeerAddress& peerLocal,
                          PeerAddress& connectedAddr);

    uint64_t sessionToken_ = 0;
    int lastAttemptCount_ = 0;
};

} // namespace omnidesk
