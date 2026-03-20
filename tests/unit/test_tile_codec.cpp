#include <gtest/gtest.h>

#include "codec/omni/tile_encoder.h"
#include "codec/omni/tile_decoder.h"
#include "core/simd_ycocg.h"
#include "core/simd_predict.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace omnidesk {
namespace omni {

// Helper: create BGRA tile with solid color
static std::vector<uint8_t> makeSolidTile(int w, int h, uint8_t b, uint8_t g, uint8_t r) {
    std::vector<uint8_t> tile(w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        tile[i * 4 + 0] = b;
        tile[i * 4 + 1] = g;
        tile[i * 4 + 2] = r;
        tile[i * 4 + 3] = 255;
    }
    return tile;
}

// Helper: create BGRA tile with random pixels
static std::vector<uint8_t> makeRandomTile(int w, int h, uint32_t seed) {
    std::vector<uint8_t> tile(w * h * 4);
    std::mt19937 rng(seed);
    for (int i = 0; i < w * h; ++i) {
        tile[i * 4 + 0] = rng() & 0xFF;
        tile[i * 4 + 1] = rng() & 0xFF;
        tile[i * 4 + 2] = rng() & 0xFF;
        tile[i * 4 + 3] = 255;
    }
    return tile;
}

// Helper: create BGRA tile with horizontal gradient (good for H prediction)
static std::vector<uint8_t> makeHGradientTile(int w, int h) {
    std::vector<uint8_t> tile(w * h * 4);
    for (int y = 0; y < h; ++y) {
        uint8_t val = static_cast<uint8_t>((y * 255) / std::max(h - 1, 1));
        for (int x = 0; x < w; ++x) {
            tile[(y * w + x) * 4 + 0] = val;
            tile[(y * w + x) * 4 + 1] = val;
            tile[(y * w + x) * 4 + 2] = val;
            tile[(y * w + x) * 4 + 3] = 255;
        }
    }
    return tile;
}

// Helper: create BGRA tile with vertical gradient (good for V prediction)
static std::vector<uint8_t> makeVGradientTile(int w, int h) {
    std::vector<uint8_t> tile(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t val = static_cast<uint8_t>((x * 255) / std::max(w - 1, 1));
            tile[(y * w + x) * 4 + 0] = val;
            tile[(y * w + x) * 4 + 1] = val;
            tile[(y * w + x) * 4 + 2] = val;
            tile[(y * w + x) * 4 + 3] = 255;
        }
    }
    return tile;
}

// Round-trip: encode tile -> decode tile -> compare BGRA pixels
static void verifyTileRoundTrip(const uint8_t* bgra, int w, int h) {
    int stride = w * 4;

    TileEncoder enc;
    TileDecoder dec;
    enc.init(std::max(w, h));
    dec.init(std::max(w, h));

    // Encode (no neighbor context -- edge tile)
    BitstreamWriter bsw(w * h * 8);
    enc.encodeLossless(bgra, stride, w, h,
                       nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr,
                       0, 0, 0, bsw);

    // Decode
    auto& encoded = bsw.data();
    BitstreamReader bsr(encoded.data(), encoded.size());

    std::vector<uint8_t> decoded(w * h * 4, 0);
    ASSERT_TRUE(dec.decodeLossless(bsr, decoded.data(), stride, w, h,
                                    nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr,
                                    0, 0, 0));

    // Verify pixel-perfect round-trip
    for (int i = 0; i < w * h; ++i) {
        int idx = i * 4;
        ASSERT_EQ(decoded[idx + 0], bgra[idx + 0])
            << "B mismatch at pixel " << i;
        ASSERT_EQ(decoded[idx + 1], bgra[idx + 1])
            << "G mismatch at pixel " << i;
        ASSERT_EQ(decoded[idx + 2], bgra[idx + 2])
            << "R mismatch at pixel " << i;
    }
}

TEST(TileCodec, RoundTrip_SolidBlack) {
    auto tile = makeSolidTile(64, 64, 0, 0, 0);
    verifyTileRoundTrip(tile.data(), 64, 64);
}

TEST(TileCodec, RoundTrip_SolidWhite) {
    auto tile = makeSolidTile(64, 64, 255, 255, 255);
    verifyTileRoundTrip(tile.data(), 64, 64);
}

TEST(TileCodec, RoundTrip_SolidColor) {
    auto tile = makeSolidTile(64, 64, 100, 150, 200);
    verifyTileRoundTrip(tile.data(), 64, 64);
}

TEST(TileCodec, RoundTrip_RandomPixels) {
    auto tile = makeRandomTile(64, 64, 42);
    verifyTileRoundTrip(tile.data(), 64, 64);
}

TEST(TileCodec, RoundTrip_HGradient) {
    auto tile = makeHGradientTile(64, 64);
    verifyTileRoundTrip(tile.data(), 64, 64);
}

TEST(TileCodec, RoundTrip_VGradient) {
    auto tile = makeVGradientTile(64, 64);
    verifyTileRoundTrip(tile.data(), 64, 64);
}

TEST(TileCodec, RoundTrip_SmallTile) {
    auto tile = makeRandomTile(8, 8, 99);
    verifyTileRoundTrip(tile.data(), 8, 8);
}

TEST(TileCodec, RoundTrip_NonSquareTile) {
    // Edge tiles at frame boundary can be non-square
    auto tile = makeRandomTile(48, 32, 777);
    verifyTileRoundTrip(tile.data(), 48, 32);
}

TEST(TileCodec, RoundTrip_32x32Tile) {
    auto tile = makeRandomTile(32, 32, 55);
    verifyTileRoundTrip(tile.data(), 32, 32);
}

// Test prediction mode helpers
TEST(Predict, DC_SolidBorder) {
    // All border pixels are 100; DC should predict 100 everywhere.
    int16_t top[8] = {100, 100, 100, 100, 100, 100, 100, 100};
    int16_t left[8] = {100, 100, 100, 100, 100, 100, 100, 100};
    int16_t out[64];

    predictDC(top, left, 8, 8, out);
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(out[i], 100);
    }
}

TEST(Predict, V_CopiesTopRow) {
    int16_t top[4] = {10, 20, 30, 40};
    int16_t out[16];

    predictV(top, 4, 4, out);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            EXPECT_EQ(out[y * 4 + x], top[x]);
        }
    }
}

TEST(Predict, H_CopiesLeftColumn) {
    int16_t left[4] = {10, 20, 30, 40};
    int16_t out[16];

    predictH(left, 4, 4, out);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            EXPECT_EQ(out[y * 4 + x], left[y]);
        }
    }
}

TEST(Predict, LeftPixel_RoundTrip) {
    int16_t src[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    int16_t residual[16];
    int16_t reconstructed[16];

    predictLeftPixel(src, residual, 4, 4);
    inversePredictLeftPixel(residual, reconstructed, 4, 4);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(reconstructed[i], src[i]) << "Mismatch at " << i;
    }
}

TEST(Predict, SAD_ZeroDiff) {
    int16_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(computeSAD(a, a, 8), 0u);
}

TEST(Predict, SAD_KnownDiff) {
    int16_t a[4] = {10, 20, 30, 40};
    int16_t b[4] = {0, 0, 0, 0};
    EXPECT_EQ(computeSAD(a, b, 4), 100u);
}

} // namespace omni
} // namespace omnidesk
