#include "transport/congestion.h"

#include <algorithm>
#include <cmath>

namespace omnidesk {

CongestionController::CongestionController() {
    lastIncreaseTime_ = Clock::now();
    lastDecreaseTime_ = Clock::now();
}

CongestionController::~CongestionController() = default;

uint64_t CongestionController::onPacketSent(uint32_t packetSize) {
    uint64_t seq = nextSeqNum_++;
    packetsSent_++;

    PacketRecord rec;
    rec.seqNum = seq;
    rec.size = packetSize;
    rec.sendTime = Clock::now();
    packetHistory_.push_back(rec);

    cleanupOldRecords();
    return seq;
}

void CongestionController::onAckReceived(uint64_t seqNum, float rttMs) {
    // Find the packet record
    for (auto& rec : packetHistory_) {
        if (rec.seqNum == seqNum && !rec.acked && !rec.lost) {
            rec.acked = true;
            break;
        }
    }

    // Update RTT estimate using EWMA
    if (!hasRttEstimate_) {
        smoothedRttMs_ = rttMs;
        rttVarianceMs_ = rttMs / 2.0f;
        hasRttEstimate_ = true;
    } else {
        float alpha = 0.125f;
        float beta = 0.25f;
        rttVarianceMs_ = (1.0f - beta) * rttVarianceMs_ +
                          beta * std::abs(smoothedRttMs_ - rttMs);
        smoothedRttMs_ = (1.0f - alpha) * smoothedRttMs_ + alpha * rttMs;
    }

    // Update delay gradient
    updateDelayGradient(rttMs);

    // Track in loss window
    lossWindow_.push_back(false);
    if (lossWindow_.size() > LOSS_WINDOW_SIZE) {
        lossWindow_.pop_front();
    }

    // Additive increase: grow bitrate by a fraction per ACK.
    // Spread the per-RTT increase across all ACKs received in that RTT window.
    // Estimate: one RTT's worth of ACKs ~ (bitrate / packetSize / 8) * RTT,
    // but for simplicity, apply a small per-ACK increment that accumulates.
    if (delayGradient_ <= 0.0f) {
        // Apply a per-ACK increase that is a fraction of the per-RTT increase.
        // We scale by 1/acksSinceIncrease to approximate per-RTT behavior.
        acksSinceLastIncrease_++;
        // Apply increase every N acks (approximate one RTT worth of packets)
        constexpr uint32_t ACKS_PER_INCREASE = 10;
        if (acksSinceLastIncrease_ >= ACKS_PER_INCREASE) {
            float increase = static_cast<float>(targetBitrateBps_) * ADDITIVE_INCREASE_FACTOR;
            targetBitrateBps_ = std::min(
                static_cast<uint32_t>(targetBitrateBps_ + increase),
                MAX_BITRATE_BPS);
            acksSinceLastIncrease_ = 0;
        }
    }

    updateLossRate();
}

void CongestionController::onLoss(uint64_t seqNum) {
    // Mark packet as lost
    for (auto& rec : packetHistory_) {
        if (rec.seqNum == seqNum && !rec.acked && !rec.lost) {
            rec.lost = true;
            packetsLost_++;
            break;
        }
    }

    // Track in loss window
    lossWindow_.push_back(true);
    if (lossWindow_.size() > LOSS_WINDOW_SIZE) {
        lossWindow_.pop_front();
    }

    // Multiplicative decrease: reduce bitrate by 15%
    // Apply decrease every N loss events to avoid over-reacting
    lossesSinceLastDecrease_++;
    constexpr uint32_t LOSSES_PER_DECREASE = 3;
    if (lossesSinceLastDecrease_ >= LOSSES_PER_DECREASE) {
        float decrease = static_cast<float>(targetBitrateBps_) *
                         MULTIPLICATIVE_DECREASE_FACTOR;
        targetBitrateBps_ = std::max(
            static_cast<uint32_t>(targetBitrateBps_ - decrease),
            MIN_BITRATE_BPS);
        lossesSinceLastDecrease_ = 0;
    }

    updateLossRate();
}

uint32_t CongestionController::getTargetBitrate() const {
    return targetBitrateBps_;
}

float CongestionController::getSmoothedRttMs() const {
    return smoothedRttMs_;
}

float CongestionController::getLossRate() const {
    return lossRate_;
}

float CongestionController::getDelayGradient() const {
    return delayGradient_;
}

void CongestionController::reset() {
    targetBitrateBps_ = INITIAL_BITRATE_BPS;
    nextSeqNum_ = 0;
    smoothedRttMs_ = 0.0f;
    rttVarianceMs_ = 0.0f;
    hasRttEstimate_ = false;
    delayGradient_ = 0.0f;
    prevRttMs_ = 0.0f;
    delayGradientHistory_.clear();
    packetsSent_ = 0;
    packetsLost_ = 0;
    lossRate_ = 0.0f;
    lossWindow_.clear();
    lastIncreaseTime_ = Clock::now();
    lastDecreaseTime_ = Clock::now();
    acksSinceLastIncrease_ = 0;
    lossesSinceLastDecrease_ = 0;
    packetHistory_.clear();
}

void CongestionController::updateLossRate() {
    if (lossWindow_.empty()) {
        lossRate_ = 0.0f;
        return;
    }
    uint64_t losses = 0;
    for (bool lost : lossWindow_) {
        if (lost) ++losses;
    }
    lossRate_ = static_cast<float>(losses) / static_cast<float>(lossWindow_.size());
}

void CongestionController::updateDelayGradient(float rttMs) {
    if (prevRttMs_ > 0.0f) {
        float gradient = rttMs - prevRttMs_;
        delayGradientHistory_.push_back(gradient);
        if (delayGradientHistory_.size() > GRADIENT_WINDOW_SIZE) {
            delayGradientHistory_.pop_front();
        }

        // Smoothed delay gradient (average of recent gradients)
        float sum = 0.0f;
        for (float g : delayGradientHistory_) {
            sum += g;
        }
        delayGradient_ = sum / static_cast<float>(delayGradientHistory_.size());

        // If delay is consistently increasing, reduce bitrate
        if (delayGradient_ > 1.0f) {
            float decrease = static_cast<float>(targetBitrateBps_) * 0.05f;
            targetBitrateBps_ = std::max(
                static_cast<uint32_t>(targetBitrateBps_ - decrease),
                MIN_BITRATE_BPS);
        }
    }
    prevRttMs_ = rttMs;
}

void CongestionController::cleanupOldRecords() {
    while (packetHistory_.size() > MAX_HISTORY_SIZE) {
        packetHistory_.pop_front();
    }
}

} // namespace omnidesk
