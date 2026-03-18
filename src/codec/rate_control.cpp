#include "codec/rate_control.h"
#include <algorithm>
#include <cmath>

namespace omnidesk {

AdaptiveBitrateController::AdaptiveBitrateController()
    : AdaptiveBitrateController(Config{}) {
}

AdaptiveBitrateController::AdaptiveBitrateController(const Config& config)
    : config_(config)
    , currentBitrateBps_(config.initialBitrateBps) {
}

void AdaptiveBitrateController::reset() {
    currentBitrateBps_ = config_.initialBitrateBps;
    lastRttMs_ = 0.0f;
    lastAdjustTimeMs_ = 0.0f;
    elapsedMs_ = 0.0f;
}

void AdaptiveBitrateController::setConfig(const Config& config) {
    config_ = config;
    currentBitrateBps_ = std::clamp(currentBitrateBps_,
                                     config_.minBitrateBps,
                                     config_.maxBitrateBps);
}

uint32_t AdaptiveBitrateController::onQualityReport(const QualityReport& report) {
    // Use RTT as a proxy for elapsed time between reports.
    // In production, use a proper clock; here we estimate from report frequency.
    elapsedMs_ += std::max(report.rttMs, 16.0f); // Assume at least ~60fps report rate

    // Enforce minimum interval between adjustments.
    if (elapsedMs_ - lastAdjustTimeMs_ < config_.minAdjustIntervalMs) {
        return currentBitrateBps_;
    }
    lastAdjustTimeMs_ = elapsedMs_;

    // Detect congestion from multiple signals:
    //   1. High packet loss
    //   2. Significant RTT increase (queuing delay)
    //   3. Decode time spike (viewer CPU overloaded)
    //   4. High jitter (unstable network)
    bool congested = false;

    if (report.packetLossPercent > config_.packetLossThreshold) {
        congested = true;
    }

    if (lastRttMs_ > 0.0f) {
        float rttIncrease = report.rttMs - lastRttMs_;
        if (rttIncrease > config_.rttIncreaseThreshold) {
            congested = true;
        }
    }
    lastRttMs_ = report.rttMs;

    // Viewer decode time too high means we're sending more data than it can handle.
    if (report.decodeTimeMs > config_.decodeTimeThreshold) {
        congested = true;
    }

    // High jitter also indicates an unreliable link.
    if (report.jitterMs > config_.rttIncreaseThreshold) {
        congested = true;
    }

    if (congested) {
        // Multiplicative decrease -- react fast to congestion.
        auto newBitrate = static_cast<uint32_t>(
            static_cast<float>(currentBitrateBps_) * config_.multiplicativeDecrease);
        currentBitrateBps_ = std::max(newBitrate, config_.minBitrateBps);
    } else {
        // Additive increase -- probe capacity gently.
        uint32_t newBitrate = currentBitrateBps_ + config_.additiveIncreaseBps;
        currentBitrateBps_ = std::min(newBitrate, config_.maxBitrateBps);
    }

    return currentBitrateBps_;
}

} // namespace omnidesk
