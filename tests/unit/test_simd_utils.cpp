#include <gtest/gtest.h>

#include "core/simd_utils.h"
#include "core/types.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace omnidesk {

// BT.601 expected Y/U/V for known colors.
// Y  =  0.299*R + 0.587*G + 0.114*B
// Cb = -0.169*R - 0.331*G + 0.500*B + 128
// Cr =  0.500*R - 0.419*G - 0.081*B + 128
// Allow a tolerance of +/-2 for rounding.

static void checkYUV(uint8_t actualY, uint8_t actualU, uint8_t actualV,
                     int expectY, int expectU, int expectV, int tol = 2) {
    EXPECT_NEAR(static_cast<int>(actualY), expectY, tol);
    EXPECT_NEAR(static_cast<int>(actualU), expectU, tol);
    EXPECT_NEAR(static_cast<int>(actualV), expectV, tol);
}

TEST(SimdUtils, BgraToI420_PureRed) {
    // BGRA pure red: B=0, G=0, R=255, A=255
    const int W = 2, H = 2;
    std::vector<uint8_t> bgra(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 0] = 0;     // B
        bgra[i * 4 + 1] = 0;     // G
        bgra[i * 4 + 2] = 255;   // R
        bgra[i * 4 + 3] = 255;   // A
    }

    std::vector<uint8_t> yPlane(W * H);
    std::vector<uint8_t> uPlane((W / 2) * (H / 2));
    std::vector<uint8_t> vPlane((W / 2) * (H / 2));

    bgraToI420(bgra.data(), W, H, W * 4,
               yPlane.data(), W,
               uPlane.data(), W / 2,
               vPlane.data(), W / 2);

    // BT.601: R=255 → Y≈76, U≈84, V≈255
    checkYUV(yPlane[0], uPlane[0], vPlane[0], 76, 84, 255);
}

TEST(SimdUtils, BgraToI420_PureGreen) {
    const int W = 2, H = 2;
    std::vector<uint8_t> bgra(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 0] = 0;     // B
        bgra[i * 4 + 1] = 255;   // G
        bgra[i * 4 + 2] = 0;     // R
        bgra[i * 4 + 3] = 255;   // A
    }

    std::vector<uint8_t> yPlane(W * H);
    std::vector<uint8_t> uPlane((W / 2) * (H / 2));
    std::vector<uint8_t> vPlane((W / 2) * (H / 2));

    bgraToI420(bgra.data(), W, H, W * 4,
               yPlane.data(), W,
               uPlane.data(), W / 2,
               vPlane.data(), W / 2);

    // BT.601: G=255 → Y≈150, U≈44, V≈21
    checkYUV(yPlane[0], uPlane[0], vPlane[0], 150, 44, 21);
}

TEST(SimdUtils, BgraToI420_PureBlue) {
    const int W = 2, H = 2;
    std::vector<uint8_t> bgra(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 0] = 255;   // B
        bgra[i * 4 + 1] = 0;     // G
        bgra[i * 4 + 2] = 0;     // R
        bgra[i * 4 + 3] = 255;   // A
    }

    std::vector<uint8_t> yPlane(W * H);
    std::vector<uint8_t> uPlane((W / 2) * (H / 2));
    std::vector<uint8_t> vPlane((W / 2) * (H / 2));

    bgraToI420(bgra.data(), W, H, W * 4,
               yPlane.data(), W,
               uPlane.data(), W / 2,
               vPlane.data(), W / 2);

    // BT.601: B=255 → Y≈29, U≈255, V≈107
    checkYUV(yPlane[0], uPlane[0], vPlane[0], 29, 255, 107);
}

