#pragma once

#include "core/types.h"
#include "diff/region_detector.h"
#include <cstdint>
#include <vector>

namespace omnidesk {

// SIMD-accelerated dirty region detector.
// Divides the frame into 16x16 pixel blocks, uses blocksDiffer() from
// core/simd_utils.h to compare blocks between frames, then converts
// the resulting bitmask of changed blocks into a list of dirty rectangles.
class SIMDDiffDetector : public IDirtyRegionDetector {
public:
    SIMDDiffDetector();
    ~SIMDDiffDetector() override = default;

    std::vector<Rect> detect(const Frame& prev, const Frame& curr) override;
    void setThreshold(int threshold) override;

private:
    static constexpr int kBlockSize = 16;

    // Build a bitmask of changed blocks into mask_ member.
    void buildChangeMask(const Frame& prev, const Frame& curr,
                         int blocksX, int blocksY);

    // Convert mask_ into rects_ member.
    void maskToRects(int blocksX, int blocksY,
                     int frameWidth, int frameHeight);

    int threshold_ = 8;

    // Pre-allocated buffers reused every frame to avoid heap allocation on the hot path.
    std::vector<bool> mask_;
    std::vector<Rect> rects_;
};

} // namespace omnidesk
