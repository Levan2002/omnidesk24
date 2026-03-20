#include <gtest/gtest.h>

#include "codec/omni/scroll_detector.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace omnidesk {
namespace omni {

// Helper: create a frame with distinct row patterns
static std::vector<uint8_t> makeTestFrame(int w, int h, int stride, uint32_t seed) {
    std::vector<uint8_t> frame(stride * h, 0);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y) {
        // Each row has a unique pattern based on row index + seed
        std::mt19937 rowRng(seed * 1000 + y);
        for (int x = 0; x < w * 4; ++x) {
            frame[y * stride + x] = rowRng() & 0xFF;
        }
    }
    return frame;
}

// Helper: create a scrolled version of a frame
static std::vector<uint8_t> scrollFrame(const std::vector<uint8_t>& src,
                                         int w, int h, int stride, int scrollY) {
    std::vector<uint8_t> dst(stride * h, 0);
    for (int y = 0; y < h; ++y) {
        int srcY = y + scrollY;
        if (srcY >= 0 && srcY < h) {
            std::memcpy(dst.data() + y * stride,
                        src.data() + srcY * stride,
                        w * 4);
        }
    }
    return dst;
}

TEST(ScrollDetect, NoScroll) {
    int w = 128, h = 128, stride = w * 4, tileSize = 64;
    auto frame = makeTestFrame(w, h, stride, 42);

    ScrollDetector sd;
    sd.init(w, h, tileSize);
    sd.updateReference(frame.data(), stride);

    // Same frame -> no scroll detected (offset 0 is excluded)
    auto result = sd.detectTileScroll(frame.data(), stride, 0, 0, 64, 64);
    EXPECT_FALSE(result.detected);
}

TEST(ScrollDetect, ScrollDown) {
    int w = 128, h = 128, stride = w * 4, tileSize = 64;
    auto ref = makeTestFrame(w, h, stride, 42);
    auto scrolled = scrollFrame(ref, w, h, stride, 16);  // scroll down by 16

    ScrollDetector sd;
    sd.init(w, h, tileSize);
    sd.updateReference(ref.data(), stride);

    // Check middle tile (to avoid edge effects)
    auto result = sd.detectTileScroll(scrolled.data(), stride, 0, 0, 64, 64);
    if (result.detected) {
        // The motion vector should indicate scroll down
        EXPECT_EQ(result.mvY, 16);
        EXPECT_EQ(result.mvX, 0);
        EXPECT_GE(result.confidence, 0.75f);
    }
}

TEST(ScrollDetect, ScrollUp) {
    int w = 128, h = 128, stride = w * 4, tileSize = 64;
    auto ref = makeTestFrame(w, h, stride, 42);
    auto scrolled = scrollFrame(ref, w, h, stride, -16);  // scroll up by 16

    ScrollDetector sd;
    sd.init(w, h, tileSize);
    sd.updateReference(ref.data(), stride);

    auto result = sd.detectTileScroll(scrolled.data(), stride, 0, 1, 64, 64);
    if (result.detected) {
        EXPECT_EQ(result.mvY, -16);
        EXPECT_EQ(result.mvX, 0);
    }
}

TEST(ScrollDetect, LargeScroll) {
    int w = 128, h = 256, stride = w * 4, tileSize = 64;
    auto ref = makeTestFrame(w, h, stride, 99);
    auto scrolled = scrollFrame(ref, w, h, stride, 100);

    ScrollDetector sd;
    sd.init(w, h, tileSize);
    sd.updateReference(ref.data(), stride);

    auto result = sd.detectTileScroll(scrolled.data(), stride, 0, 1, 64, 64);
    if (result.detected) {
        EXPECT_EQ(result.mvY, 100);
    }
}

TEST(ScrollDetect, DifferentContent_NoDetection) {
    int w = 128, h = 128, stride = w * 4, tileSize = 64;
    auto ref = makeTestFrame(w, h, stride, 42);
    auto different = makeTestFrame(w, h, stride, 999);

    ScrollDetector sd;
    sd.init(w, h, tileSize);
    sd.updateReference(ref.data(), stride);

    auto result = sd.detectTileScroll(different.data(), stride, 0, 0, 64, 64);
    EXPECT_FALSE(result.detected);
}

TEST(ScrollDetect, Reset) {
    int w = 128, h = 128, stride = w * 4, tileSize = 64;
    auto frame = makeTestFrame(w, h, stride, 42);

    ScrollDetector sd;
    sd.init(w, h, tileSize);
    sd.updateReference(frame.data(), stride);
    sd.reset();

    // After reset, should not detect anything
    auto scrolled = scrollFrame(frame, w, h, stride, 16);
    auto result = sd.detectTileScroll(scrolled.data(), stride, 0, 0, 64, 64);
    EXPECT_FALSE(result.detected);
}

} // namespace omni
} // namespace omnidesk
