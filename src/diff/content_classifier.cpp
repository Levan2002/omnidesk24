#include "diff/content_classifier.h"
#include "core/simd_utils.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace omnidesk {

ContentClassifier::ContentClassifier() = default;

ContentType ContentClassifier::classify(const Frame& frame, const Rect& region) {
    // Check temporal activity first: high activity means motion.
    float activity = computeTemporalActivity(region);
    if (activity > temporalActivityThreshold_) {
        return ContentType::MOTION;
    }

    // If very low activity, consider it static.
    if (activity < 0.01f) {
        return ContentType::STATIC;
    }

    // Analyze spatial characteristics.
    float edgeDensity = computeEdgeDensity(frame, region);
    int colorCount = countDistinctColors(frame, region);

    // High edge density + low color count → text/UI
    if (edgeDensity > edgeDensityThreshold_ && colorCount < colorCountThreshold_) {
        return ContentType::TEXT;
    }

    // High edge density alone may still be text (e.g., anti-aliased text).
    if (edgeDensity > edgeDensityThreshold_ * 2.0f) {
        return ContentType::TEXT;
    }

    // Default to motion for anything else that has some temporal change.
    return ContentType::MOTION;
}

void ContentClassifier::updateTemporalState(const Frame& prev, const Frame& curr) {
    if (prev.width != curr.width || prev.height != curr.height) {
        temporalActivity_.clear();
        return;
    }

    temporalFrameWidth_ = curr.width;
    temporalFrameHeight_ = curr.height;
    temporalBlocksX_ = (curr.width + kTemporalBlockSize - 1) / kTemporalBlockSize;
    temporalBlocksY_ = (curr.height + kTemporalBlockSize - 1) / kTemporalBlockSize;

    const size_t totalBlocks = static_cast<size_t>(temporalBlocksX_) * temporalBlocksY_;
    temporalActivity_.resize(totalBlocks, 0.0f);

    const int bytesPerPixel = (prev.format == PixelFormat::BGRA ||
                               prev.format == PixelFormat::RGBA) ? 4 : 1;

    for (int by = 0; by < temporalBlocksY_; ++by) {
        for (int bx = 0; bx < temporalBlocksX_; ++bx) {
            const int px = bx * kTemporalBlockSize;
            const int py = by * kTemporalBlockSize;
            const int bw = std::min(kTemporalBlockSize, static_cast<int>(prev.width) - px);
            const int bh = std::min(kTemporalBlockSize, static_cast<int>(prev.height) - py);

            int changedPixels = 0;
            const int totalPixels = bw * bh;

            for (int y = 0; y < bh; ++y) {
                const uint8_t* rowA = prev.data.data() + (py + y) * prev.stride + px * bytesPerPixel;
                const uint8_t* rowB = curr.data.data() + (py + y) * curr.stride + px * bytesPerPixel;
                for (int x = 0; x < bw * bytesPerPixel; x += bytesPerPixel) {
                    // Compare pixel values with small tolerance.
                    bool differs = false;
                    for (int c = 0; c < 3; ++c) { // Compare RGB, skip alpha
                        if (std::abs(static_cast<int>(rowA[x + c]) -
                                     static_cast<int>(rowB[x + c])) > 4) {
                            differs = true;
                            break;
                        }
                    }
                    if (differs) {
                        ++changedPixels;
                    }
                }
            }

            float blockActivity = (totalPixels > 0)
                ? static_cast<float>(changedPixels) / static_cast<float>(totalPixels)
                : 0.0f;

            temporalActivity_[static_cast<size_t>(by) * temporalBlocksX_ + bx] = blockActivity;
        }
    }
}

