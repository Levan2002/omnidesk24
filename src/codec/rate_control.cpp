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

    // Detect congestion: high packet loss or significant RTT increase.
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

    if (congested) {
        // Multiplicative decrease.
        auto newBitrate = static_cast<uint32_t>(
            static_cast<float>(currentBitrateBps_) * config_.multiplicativeDecrease);
        currentBitrateBps_ = std::max(newBitrate, config_.minBitrateBps);
    } else {
        // Additive increase.
        uint32_t newBitrate = currentBitrateBps_ + config_.additiveIncreaseBps;
        currentBitrateBps_ = std::min(newBitrate, config_.maxBitrateBps);
    }

    return currentBitrateBps_;
}

} // namespace omnidesk
