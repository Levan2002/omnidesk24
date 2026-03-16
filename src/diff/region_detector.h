#pragma once

#include "core/types.h"
#include <memory>
#include <vector>

namespace omnidesk {

// Interface for dirty region detection.
// Compares two consecutive frames and returns a list of rectangles
// covering the regions that have changed.
class IDirtyRegionDetector {
public:
    virtual ~IDirtyRegionDetector() = default;

    // Detect changed regions between two frames.
    // Returns a vector of Rects covering all dirty (changed) areas.
    virtual std::vector<Rect> detect(const Frame& prev, const Frame& curr) = 0;

    // Set the pixel difference threshold for considering a block as changed.
    // Higher values reduce sensitivity to minor changes (e.g., compression artifacts).
    virtual void setThreshold(int threshold) = 0;
};

// Factory: creates the default SIMD-based dirty region detector
std::unique_ptr<IDirtyRegionDetector> createDirtyRegionDetector();

} // namespace omnidesk
