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
        uint32_t initialBitrateBps  = 1500000;  // 1.5 Mbps (conservative start)
        uint32_t minBitrateBps      = 150000;   // 150 Kbps floor
        uint32_t maxBitrateBps      = 8000000;  // 8 Mbps ceiling

        // AIMD parameters
        uint32_t additiveIncreaseBps = 80000;   // +80 Kbps per good report (gentle ramp)
        float    multiplicativeDecrease = 0.65f; // x0.65 on congestion (fast drop)

        // Thresholds for congestion detection
        float packetLossThreshold   = 1.5f;     // % loss to trigger decrease
        float rttIncreaseThreshold  = 40.0f;    // ms RTT increase to trigger decrease
        float decodeTimeThreshold   = 20.0f;    // ms decode time spike (viewer overloaded)

        // Smoothing: minimum interval between bitrate changes
        float minAdjustIntervalMs   = 400.0f;
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
