#include <gtest/gtest.h>

#include "core/simd_ycocg.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace omnidesk {

// Helper: generate BGRA pixels with known colors
static std::vector<uint8_t> makeBGRA(int w, int h,
                                      uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
    std::vector<uint8_t> bgra(w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        bgra[i * 4 + 0] = b;
        bgra[i * 4 + 1] = g;
        bgra[i * 4 + 2] = r;
        bgra[i * 4 + 3] = a;
    }
    return bgra;
}

// Helper: full forward + inverse round-trip, verify exact match
static void verifyRoundTrip(const uint8_t* bgra, int w, int h) {
    int stride = w * 4;
    std::vector<int16_t> yPlane(w * h), coPlane(w * h), cgPlane(w * h);

    bgraToYCoCgR(bgra, w, h, stride,
                  yPlane.data(), coPlane.data(), cgPlane.data());

    std::vector<uint8_t> reconstructed(w * h * 4);
    yCoCgRToBgra(yPlane.data(), coPlane.data(), cgPlane.data(),
                  w, h, reconstructed.data(), stride);

    for (int i = 0; i < w * h; ++i) {
        int idx = i * 4;
        ASSERT_EQ(reconstructed[idx + 0], bgra[idx + 0])
            << "B mismatch at pixel " << i;
        ASSERT_EQ(reconstructed[idx + 1], bgra[idx + 1])
            << "G mismatch at pixel " << i;
        ASSERT_EQ(reconstructed[idx + 2], bgra[idx + 2])
            << "R mismatch at pixel " << i;
        ASSERT_EQ(reconstructed[idx + 3], 255)
            << "A should be 255 at pixel " << i;
    }
}

TEST(YCoCgR, RoundTrip_Black) {
    auto bgra = makeBGRA(16, 16, 0, 0, 0);
    verifyRoundTrip(bgra.data(), 16, 16);
}

TEST(YCoCgR, RoundTrip_White) {
    auto bgra = makeBGRA(16, 16, 255, 255, 255);
    verifyRoundTrip(bgra.data(), 16, 16);
}

TEST(YCoCgR, RoundTrip_PureRed) {
    auto bgra = makeBGRA(8, 8, 0, 0, 255);
    verifyRoundTrip(bgra.data(), 8, 8);
}

TEST(YCoCgR, RoundTrip_PureGreen) {
    auto bgra = makeBGRA(8, 8, 0, 255, 0);
    verifyRoundTrip(bgra.data(), 8, 8);
}

TEST(YCoCgR, RoundTrip_PureBlue) {
    auto bgra = makeBGRA(8, 8, 255, 0, 0);
    verifyRoundTrip(bgra.data(), 8, 8);
}

TEST(YCoCgR, RoundTrip_RandomPixels) {
    const int W = 64, H = 64;
    std::vector<uint8_t> bgra(W * H * 4);
    std::mt19937 rng(42);
    for (size_t i = 0; i < bgra.size(); ++i) {
        bgra[i] = rng() & 0xFF;
    }
    // Set alpha to 255 for clean comparison
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 3] = 255;
    }
    verifyRoundTrip(bgra.data(), W, H);
}

TEST(YCoCgR, RoundTrip_NonMultipleOf4Width) {
    // 7x5 -- not a multiple of 4 (tests scalar fallback path)
    const int W = 7, H = 5;
    std::vector<uint8_t> bgra(W * H * 4);
    std::mt19937 rng(99);
    for (size_t i = 0; i < bgra.size(); ++i) {
        bgra[i] = rng() & 0xFF;
    }
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 3] = 255;
    }
    verifyRoundTrip(bgra.data(), W, H);
}

TEST(YCoCgR, RoundTrip_SinglePixel) {
    auto bgra = makeBGRA(1, 1, 128, 64, 200);
    verifyRoundTrip(bgra.data(), 1, 1);
}

TEST(YCoCgR, RoundTrip_AllByteValues) {
    // Cover every possible byte value systematically
    const int W = 256, H = 3;
    std::vector<uint8_t> bgra(W * H * 4);
    for (int c = 0; c < 3; ++c) {
        for (int v = 0; v < 256; ++v) {
            int idx = (c * 256 + v) * 4;
            // Set B, G, R to v for one channel, 128 for others
            bgra[idx + 0] = (c == 0) ? v : 128; // B
            bgra[idx + 1] = (c == 1) ? v : 128; // G
            bgra[idx + 2] = (c == 2) ? v : 128; // R
            bgra[idx + 3] = 255;
        }
    }
    verifyRoundTrip(bgra.data(), W, H);
}

TEST(YCoCgR, ForwardTransform_KnownValues) {
    // YCoCg-R forward for R=200, G=100, B=50:
    // Co = R - B = 200 - 50 = 150
    // t  = B + (Co >> 1) = 50 + 75 = 125
    // Cg = G - t = 100 - 125 = -25
    // Y  = t + (Cg >> 1) = 125 + (-13) = 112  (note: -25>>1 = -13 in integer arithmetic)
    auto bgra = makeBGRA(1, 1, 50, 100, 200);
    int16_t y, co, cg;
    bgraToYCoCgR(bgra.data(), 1, 1, 4, &y, &co, &cg);

    EXPECT_EQ(co, 150);
    // t = 50 + 75 = 125
    EXPECT_EQ(cg, -25);
    // Y = 125 + (-25 >> 1) = 125 + (-13) = 112
    EXPECT_EQ(y, 112);
}

TEST(YCoCgR, RoundTrip_LargeTile) {
    // 64x64 tile (standard OmniCodec tile size)
    const int W = 64, H = 64;
    std::vector<uint8_t> bgra(W * H * 4);
    std::mt19937 rng(777);
    for (size_t i = 0; i < bgra.size(); ++i) {
        bgra[i] = rng() & 0xFF;
    }
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 3] = 255;
    }
    verifyRoundTrip(bgra.data(), W, H);
}

} // namespace omnidesk
