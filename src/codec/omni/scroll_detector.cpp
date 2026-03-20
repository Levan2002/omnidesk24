#include "codec/omni/scroll_detector.h"

#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace omni {

ScrollDetector::ScrollDetector() = default;

void ScrollDetector::init(int width, int height, int tileSize) {
    width_ = width;
    height_ = height;
    tileSize_ = tileSize;
    refRowHashes_.assign(height, 0);
}

void ScrollDetector::reset() {
    std::fill(refRowHashes_.begin(), refRowHashes_.end(), 0ULL);
}

uint64_t ScrollDetector::hashRow(const uint8_t* row, int widthBytes) {
    // FNV-1a hash
    uint64_t h = 0x517CC1B727220A95ULL;
    for (int i = 0; i < widthBytes; ++i) {
        h ^= static_cast<uint64_t>(row[i]);
        h *= 0x100000001B3ULL;
    }
    return h;
}

void ScrollDetector::updateReference(const uint8_t* bgra, int stride) {
    for (int y = 0; y < height_; ++y) {
        refRowHashes_[y] = hashRow(bgra + y * stride, width_ * 4);
    }
}

ScrollResult ScrollDetector::detectTileScroll(const uint8_t* currentFrame,
                                                int currentStride,
                                                int tileX, int tileY,
                                                int tileW, int tileH) const {
    ScrollResult result;

    int py = tileY * tileSize_;
    int rowBytes = width_ * 4;

    // Hash the rows of the current tile region (full-width rows for simplicity)
    std::vector<uint64_t> currentHashes(tileH);
    for (int y = 0; y < tileH; ++y) {
        int row = py + y;
        if (row >= height_) break;
        currentHashes[y] = hashRow(currentFrame + row * currentStride, rowBytes);
    }

    // Search for the best vertical scroll offset
    // Try offsets in [-MAX_SCROLL_DISTANCE, MAX_SCROLL_DISTANCE]
    int bestOffset = 0;
    int bestMatches = 0;

    for (int dy = -MAX_SCROLL_DISTANCE; dy <= MAX_SCROLL_DISTANCE; ++dy) {
        if (dy == 0) continue;  // Skip zero offset (that's SKIP, not COPY)

        int matches = 0;
        for (int y = 0; y < tileH; ++y) {
            int refRow = py + y + dy;
            if (refRow < 0 || refRow >= height_) continue;
            if (currentHashes[y] == refRowHashes_[refRow]) {
                ++matches;
            }
        }

        if (matches > bestMatches) {
            bestMatches = matches;
            bestOffset = dy;
        }
    }

    // Require at least 75% of rows to match
    float confidence = static_cast<float>(bestMatches) / std::max(tileH, 1);
    if (confidence >= 0.75f && bestMatches >= 4) {
        result.detected = true;
        result.mvX = 0;
        result.mvY = static_cast<int16_t>(bestOffset);
        result.confidence = confidence;
    }

    (void)tileX;
    (void)tileW;
    return result;
}

} // namespace omni
} // namespace omnidesk
