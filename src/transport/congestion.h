#pragma once

#include "core/types.h"

#include <chrono>
#include <cstdint>
#include <deque>

namespace omnidesk {

// GCC-inspired congestion controller using AIMD (Additive Increase,
// Multiplicative Decrease). Tracks delay gradient and loss rate to
// determine the target bitrate for video encoding.
class CongestionController {
public:
    CongestionController();
    ~CongestionController();

    // Minimum and maximum bitrate bounds
    static constexpr uint32_t MIN_BITRATE_BPS = 100'000;      // 100 kbps
    static constexpr uint32_t MAX_BITRATE_BPS = 20'000'000;   // 20 Mbps
    static constexpr uint32_t INITIAL_BITRATE_BPS = 1'000'000; // 1 Mbps

    // AIMD parameters
    static constexpr float ADDITIVE_INCREASE_FACTOR = 0.05f;   // 5% per RTT
    static constexpr float MULTIPLICATIVE_DECREASE_FACTOR = 0.15f; // 15% on loss

    // Called when a packet is sent. Returns a sequence number for tracking.
    uint64_t onPacketSent(uint32_t packetSize);

    // Called when an ACK is received for a packet. Provides the measured RTT.
    void onAckReceived(uint64_t seqNum, float rttMs);

    // Called when a packet loss is detected.
    void onLoss(uint64_t seqNum);

    // Get the current target bitrate for the encoder.
    uint32_t getTargetBitrate() const;

    // Get the smoothed RTT estimate.
    float getSmoothedRttMs() const;

    // Get the current estimated loss rate (0.0 - 1.0).
    float getLossRate() const;

    // Get the current delay gradient (positive = increasing delay).
    float getDelayGradient() const;

    // Reset to initial state.
    void reset();

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // Per-packet tracking
    struct PacketRecord {
        uint64_t seqNum = 0;
        uint32_t size = 0;
        TimePoint sendTime;
        bool acked = false;
        bool lost = false;
    };

    void updateBitrate();
    void updateLossRate();
    void updateDelayGradient(float rttMs);
    void cleanupOldRecords();

    // Current state
    uint32_t targetBitrateBps_ = INITIAL_BITRATE_BPS;
    uint64_t nextSeqNum_ = 0;

    // RTT tracking
    float smoothedRttMs_ = 0.0f;
    float rttVarianceMs_ = 0.0f;
    bool hasRttEstimate_ = false;

    // Delay gradient tracking (for GCC-style overuse detection)
    float delayGradient_ = 0.0f;
    float prevRttMs_ = 0.0f;
    std::deque<float> delayGradientHistory_;
    static constexpr size_t GRADIENT_WINDOW_SIZE = 20;

    // Loss tracking
    uint64_t packetsSent_ = 0;
    uint64_t packetsLost_ = 0;
    float lossRate_ = 0.0f;
    std::deque<bool> lossWindow_;  // true = lost, false = received
    static constexpr size_t LOSS_WINDOW_SIZE = 100;

    // AIMD state
    TimePoint lastIncreaseTime_;
    TimePoint lastDecreaseTime_;
    static constexpr int MIN_DECREASE_INTERVAL_MS = 200;  // Min time between decreases
    uint32_t acksSinceLastIncrease_ = 0;
    uint32_t lossesSinceLastDecrease_ = 0;

    // Packet history
    std::deque<PacketRecord> packetHistory_;
    static constexpr size_t MAX_HISTORY_SIZE = 1000;
};

} // namespace omnidesk
