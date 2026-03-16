#include "diff/simd_diff.h"
#include "core/simd_utils.h"
#include <algorithm>

namespace omnidesk {

SIMDDiffDetector::SIMDDiffDetector() = default;

void SIMDDiffDetector::setThreshold(int threshold) {
    threshold_ = std::max(0, threshold);
}

std::vector<Rect> SIMDDiffDetector::detect(const Frame& prev, const Frame& curr) {
    // Frames must have matching dimensions and be BGRA.
    if (prev.width != curr.width || prev.height != curr.height) {
        return {};
    }
    if (prev.data.empty() || curr.data.empty()) {
        return {};
    }

    const int blocksX = (curr.width + kBlockSize - 1) / kBlockSize;
    const int blocksY = (curr.height + kBlockSize - 1) / kBlockSize;

    std::vector<bool> mask = buildChangeMask(prev, curr, blocksX, blocksY);
    return maskToRects(mask, blocksX, blocksY, curr.width, curr.height);
}

std::vector<bool> SIMDDiffDetector::buildChangeMask(const Frame& prev,
                                                     const Frame& curr,
                                                     int blocksX,
                                                     int blocksY) const {
    std::vector<bool> mask(static_cast<size_t>(blocksX) * blocksY, false);

    const int bytesPerPixel = (prev.format == PixelFormat::BGRA ||
                               prev.format == PixelFormat::RGBA) ? 4 : 1;
    const int stride = prev.stride;

    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const int px = bx * kBlockSize;
            const int py = by * kBlockSize;

            // Compute effective block size (may be smaller at frame edges)
            const int bw = std::min(kBlockSize, static_cast<int>(prev.width) - px);
            const int bh = std::min(kBlockSize, static_cast<int>(prev.height) - py);

            const uint8_t* ptrA = prev.data.data() + py * stride + px * bytesPerPixel;
            const uint8_t* ptrB = curr.data.data() + py * stride + px * bytesPerPixel;

            // Use SIMD-accelerated block comparison from core/simd_utils.h.
            // For edge blocks that are smaller than 16x16 we still call
            // blocksDiffer which handles arbitrary blockSize.
            const int effectiveBlockSize = std::min(bw, bh);
            if (effectiveBlockSize > 0 &&
                blocksDiffer(ptrA, ptrB, stride, effectiveBlockSize, threshold_)) {
                mask[static_cast<size_t>(by) * blocksX + bx] = true;
            }
        }
    }

    return mask;
}

std::vector<Rect> SIMDDiffDetector::maskToRects(const std::vector<bool>& mask,
                                                 int blocksX, int blocksY,
                                                 int frameWidth,
                                                 int frameHeight) const {
    std::vector<Rect> rects;
    rects.reserve(32);

    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (!mask[static_cast<size_t>(by) * blocksX + bx]) {
                continue;
            }

            Rect r;
            r.x = bx * kBlockSize;
            r.y = by * kBlockSize;
            r.width = std::min(kBlockSize, frameWidth - r.x);
            r.height = std::min(kBlockSize, frameHeight - r.y);
            rects.push_back(r);
        }
    }

    return rects;
}

// Factory function for creating a dirty region detector
std::unique_ptr<IDirtyRegionDetector> createDirtyRegionDetector() {
    return std::make_unique<SIMDDiffDetector>();
}

} // namespace omnidesk
