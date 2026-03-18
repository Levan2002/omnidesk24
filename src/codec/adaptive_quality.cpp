#include "codec/adaptive_quality.h"
#include "core/logger.h"
#include <algorithm>
#include <cmath>

namespace omnidesk {

// Static member definition
constexpr float AdaptiveQuality::kScaleFactors[Config::kMaxLevels];

AdaptiveQuality::AdaptiveQuality()
    : AdaptiveQuality(Config{}) {
}

AdaptiveQuality::AdaptiveQuality(const Config& config)
    : config_(config)
    , targetWidth_(config.nativeWidth)
    , targetHeight_(config.nativeHeight)
    , targetBitrate_(config.upscaleBitrateBps) {
    computeTargetResolution();
}

void AdaptiveQuality::reset() {
    currentLevel_ = 0;
    downscaleCounter_ = 0;
    upscaleCounter_ = 0;
    resolutionChanged_ = false;
    computeTargetResolution();
}

void AdaptiveQuality::setConfig(const Config& config) {
    config_ = config;
    currentLevel_ = 0;
    downscaleCounter_ = 0;
    upscaleCounter_ = 0;
    computeTargetResolution();
}

void AdaptiveQuality::update(float encodeTimeMs, float frameBudgetMs,
                              uint32_t currentBitrateBps) {
    resolutionChanged_ = false;

    // Determine if we should scale down or up based on encode time and bandwidth.
    float encodeLoad = (frameBudgetMs > 0.0f)
                       ? (encodeTimeMs / frameBudgetMs)
                       : 0.0f;

    bool wantDown = false;
    bool wantUp   = false;

    // CPU pressure: encode time exceeds budget threshold
    if (encodeLoad > config_.downscaleThreshold) {
        wantDown = true;
    }

    // Bandwidth pressure: bitrate dropped below downscale threshold
    if (currentBitrateBps > 0 && currentBitrateBps < config_.downscaleBitrateBps) {
        wantDown = true;
    }

    // Conditions for scaling up: low CPU load AND healthy bandwidth
    if (encodeLoad < config_.upscaleThreshold &&
        currentBitrateBps >= config_.upscaleBitrateBps) {
        wantUp = true;
    }

    // Hysteresis: require consecutive frames before changing level
    if (wantDown && currentLevel_ < Config::kMaxLevels - 1) {
        ++downscaleCounter_;
        upscaleCounter_ = 0;

        if (downscaleCounter_ >= config_.downscaleFrameCount) {
            ++currentLevel_;
            downscaleCounter_ = 0;
            resolutionChanged_ = true;
            LOG_INFO("AdaptiveQuality: downscale to level %d (load=%.0f%%, bitrate=%u bps)",
                     currentLevel_, encodeLoad * 100.0f, currentBitrateBps);
        }
    } else if (wantUp && currentLevel_ > 0) {
        ++upscaleCounter_;
        downscaleCounter_ = 0;

        if (upscaleCounter_ >= config_.upscaleFrameCount) {
            --currentLevel_;
            upscaleCounter_ = 0;
            resolutionChanged_ = true;
            LOG_INFO("AdaptiveQuality: upscale to level %d (load=%.0f%%, bitrate=%u bps)",
                     currentLevel_, encodeLoad * 100.0f, currentBitrateBps);
        }
    } else {
        // Neither direction: reset both counters (prevents stale pressure)
        downscaleCounter_ = 0;
        upscaleCounter_ = 0;
    }

    if (resolutionChanged_) {
        computeTargetResolution();
    }

    // Always update target bitrate (proportional to pixel count)
    float scaleFactor = kScaleFactors[currentLevel_];
    float pixelRatio = scaleFactor * scaleFactor; // area ratio
    targetBitrate_ = std::max(
        static_cast<uint32_t>(currentBitrateBps * pixelRatio),
        config_.minBitrateBps);
}

void AdaptiveQuality::computeTargetResolution() {
    float scale = kScaleFactors[currentLevel_];

    // Scale proportionally from native, ensure even dimensions
    int w = static_cast<int>(config_.nativeWidth * scale);
    int h = static_cast<int>(config_.nativeHeight * scale);
    w &= ~1; // round down to even
    h &= ~1;

    // Enforce minimum (720p floor)
    if (w < config_.minWidth) w = config_.minWidth;
    if (h < config_.minHeight) h = config_.minHeight;

    targetWidth_ = w;
    targetHeight_ = h;
}

} // namespace omnidesk
