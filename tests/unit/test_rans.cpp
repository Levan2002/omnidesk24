#include <gtest/gtest.h>

#include "codec/omni/rans_codec.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace omnidesk {
namespace omni {

// Helper: build histogram from raw symbols
static std::vector<uint32_t> buildHistogram(const uint8_t* symbols, size_t count) {
    std::vector<uint32_t> counts(256, 0);
    for (size_t i = 0; i < count; ++i) {
        counts[symbols[i]]++;
    }
    return counts;
}

// Helper: round-trip encode/decode and verify
static void verifyRoundTrip(const uint8_t* symbols, size_t count) {
    auto counts = buildHistogram(symbols, count);

    // Build frequency table from histogram
    auto freqTable = buildFrequencyTable(counts.data(), 256);

    // Build decode table
    auto decodeTable = buildDecodeTable(freqTable, 256);

    // Encode
    RANSEncoder encoder;
    std::vector<uint8_t> encoded;
    encoder.encode(symbols, count, freqTable.data(), 256, encoded);
    ASSERT_FALSE(encoded.empty());

    // Decode
    RANSDecoder decoder;
    std::vector<uint8_t> decoded(count);
    ASSERT_TRUE(decoder.decode(encoded.data(), encoded.size(),
                                decodeTable.data(), count, decoded.data()));

    // Verify exact match
    for (size_t i = 0; i < count; ++i) {
        ASSERT_EQ(decoded[i], symbols[i])
            << "Mismatch at index " << i << " (count=" << count << ")";
    }
}

TEST(RANSCodec, RoundTrip_SingleSymbol) {
    std::vector<uint8_t> data(256, 42);
    verifyRoundTrip(data.data(), data.size());
}

TEST(RANSCodec, RoundTrip_TwoSymbols) {
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = (i % 2 == 0) ? 0 : 255;
    }
    verifyRoundTrip(data.data(), data.size());
}

TEST(RANSCodec, RoundTrip_AllSymbols) {
    // All 256 possible byte values
    std::vector<uint8_t> data(256);
    std::iota(data.begin(), data.end(), 0);
    verifyRoundTrip(data.data(), data.size());
}

TEST(RANSCodec, RoundTrip_SkewedDistribution) {
    // 90% zeros, 10% ones — typical screen content residual
    std::vector<uint8_t> data(4096);
    std::mt19937 rng(12345);
    for (auto& v : data) {
        v = (rng() % 10 == 0) ? 1 : 0;
    }
    verifyRoundTrip(data.data(), data.size());
}

TEST(RANSCodec, RoundTrip_RandomData) {
    std::vector<uint8_t> data(8192);
    std::mt19937 rng(42);
    for (auto& v : data) {
        v = rng() & 0xFF;
    }
    verifyRoundTrip(data.data(), data.size());
}

TEST(RANSCodec, RoundTrip_LargeBuffer) {
    // 64x64 tile * 3 channels = 12288 symbols (typical tile payload)
    std::vector<uint8_t> data(64 * 64 * 3);
    std::mt19937 rng(99);
    // Screen-like distribution: mostly small residuals
    for (auto& v : data) {
        int r = rng() % 100;
        if (r < 70) v = 0;
        else if (r < 85) v = 1;
        else if (r < 92) v = 2;
        else if (r < 96) v = 3;
        else v = rng() & 0xFF;
    }
    verifyRoundTrip(data.data(), data.size());
}

TEST(RANSCodec, RoundTrip_SmallBuffer) {
    uint8_t data[] = {10, 20, 30};
    verifyRoundTrip(data, 3);
}

TEST(RANSCodec, FrequencyTable_NormalizesToScale) {
    std::vector<uint8_t> data(1000);
    std::mt19937 rng(7);
    for (auto& v : data) {
        v = rng() & 0xFF;
    }

    auto counts = buildHistogram(data.data(), data.size());
    auto table = buildFrequencyTable(counts.data(), 256);

    uint32_t totalFreq = 0;
    for (int i = 0; i < 256; ++i) {
        totalFreq += table[i].freq;
    }
    EXPECT_EQ(totalFreq, RANS_SCALE);
}

TEST(RANSCodec, FrequencyTable_MinFreqForPresent) {
    // All 256 values present: each must have freq >= 1
    std::vector<uint8_t> data(256);
    std::iota(data.begin(), data.end(), 0);

    auto counts = buildHistogram(data.data(), data.size());
    auto table = buildFrequencyTable(counts.data(), 256);

    for (int i = 0; i < 256; ++i) {
        EXPECT_GE(table[i].freq, 1u) << "Symbol " << i << " has freq 0";
    }
}

TEST(RANSCodec, DecodeTable_CoverAllPositions) {
    std::vector<uint8_t> data(500);
    std::mt19937 rng(13);
    for (auto& v : data) {
        v = rng() % 10; // Only 10 distinct symbols
    }

    auto counts = buildHistogram(data.data(), data.size());
    auto freqTable = buildFrequencyTable(counts.data(), 256);
    auto decTable = buildDecodeTable(freqTable, 256);

    // Every position in decode table should be populated
    EXPECT_EQ(decTable.size(), static_cast<size_t>(RANS_SCALE));
    for (size_t i = 0; i < decTable.size(); ++i) {
        EXPECT_GT(decTable[i].freq, 0u) << "Decode table entry " << i << " has freq 0";
    }
}

TEST(RANSCodec, Compression_SkewedBetterThanUniform) {
    // Skewed data should compress much better than uniform
    std::vector<uint8_t> skewed(4096, 0);
    for (int i = 0; i < 40; ++i) skewed[i] = 1;

    std::vector<uint8_t> uniform(4096);
    std::mt19937 rng(1);
    for (auto& v : uniform) v = rng() & 0xFF;

    auto skewedCounts = buildHistogram(skewed.data(), skewed.size());
    auto uniformCounts = buildHistogram(uniform.data(), uniform.size());
    auto skewedFreq = buildFrequencyTable(skewedCounts.data(), 256);
    auto uniformFreq = buildFrequencyTable(uniformCounts.data(), 256);

    RANSEncoder enc;
    std::vector<uint8_t> skewedEnc, uniformEnc;
    enc.encode(skewed.data(), skewed.size(), skewedFreq.data(), 256, skewedEnc);
    enc.encode(uniform.data(), uniform.size(), uniformFreq.data(), 256, uniformEnc);

    // Skewed should be significantly smaller
    EXPECT_LT(skewedEnc.size(), uniformEnc.size() / 2);
}

} // namespace omni
} // namespace omnidesk