TEST(SimdUtils, BgraToI420_White) {
    const int W = 2, H = 2;
    std::vector<uint8_t> bgra(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 0] = 255;
        bgra[i * 4 + 1] = 255;
        bgra[i * 4 + 2] = 255;
        bgra[i * 4 + 3] = 255;
    }

    std::vector<uint8_t> yPlane(W * H);
    std::vector<uint8_t> uPlane((W / 2) * (H / 2));
    std::vector<uint8_t> vPlane((W / 2) * (H / 2));

    bgraToI420(bgra.data(), W, H, W * 4,
               yPlane.data(), W,
               uPlane.data(), W / 2,
               vPlane.data(), W / 2);

    // BT.601: White → Y≈235(or 255), U≈128, V≈128
    checkYUV(yPlane[0], uPlane[0], vPlane[0], 255, 128, 128, 3);
}

TEST(SimdUtils, BgraToI420_Black) {
    const int W = 2, H = 2;
    std::vector<uint8_t> bgra(W * H * 4, 0);
    // Set alpha
    for (int i = 0; i < W * H; ++i) {
        bgra[i * 4 + 3] = 255;
    }

    std::vector<uint8_t> yPlane(W * H);
    std::vector<uint8_t> uPlane((W / 2) * (H / 2));
    std::vector<uint8_t> vPlane((W / 2) * (H / 2));

    bgraToI420(bgra.data(), W, H, W * 4,
               yPlane.data(), W,
               uPlane.data(), W / 2,
               vPlane.data(), W / 2);

    // BT.601: Black → Y≈0(or 16), U≈128, V≈128
    checkYUV(yPlane[0], uPlane[0], vPlane[0], 0, 128, 128, 16);
}

TEST(SimdUtils, ConvertFrameToI420) {
    Frame src;
    src.allocate(16, 16, PixelFormat::BGRA);
    // Fill with a solid color
    for (size_t i = 0; i < src.data.size(); i += 4) {
        src.data[i + 0] = 100; // B
        src.data[i + 1] = 150; // G
        src.data[i + 2] = 200; // R
        src.data[i + 3] = 255; // A
    }

    Frame dst;
    convertFrameToI420(src, dst);

    EXPECT_EQ(dst.width, 16);
    EXPECT_EQ(dst.height, 16);
    EXPECT_EQ(dst.format, PixelFormat::I420);
    // I420 size: W*H + W/2*H/2 + W/2*H/2 = W*H*3/2
    EXPECT_EQ(dst.data.size(), static_cast<size_t>(16 * 16 * 3 / 2));
}

TEST(SimdUtils, BlocksDiffer_Identical) {
    const int stride = 16 * 4;
    const int blockSize = 16;
    std::vector<uint8_t> block(stride * blockSize, 42);

    EXPECT_FALSE(blocksDiffer(block.data(), block.data(), stride, blockSize, 0));
}

TEST(SimdUtils, BlocksDiffer_Different) {
    const int stride = 16 * 4;
    const int blockSize = 16;
    std::vector<uint8_t> blockA(stride * blockSize, 0);
    std::vector<uint8_t> blockB(stride * blockSize, 255);

    EXPECT_TRUE(blocksDiffer(blockA.data(), blockB.data(), stride, blockSize, 0));
}

TEST(SimdUtils, BlockHash_SameData) {
    const int stride = 16 * 4;
    const int blockSize = 16;
    std::vector<uint8_t> block(stride * blockSize, 123);

    uint64_t h1 = blockHash(block.data(), stride, blockSize);
    uint64_t h2 = blockHash(block.data(), stride, blockSize);
    EXPECT_EQ(h1, h2);
}

TEST(SimdUtils, BlockHash_DifferentData) {
    const int stride = 16 * 4;
    const int blockSize = 16;
    std::vector<uint8_t> blockA(stride * blockSize, 0);
    std::vector<uint8_t> blockB(stride * blockSize, 255);

    uint64_t h1 = blockHash(blockA.data(), stride, blockSize);
    uint64_t h2 = blockHash(blockB.data(), stride, blockSize);
    EXPECT_NE(h1, h2);
}

TEST(SimdUtils, CpuSupportsAVX2_NoCrash) {
    // Just call it and ensure it doesn't crash. The result depends on the CPU.
    bool result = cpuSupportsAVX2();
    (void)result;
}

} // namespace omnidesk
