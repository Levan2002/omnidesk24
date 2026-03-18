#pragma once

#include "core/types.h"
#include <cstdint>
#include <vector>

namespace omnidesk {

// Classifies a region of a frame as TEXT, MOTION, or STATIC based on:
//   - Edge density (simplified Sobel filter)
//   - Color count heuristic (low color count → text)
//   - Temporal activity (comparison with previous classification state)
class ContentClassifier {
public:
    ContentClassifier();
    ~ContentClassifier() = default;

    // Classify the content of a rectangular region within the frame.
    ContentType classify(const Frame& frame, const Rect& region);

    // Feed temporal information: call once per frame before classify() calls
    // to enable temporal activity tracking.
    void updateTemporalState(const Frame& prev, const Frame& curr);

    // Tuning parameters
    void setEdgeDensityThreshold(float threshold) { edgeDensityThreshold_ = threshold; }
    void setColorCountThreshold(int count) { colorCountThreshold_ = count; }
    void setTemporalActivityThreshold(float threshold) { temporalActivityThreshold_ = threshold; }

private:
    // Compute edge density of a region using a simplified 3x3 Sobel operator.
    // Returns a value in [0, 1] where higher means more edges.
    float computeEdgeDensity(const Frame& frame, const Rect& region) const;

    // Count approximate number of distinct colors in a region.
    // Uses a hash-set on quantized color values for speed.
    int countDistinctColors(const Frame& frame, const Rect& region) const;

    // Compute temporal activity as the fraction of pixels changed since
    // the previous frame in the given region.
    float computeTemporalActivity(const Rect& region) const;

    // Thresholds (tuned for desktop/IDE/terminal text detection)
    float edgeDensityThreshold_ = 0.12f;   // Above this -> likely text (lower catches anti-aliased text)
    int colorCountThreshold_ = 80;          // Below this -> likely text (higher catches syntax-highlighted code)
    float temporalActivityThreshold_ = 0.25f; // Above this -> motion (lower = more sensitive to motion)

    // Temporal state: per-block change flags from most recent updateTemporalState()
    std::vector<float> temporalActivity_;   // Per-block activity [0..1]
    int temporalBlocksX_ = 0;
    int temporalBlocksY_ = 0;
    int temporalFrameWidth_ = 0;
    int temporalFrameHeight_ = 0;

    static constexpr int kTemporalBlockSize = 16;
};

} // namespace omnidesk
