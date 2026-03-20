#include "core/simd_ycocg.h"

#include <immintrin.h>
#include <algorithm>

namespace omnidesk {
namespace avx2 {

void bgraToYCoCgR(const uint8_t* bgra, int width, int height, int bgraStride,
                   int16_t* yPlane, int16_t* coPlane, int16_t* cgPlane) {
    const __m256i mask_ff = _mm256_set1_epi32(0xFF);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        int16_t* yRow = yPlane + y * width;
        int16_t* coRow = coPlane + y * width;
        int16_t* cgRow = cgPlane + y * width;

        int x = 0;
        // Process 8 pixels at a time (32 bytes = 1 AVX2 register of BGRA)
        for (; x + 7 < width; x += 8) {
            __m256i pix = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + x * 4));

            // Extract B, G, R as 32-bit
            __m256i b32 = _mm256_and_si256(pix, mask_ff);
            __m256i g32 = _mm256_and_si256(_mm256_srli_epi32(pix, 8), mask_ff);
            __m256i r32 = _mm256_and_si256(_mm256_srli_epi32(pix, 16), mask_ff);

            // Co = R - B
            __m256i co32 = _mm256_sub_epi32(r32, b32);

            // t = B + (Co >> 1)
            __m256i t32 = _mm256_add_epi32(b32, _mm256_srai_epi32(co32, 1));

            // Cg = G - t
            __m256i cg32 = _mm256_sub_epi32(g32, t32);

            // Y = t + (Cg >> 1)
            __m256i y32 = _mm256_add_epi32(t32, _mm256_srai_epi32(cg32, 1));

            // Pack 32->16 (signed). Note: _mm256_packs_epi32 operates per-lane.
            // We pack with zero to get values in the lower half of each 128-bit lane.
            __m256i y16 = _mm256_packs_epi32(y32, _mm256_setzero_si256());
            __m256i co16 = _mm256_packs_epi32(co32, _mm256_setzero_si256());
            __m256i cg16 = _mm256_packs_epi32(cg32, _mm256_setzero_si256());

            // After packs: lane0=[v0,v1,v2,v3,0,0,0,0], lane1=[v4,v5,v6,v7,0,0,0,0]
            // Use permute to gather all values into lower 128 bits
            alignas(32) int16_t yBuf[16], coBuf[16], cgBuf[16];
            _mm256_store_si256(reinterpret_cast<__m256i*>(yBuf), y16);
            _mm256_store_si256(reinterpret_cast<__m256i*>(coBuf), co16);
            _mm256_store_si256(reinterpret_cast<__m256i*>(cgBuf), cg16);

            // Copy lane0[0..3] and lane1[0..3]
            yRow[x+0] = yBuf[0]; yRow[x+1] = yBuf[1]; yRow[x+2] = yBuf[2]; yRow[x+3] = yBuf[3];
            yRow[x+4] = yBuf[8]; yRow[x+5] = yBuf[9]; yRow[x+6] = yBuf[10]; yRow[x+7] = yBuf[11];

            coRow[x+0] = coBuf[0]; coRow[x+1] = coBuf[1]; coRow[x+2] = coBuf[2]; coRow[x+3] = coBuf[3];
            coRow[x+4] = coBuf[8]; coRow[x+5] = coBuf[9]; coRow[x+6] = coBuf[10]; coRow[x+7] = coBuf[11];

            cgRow[x+0] = cgBuf[0]; cgRow[x+1] = cgBuf[1]; cgRow[x+2] = cgBuf[2]; cgRow[x+3] = cgBuf[3];
            cgRow[x+4] = cgBuf[8]; cgRow[x+5] = cgBuf[9]; cgRow[x+6] = cgBuf[10]; cgRow[x+7] = cgBuf[11];
        }

        // Scalar remainder
        for (; x < width; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];

            int co = r - b;
            int t  = b + (co >> 1);
            int cg = g - t;
            int yVal = t + (cg >> 1);

            yRow[x]  = static_cast<int16_t>(yVal);
            coRow[x] = static_cast<int16_t>(co);
            cgRow[x] = static_cast<int16_t>(cg);
        }
    }
}

void yCoCgRToBgra(const int16_t* yPlane, const int16_t* coPlane, const int16_t* cgPlane,
                   int width, int height,
                   uint8_t* bgra, int bgraStride) {
    const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xFF000000));
    const __m256i maxVal = _mm256_set1_epi32(255);
    const __m256i zeroVec = _mm256_setzero_si256();

    for (int y = 0; y < height; ++y) {
        const int16_t* yRow = yPlane + y * width;
        const int16_t* coRow = coPlane + y * width;
        const int16_t* cgRow = cgPlane + y * width;
        uint8_t* row = bgra + y * bgraStride;

        int x = 0;
        for (; x + 7 < width; x += 8) {
            // Load 8 int16 values, sign-extend to 32-bit
            __m128i y16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yRow + x));
            __m128i co16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(coRow + x));
            __m128i cg16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cgRow + x));

            __m256i y32 = _mm256_cvtepi16_epi32(y16);
            __m256i co32 = _mm256_cvtepi16_epi32(co16);
            __m256i cg32 = _mm256_cvtepi16_epi32(cg16);

            // t = Y - (Cg >> 1)
            __m256i t32 = _mm256_sub_epi32(y32, _mm256_srai_epi32(cg32, 1));

            // G = Cg + t
            __m256i g32 = _mm256_add_epi32(cg32, t32);

            // B = t - (Co >> 1)
            __m256i b32 = _mm256_sub_epi32(t32, _mm256_srai_epi32(co32, 1));

            // R = Co + B
            __m256i r32 = _mm256_add_epi32(co32, b32);

            // Clamp to [0, 255]
            b32 = _mm256_max_epi32(_mm256_min_epi32(b32, maxVal), zeroVec);
            g32 = _mm256_max_epi32(_mm256_min_epi32(g32, maxVal), zeroVec);
            r32 = _mm256_max_epi32(_mm256_min_epi32(r32, maxVal), zeroVec);

            // Pack to BGRA
            __m256i bgra32 = _mm256_or_si256(
                _mm256_or_si256(b32, _mm256_slli_epi32(g32, 8)),
                _mm256_or_si256(_mm256_slli_epi32(r32, 16), alpha)
            );

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(row + x * 4), bgra32);
        }

        // Scalar remainder
        for (; x < width; ++x) {
            int yVal = yRow[x];
            int co = coRow[x];
            int cg = cgRow[x];

            int t = yVal - (cg >> 1);
            int g = cg + t;
            int b = t - (co >> 1);
            int r = co + b;

            row[x * 4 + 0] = static_cast<uint8_t>(std::max(0, std::min(255, b)));
            row[x * 4 + 1] = static_cast<uint8_t>(std::max(0, std::min(255, g)));
            row[x * 4 + 2] = static_cast<uint8_t>(std::max(0, std::min(255, r)));
            row[x * 4 + 3] = 255;
        }
    }
}

} // namespace avx2
} // namespace omnidesk
