#pragma once

#include "core/types.h"
#include <cstdint>

namespace omnidesk {

// Adaptive bitrate controller using AIMD (Additive Increase Multiplicative Decrease).
//
// Takes periodic QualityReport feedback from the viewer and outputs a target
// bitrate for the encoder. The algorithm:
//   - On good network (low loss, low RTT): additively increase bitrate
//   - On congestion (high loss or RTT spike): multiplicatively decrease bitrate
//   - Clamps output between configured min and max bitrate
class AdaptiveBitrateController {
public:
    struct Config {
        uint32_t initialBitrateBps  = 2000000;  // 2 Mbps
        uint32_t minBitrateBps      = 200000;   // 200 Kbps
        uint32_t maxBitrateBps      = 8000000;  // 8 Mbps

        // AIMD parameters
        uint32_t additiveIncreaseBps = 100000;  // +100 Kbps per good report
        float    multiplicativeDecrease = 0.7f;  // x0.7 on congestion

        // Thresholds for congestion detection
        float packetLossThreshold   = 2.0f;     // % loss to trigger decrease
        float rttIncreaseThreshold  = 50.0f;    // ms RTT increase to trigger decrease

        // Smoothing: minimum interval between bitrate changes
        float minAdjustIntervalMs   = 500.0f;
    };

    AdaptiveBitrateController();
    explicit AdaptiveBitrateController(const Config& config);
    ~AdaptiveBitrateController() = default;

    // Process a quality report and return the new target bitrate.
    uint32_t onQualityReport(const QualityReport& report);

    // Get the current target bitrate.
    uint32_t currentBitrate() const { return currentBitrateBps_; }

    // Reset to initial state.
    void reset();

    // Update configuration.
    void setConfig(const Config& config);

private:
    Config config_;
    uint32_t currentBitrateBps_;
    float lastRttMs_ = 0.0f;
    float lastAdjustTimeMs_ = 0.0f;
    float elapsedMs_ = 0.0f;
};

} // namespace omnidesk
