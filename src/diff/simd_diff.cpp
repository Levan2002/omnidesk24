#include "diff/simd_diff.h"
#include "core/simd_utils.h"
#include <algorithm>

namespace omnidesk {

SIMDDiffDetector::SIMDDiffDetector() = default;

void SIMDDiffDetector::setThreshold(int threshold) {
    threshold_ = std::max(0, threshold);
}

std::vector<Rect> SIMDDiffDetector::detect(const Frame& prev, const Frame& curr) {
    if (prev.width != curr.width || prev.height != curr.height) {
        return {};
    }
    if (prev.data.empty() || curr.data.empty()) {
        return {};
    }

    const int blocksX = (curr.width + kBlockSize - 1) / kBlockSize;
    const int blocksY = (curr.height + kBlockSize - 1) / kBlockSize;

    buildChangeMask(prev, curr, blocksX, blocksY);
    maskToRects(blocksX, blocksY, curr.width, curr.height);
    return std::move(rects_);
}

void SIMDDiffDetector::buildChangeMask(const Frame& prev,
                                        const Frame& curr,
                                        int blocksX,
                                        int blocksY) {
    const size_t totalBlocks = static_cast<size_t>(blocksX) * blocksY;
    mask_.assign(totalBlocks, false);

    const int bytesPerPixel = (prev.format == PixelFormat::BGRA ||
                               prev.format == PixelFormat::RGBA) ? 4 : 1;
    const int stride = prev.stride;

    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const int px = bx * kBlockSize;
            const int py = by * kBlockSize;

            const int bw = std::min(kBlockSize, static_cast<int>(prev.width) - px);
            const int bh = std::min(kBlockSize, static_cast<int>(prev.height) - py);

            const uint8_t* ptrA = prev.data.data() + py * stride + px * bytesPerPixel;
            const uint8_t* ptrB = curr.data.data() + py * stride + px * bytesPerPixel;

            const int effectiveBlockSize = std::min(bw, bh);
            if (effectiveBlockSize > 0 &&
                blocksDiffer(ptrA, ptrB, stride, effectiveBlockSize, threshold_)) {
                mask_[static_cast<size_t>(by) * blocksX + bx] = true;
            }
        }
    }
}

void SIMDDiffDetector::maskToRects(int blocksX, int blocksY,
                                    int frameWidth, int frameHeight) {
    rects_.clear();

    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (!mask_[static_cast<size_t>(by) * blocksX + bx]) {
                continue;
            }

            Rect r;
            r.x = bx * kBlockSize;
            r.y = by * kBlockSize;
            r.width = std::min(kBlockSize, frameWidth - r.x);
            r.height = std::min(kBlockSize, frameHeight - r.y);
            rects_.push_back(r);
        }
    }
}

std::unique_ptr<IDirtyRegionDetector> createDirtyRegionDetector() {
    return std::make_unique<SIMDDiffDetector>();
}

} // namespace omnidesk
