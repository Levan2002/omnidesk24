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

    // Build a bitmask of changed blocks. Each bit in the returned vector
    // corresponds to one block (row-major order). true = changed.
    std::vector<bool> buildChangeMask(const Frame& prev, const Frame& curr,
                                      int blocksX, int blocksY) const;

    // Convert the bitmask of changed blocks into a list of rectangles.
    // Each set bit becomes a rect covering its 16x16 block, clipped to frame bounds.
    std::vector<Rect> maskToRects(const std::vector<bool>& mask,
                                  int blocksX, int blocksY,
                                  int frameWidth, int frameHeight) const;

    int threshold_ = 8;
};

} // namespace omnidesk
