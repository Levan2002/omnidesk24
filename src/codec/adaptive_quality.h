#pragma once

#include <cstdint>

namespace omnidesk {

// Adaptive quality controller that scales resolution and bitrate based on
// CPU load (encode time vs frame budget) and network bandwidth.
//
// Resolution ladder (proportional to native):
//   Level 0 (best):  native  (e.g. 1920x1080)
//   Level 1:         ~83%    (e.g. 1600x900)
//   Level 2 (floor): ~67%    (e.g. 1280x720)
//
// Transitions are smoothed: the controller requires several consecutive
// samples before changing level, preventing rapid oscillation.
//
// Text regions are always encoded at maximum quality (QP delta -15)
// regardless of the current resolution level.
class AdaptiveQuality {
public:
    struct Config {
        int nativeWidth  = 1920;
        int nativeHeight = 1080;

        // Minimum resolution floor (720p)
        int minWidth  = 1280;
        int minHeight = 720;

        // Encode time thresholds (fraction of frame budget)
        float downscaleThreshold = 0.80f;  // >80% of budget → scale down
        float upscaleThreshold   = 0.50f;  // <50% of budget → scale up

        // Bandwidth thresholds (bits per second)
        uint32_t downscaleBitrateBps = 500000;   // <500 kbps → scale down
        uint32_t upscaleBitrateBps   = 2000000;  // >2 Mbps → allow scale up
        uint32_t minBitrateBps       = 300000;    // Absolute minimum at 720p

        // FPS range
        float maxFps = 60.0f;
        float minFps = 15.0f;
        float idleFps = 30.0f;

        // Hysteresis: number of consecutive frames before changing level
        int downscaleFrameCount = 10;  // ~333ms at 30fps
        int upscaleFrameCount   = 30;  // ~1s at 30fps (slower upscale)

        // Number of resolution steps (including native)
        static constexpr int kMaxLevels = 3;
    };

    AdaptiveQuality();
    explicit AdaptiveQuality(const Config& config);
    ~AdaptiveQuality() = default;

    // Call once per encoded frame with current performance metrics.
    void update(float encodeTimeMs, float frameBudgetMs,
                uint32_t currentBitrateBps, float motionRatio);

    // Current target resolution (always even, never below minimum)
    int targetWidth() const { return targetWidth_; }
    int targetHeight() const { return targetHeight_; }

    // Current resolution level (0 = native, 1 = medium, 2 = low/720p)
    int currentLevel() const { return currentLevel_; }

    // Adaptive target FPS — use this to drive capture/encode rate
    float targetFps() const { return targetFps_; }

    // Target bitrate adjusted for current resolution level
    uint32_t targetBitrate() const { return targetBitrate_; }

    // Whether the resolution changed since last update()
    bool resolutionChanged() const { return resolutionChanged_; }

    // Reset to native resolution
    void reset();

    // Reconfigure (e.g. when native resolution changes)
    void setConfig(const Config& config);

private:
    void computeTargetResolution();

    Config config_;

    int currentLevel_ = 0;  // 0=native, 1=medium, 2=low
    int targetWidth_  = 1920;
    int targetHeight_ = 1080;
    float targetFps_  = 30.0f;
    uint32_t targetBitrate_ = 2000000;
    bool resolutionChanged_ = false;

    // Hysteresis counters
    int downscaleCounter_ = 0;
    int upscaleCounter_   = 0;

    // Scale factors for each level (fraction of native, in 0.01 increments)
    // Level 0: 100%, Level 1: 83%, Level 2: 67%
    static constexpr float kScaleFactors[Config::kMaxLevels] = {1.00f, 0.833f, 0.667f};
};

} // namespace omnidesk
