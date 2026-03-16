#pragma once

#include "core/types.h"
#include <cstdint>
#include <vector>

namespace omnidesk {

// Merges a potentially large list of small dirty rectangles into a compact
// set of rectangles suitable for region-based encoding.
//
// Uses a cost model that minimizes total encoded pixel area while keeping
// the rectangle count at or below maxRects.
class RectMerger {
public:
    // Merge overlapping and adjacent rectangles.
    // Returns at most maxRects rectangles covering all input rects.
    static std::vector<Rect> merge(const std::vector<Rect>& rects, int maxRects = 8);

private:
    // Compute encoding cost: sum of all rect areas.
    static int64_t totalArea(const std::vector<Rect>& rects);

    // Find the pair of rectangles whose union wastes the least extra area.
    // Returns indices (i, j) with i < j.
    static std::pair<int, int> findBestMergePair(const std::vector<Rect>& rects);

    // Merge overlapping rectangles in-place (exact overlap / containment pass).
    static std::vector<Rect> mergeOverlapping(const std::vector<Rect>& rects);
};

} // namespace omnidesk
