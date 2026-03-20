#include <gtest/gtest.h>

#include "core/simd_dct.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace omnidesk {

// Helper: compute PSNR between two int16_t buffers
static double computePSNR(const int16_t* a, const int16_t* b, int count) {
    double mse = 0;
    for (int i = 0; i < count; ++i) {
        double diff = a[i] - b[i];
        mse += diff * diff;
    }
    mse /= count;
    if (mse == 0) return 999.0;  // Perfect
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

TEST(DCT, Forward_Inverse_4x4_Zeros) {
    int16_t src[4 * 4] = {};
    int16_t coeffs[4 * 4] = {};
    int16_t recon[4 * 4] = {};

    dctForward(src, 4, coeffs, 4, 4);
    dctInverse(coeffs, 4, recon, 4, 4);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(recon[i], 0);
    }
}

TEST(DCT, Forward_Inverse_4x4_DC) {
    // Constant block: all 100
    int16_t src[16];
    int16_t coeffs[16];
    int16_t recon[16];
    std::fill(src, src + 16, 100);

    dctForward(src, 4, coeffs, 4, 4);
    dctInverse(coeffs, 4, recon, 4, 4);

    // Should be close to original (small rounding error from fixed-point)
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(recon[i], 100, 3) << "Mismatch at " << i;
    }
}

TEST(DCT, Forward_Inverse_8x8_Random) {
    std::mt19937 rng(42);
    int16_t src[64], coeffs[64], recon[64];
    for (auto& v : src) v = (rng() % 256) - 128;

    dctForward(src, 8, coeffs, 8, 8);
    dctInverse(coeffs, 8, recon, 8, 8);

    // Allow some fixed-point rounding error
    double psnr = computePSNR(src, recon, 64);
    EXPECT_GT(psnr, 30.0) << "DCT round-trip PSNR too low: " << psnr;
}

TEST(DCT, Forward_Inverse_16x16_Random) {
    std::mt19937 rng(99);
    int16_t src[256], coeffs[256], recon[256];
    for (auto& v : src) v = (rng() % 256) - 128;

    dctForward(src, 16, coeffs, 16, 16);
    dctInverse(coeffs, 16, recon, 16, 16);

    double psnr = computePSNR(src, recon, 256);
    EXPECT_GT(psnr, 25.0) << "DCT 16x16 round-trip PSNR too low: " << psnr;
}

TEST(DCT, Quantize_Dequantize_LowQP) {
    // Low QP (high quality): quantize+dequantize should preserve most info
    int16_t src[64], coeffs[64], qcoeffs[64], recon[64];
    std::mt19937 rng(7);
    for (auto& v : src) v = (rng() % 200) - 100;

    dctForward(src, 8, coeffs, 8, 8);
    std::memcpy(qcoeffs, coeffs, sizeof(qcoeffs));
    quantize(qcoeffs, 8, 8, 10);  // Low QP
    dequantize(qcoeffs, 8, 8, 10);
    dctInverse(qcoeffs, 8, recon, 8, 8);

    double psnr = computePSNR(src, recon, 64);
    EXPECT_GT(psnr, 20.0) << "Low QP PSNR too low: " << psnr;
}

TEST(DCT, Quantize_Dequantize_HighQP) {
    // High QP (low quality): more loss but still reasonable
    int16_t src[64], coeffs[64], qcoeffs[64], recon[64];
    std::mt19937 rng(7);
    for (auto& v : src) v = (rng() % 200) - 100;

    dctForward(src, 8, coeffs, 8, 8);
    std::memcpy(qcoeffs, coeffs, sizeof(qcoeffs));
    quantize(qcoeffs, 8, 8, 40);  // High QP
    dequantize(qcoeffs, 8, 8, 40);
    dctInverse(qcoeffs, 8, recon, 8, 8);

    double psnr = computePSNR(src, recon, 64);
    EXPECT_GT(psnr, 10.0) << "High QP PSNR too low: " << psnr;
}

TEST(DCT, Quantize_ZerosHighFreq) {
    // High QP should zero out high-frequency coefficients
    int16_t coeffs[64];
    std::mt19937 rng(3);
    for (auto& v : coeffs) v = rng() % 50;

    quantize(coeffs, 8, 8, 50);

    // Check that high-frequency coefficients are mostly zero
    int nonZeroHighFreq = 0;
    for (int y = 4; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            if (coeffs[y * 8 + x] != 0) ++nonZeroHighFreq;
        }
    }
    // Most high-freq should be zeroed
    EXPECT_LE(nonZeroHighFreq, 8);
}

TEST(DCT, EnergyCompaction) {
    // DCT should concentrate energy in low-frequency coefficients
    int16_t src[64];
    int16_t coeffs[64];

    // Smooth signal (low-frequency content)
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            src[y * 8 + x] = static_cast<int16_t>(128 + 50 * std::sin(y * 0.5));
        }
    }

    dctForward(src, 8, coeffs, 8, 8);

    // DC coefficient should have most energy
    int32_t dcEnergy = static_cast<int32_t>(coeffs[0]) * coeffs[0];
    int64_t totalEnergy = 0;
    for (int i = 0; i < 64; ++i) {
        totalEnergy += static_cast<int64_t>(coeffs[i]) * coeffs[i];
    }

    // DC should be at least 50% of total energy for this smooth signal
    EXPECT_GT(dcEnergy, totalEnergy / 2);
}

} // namespace omnidesk