float ContentClassifier::computeEdgeDensity(const Frame& frame, const Rect& region) const {
    if (frame.data.empty() || region.empty()) {
        return 0.0f;
    }

    const int bytesPerPixel = (frame.format == PixelFormat::BGRA ||
                               frame.format == PixelFormat::RGBA) ? 4 : 1;

    // Clamp region to frame bounds.
    const int x0 = std::max(1, region.x);
    const int y0 = std::max(1, region.y);
    const int x1 = std::min(region.right(), frame.width - 1);
    const int y1 = std::min(region.bottom(), frame.height - 1);

    if (x1 <= x0 || y1 <= y0) {
        return 0.0f;
    }

    int edgePixels = 0;
    int totalPixels = 0;
    const int edgeThreshold = 30;

    // Simplified Sobel: compute gradient magnitude on the green channel
    // (or luminance approximation) for speed.
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            // Use green channel (index 1 for BGRA) as luminance proxy.
            const int cIdx = (frame.format == PixelFormat::BGRA) ? 1 : 1;

            auto getPixel = [&](int px, int py) -> int {
                return static_cast<int>(
                    frame.data[static_cast<size_t>(py) * frame.stride +
                               static_cast<size_t>(px) * bytesPerPixel + cIdx]);
            };

            // Sobel Gx = [-1 0 +1; -2 0 +2; -1 0 +1]
            int gx = -getPixel(x - 1, y - 1) + getPixel(x + 1, y - 1)
                     - 2 * getPixel(x - 1, y) + 2 * getPixel(x + 1, y)
                     - getPixel(x - 1, y + 1) + getPixel(x + 1, y + 1);

            // Sobel Gy = [-1 -2 -1; 0 0 0; +1 +2 +1]
            int gy = -getPixel(x - 1, y - 1) - 2 * getPixel(x, y - 1) - getPixel(x + 1, y - 1)
                     + getPixel(x - 1, y + 1) + 2 * getPixel(x, y + 1) + getPixel(x + 1, y + 1);

            int magnitude = std::abs(gx) + std::abs(gy);
            if (magnitude > edgeThreshold) {
                ++edgePixels;
            }
            ++totalPixels;
        }
    }

    return (totalPixels > 0)
        ? static_cast<float>(edgePixels) / static_cast<float>(totalPixels)
        : 0.0f;
}

int ContentClassifier::countDistinctColors(const Frame& frame, const Rect& region) const {
    if (frame.data.empty() || region.empty()) {
        return 0;
    }

    const int bytesPerPixel = (frame.format == PixelFormat::BGRA ||
                               frame.format == PixelFormat::RGBA) ? 4 : 1;

    const int x0 = std::max(0, region.x);
    const int y0 = std::max(0, region.y);
    const int x1 = std::min(region.right(), frame.width);
    const int y1 = std::min(region.bottom(), frame.height);

    // Quantize colors to reduce noise: drop lower 3 bits of each channel.
    std::unordered_set<uint32_t> colorSet;
    colorSet.reserve(256);

    // Sample at most every 2nd pixel for large regions.
    const int step = ((x1 - x0) * (y1 - y0) > 4096) ? 2 : 1;

    for (int y = y0; y < y1; y += step) {
        const uint8_t* row = frame.data.data() + static_cast<size_t>(y) * frame.stride;
        for (int x = x0; x < x1; x += step) {
            const uint8_t* px = row + static_cast<size_t>(x) * bytesPerPixel;
            // Quantize: shift right by 3 bits, pack into uint32_t.
            uint32_t quantized = (static_cast<uint32_t>(px[0] >> 3) << 16) |
                                 (static_cast<uint32_t>(px[1] >> 3) << 8) |
                                 (static_cast<uint32_t>(px[2] >> 3));
            colorSet.insert(quantized);

            // Early exit if clearly not text.
            if (static_cast<int>(colorSet.size()) > 512) {
                return static_cast<int>(colorSet.size());
            }
        }
    }

    return static_cast<int>(colorSet.size());
}

float ContentClassifier::computeTemporalActivity(const Rect& region) const {
    if (temporalActivity_.empty() || temporalBlocksX_ == 0 || temporalBlocksY_ == 0) {
        // No temporal data available; return zero activity so that classify()
        // falls through to spatial analysis or correctly reports STATIC.
        return 0.0f;
    }

    // Map the pixel region to block coordinates.
    const int bx0 = std::max(0, region.x / kTemporalBlockSize);
    const int by0 = std::max(0, region.y / kTemporalBlockSize);
    const int bx1 = std::min(temporalBlocksX_,
                             (region.right() + kTemporalBlockSize - 1) / kTemporalBlockSize);
    const int by1 = std::min(temporalBlocksY_,
                             (region.bottom() + kTemporalBlockSize - 1) / kTemporalBlockSize);

    if (bx1 <= bx0 || by1 <= by0) {
        return 0.0f;
    }

    float totalActivity = 0.0f;
    int blockCount = 0;

    for (int by = by0; by < by1; ++by) {
        for (int bx = bx0; bx < bx1; ++bx) {
            totalActivity += temporalActivity_[static_cast<size_t>(by) * temporalBlocksX_ + bx];
            ++blockCount;
        }
    }

    return (blockCount > 0) ? totalActivity / static_cast<float>(blockCount) : 0.0f;
}

} // namespace omnidesk
