#include "transport/hole_punch.h"
#include "transport/protocol.h"

#include <chrono>
#include <cstring>

namespace omnidesk {

// ---- PunchPacket ----

void HolePuncher::PunchPacket::serialize(uint8_t* buf) const {
    writeU32(buf, magic);
    // Write 64-bit token as two 32-bit values (network byte order)
    writeU32(buf + 4, static_cast<uint32_t>(sessionToken >> 32));
    writeU32(buf + 8, static_cast<uint32_t>(sessionToken & 0xFFFFFFFF));
}

HolePuncher::PunchPacket HolePuncher::PunchPacket::deserialize(const uint8_t* buf) {
    PunchPacket pkt;
    pkt.magic = readU32(buf);
    uint64_t hi = readU32(buf + 4);
    uint64_t lo = readU32(buf + 8);
    pkt.sessionToken = (hi << 32) | lo;
    return pkt;
}

// ---- PunchAckPacket ----

void HolePuncher::PunchAckPacket::serialize(uint8_t* buf) const {
    writeU32(buf, magic);
    writeU32(buf + 4, static_cast<uint32_t>(sessionToken >> 32));
    writeU32(buf + 8, static_cast<uint32_t>(sessionToken & 0xFFFFFFFF));
}

HolePuncher::PunchAckPacket HolePuncher::PunchAckPacket::deserialize(const uint8_t* buf) {
    PunchAckPacket pkt;
    pkt.magic = readU32(buf);
    uint64_t hi = readU32(buf + 4);
    uint64_t lo = readU32(buf + 8);
    pkt.sessionToken = (hi << 32) | lo;
    return pkt;
}

// ---- HolePuncher ----

HolePuncher::HolePuncher() = default;
HolePuncher::~HolePuncher() = default;

HolePuncher::Result HolePuncher::punch(const PeerAddress& peerPublic,
                                         const PeerAddress& peerLocal,
                                         uint16_t localPort) {
    Result result;
    lastAttemptCount_ = 0;

    auto channel = std::make_unique<UdpChannel>();
    if (!channel->bind(localPort)) {
        return result;
    }

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        lastAttemptCount_ = attempt + 1;

        PeerAddress connectedAddr;
        if (trySingleAttempt(*channel, peerPublic, peerLocal, connectedAddr)) {
            // Successfully punched through. Connect the channel to the peer.
            channel->connectTo(connectedAddr);
            result.success = true;
            result.connectedAddr = connectedAddr;
            result.channel = std::move(channel);
            return result;
        }
    }

    // All attempts failed
    channel->close();
    return result;
}

bool HolePuncher::trySingleAttempt(UdpChannel& channel,
                                    const PeerAddress& peerPublic,
                                    const PeerAddress& peerLocal,
                                    PeerAddress& connectedAddr) {
    using Clock = std::chrono::steady_clock;
    auto startTime = Clock::now();

    PunchPacket punchPkt;
    punchPkt.sessionToken = sessionToken_;

    uint8_t sendBuf[PunchPacket::SIZE];
    punchPkt.serialize(sendBuf);

    auto lastSendTime = Clock::now() - std::chrono::milliseconds(PUNCH_INTERVAL_MS);

    while (true) {
        auto now = Clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startTime).count();

        if (elapsedMs >= TIMEOUT_PER_ATTEMPT_MS) {
            return false;  // Timeout
        }

        // Send punch packets at regular intervals
        auto msSinceLastSend = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastSendTime).count();

        if (msSinceLastSend >= PUNCH_INTERVAL_MS) {
            // Send to both public and local addresses
            if (peerPublic.valid()) {
                channel.sendTo(peerPublic, sendBuf, PunchPacket::SIZE);
            }
            if (peerLocal.valid() && !(peerLocal == peerPublic)) {
                channel.sendTo(peerLocal, sendBuf, PunchPacket::SIZE);
            }
            lastSendTime = now;
        }

        // Check for incoming packets
        if (channel.pollRead(10)) {
            uint8_t recvBuf[64];
            PeerAddress sender;
            size_t n = channel.recvFrom(recvBuf, sizeof(recvBuf), sender);

            if (n >= PunchPacket::SIZE) {
                uint32_t magic = readU32(recvBuf);

                if (magic == PUNCH_MAGIC) {
                    PunchPacket received = PunchPacket::deserialize(recvBuf);

                    if (received.sessionToken == sessionToken_) {
                        // Got a punch from the peer - send ACK back
                        PunchAckPacket ack;
                        ack.sessionToken = sessionToken_;
                        uint8_t ackBuf[PunchAckPacket::SIZE];
                        ack.serialize(ackBuf);
                        channel.sendTo(sender, ackBuf, PunchAckPacket::SIZE);

                        connectedAddr = sender;
                        return true;
                    }
                } else if (magic == PUNCH_ACK_MAGIC) {
                    PunchAckPacket ack = PunchAckPacket::deserialize(recvBuf);

                    if (ack.sessionToken == sessionToken_) {
                        // Peer acknowledged our punch
                        connectedAddr = sender;
                        return true;
                    }
                }
            }
        }
    }
}

} // namespace omnidesk
