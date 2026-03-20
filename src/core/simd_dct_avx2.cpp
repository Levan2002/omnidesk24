#include "core/simd_dct.h"

#include <immintrin.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace avx2 {

// Fixed-point scale factor matching scalar implementation
static constexpr int FP_SHIFT = 12;
static constexpr int FP_ROUND = 1 << (FP_SHIFT - 1);

// Precomputed 8x8 cosine table: row k contains the 8 basis coefficients for frequency k.
// table[k][n] = round(alpha[k] * cos(pi*(2n+1)*k / 16) * 4096)
static int32_t cosTable8[8][8];
static bool tablesInitialized = false;

static void initCosTables() {
    if (tablesInitialized) return;

    for (int k = 0; k < 8; ++k) {
        double alpha = (k == 0) ? std::sqrt(1.0 / 8) : std::sqrt(2.0 / 8);
        for (int n = 0; n < 8; ++n) {
            double val = alpha * std::cos(M_PI * (2.0 * n + 1.0) * k / 16.0);
            cosTable8[k][n] = static_cast<int32_t>(std::round(val * (1 << FP_SHIFT)));
        }
    }
    tablesInitialized = true;
}

// AVX2 1D DCT-II on 8 elements: Y[k] = sum_n(x[n] * A[k][n]) >> FP_SHIFT
// Processes one row/column at a time using 256-bit operations.
// src: 8 int16 input values (with srcStep stride between elements)
// dst: 8 int16 output values (with dstStep stride between elements)
static inline void dct1D_avx2_8(const int16_t* src, int srcStep,
                                  int16_t* dst, int dstStep) {
    // Load 8 input values into a 128-bit register, then broadcast to 256-bit
    // for paired multiply-accumulate with cos table entries.
    //
    // For each output k, we need: sum_n(x[n] * cosTable[k][n])
    // We process 2 output frequencies at a time using _mm256_madd_epi16.

    // Gather 8 input values (may have non-unit stride)
    int16_t xvals[8];
    for (int i = 0; i < 8; ++i) xvals[i] = src[i * srcStep];

    // Load input as 128-bit and broadcast to both lanes of 256-bit
    __m128i x128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(xvals));
    __m256i x = _mm256_broadcastsi128_si256(x128);

    // For each pair of output frequencies, load cos coefficients and madd
    for (int k = 0; k < 8; k += 2) {
        // Pack cosTable[k][0..7] into lo 128 bits, cosTable[k+1][0..7] into hi 128 bits
        // But _mm256_madd_epi16 does paired multiply-accumulate on 16-bit lanes:
        // result[i] = a[2i]*b[2i] + a[2i+1]*b[2i+1]
        // We need 32-bit cosine values packed as 16-bit pairs with x values.

        // Actually, use simpler approach: _mm256_madd_epi16 with 16-bit cos table.
        // Truncate cos table to 16-bit (fits since max ~4096*sqrt(2) < 6000 < 32767)
        int16_t cosK[8], cosK1[8];
        for (int n = 0; n < 8; ++n) {
            cosK[n] = static_cast<int16_t>(cosTable8[k][n]);
            cosK1[n] = static_cast<int16_t>(cosTable8[k + 1][n]);
        }

        // Interleave: [x0,c0, x1,c1, x2,c2, x3,c3 | x4,c4, x5,c5, x6,c6, x7,c7]
        // Then madd gives: [x0*c0+x1*c1, x2*c2+x3*c3 | x4*c4+x5*c5, x6*c6+x7*c7]
        __m128i cos_k = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cosK));
        __m128i cos_k1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cosK1));

        // madd for frequency k: x * cosK -> 4 partial int32 sums
        __m256i xwide = _mm256_cvtepi16_epi32(x128);
        __m256i cwide = _mm256_cvtepi16_epi32(cos_k);
        __m256i prod_k = _mm256_mullo_epi32(xwide, cwide);

        // Horizontal sum of 8 int32 values
        // [p0, p1, p2, p3, p4, p5, p6, p7]
        __m128i lo = _mm256_castsi256_si128(prod_k);
        __m128i hi = _mm256_extracti128_si256(prod_k, 1);
        __m128i sum4 = _mm_add_epi32(lo, hi);  // [p0+p4, p1+p5, p2+p6, p3+p7]
        __m128i sum2 = _mm_add_epi32(sum4, _mm_shuffle_epi32(sum4, 0x4E)); // [s01+s23, ...]
        __m128i sum1 = _mm_add_epi32(sum2, _mm_shuffle_epi32(sum2, 0x01));
        int32_t result_k = _mm_cvtsi128_si32(sum1);
        result_k = (result_k + FP_ROUND) >> FP_SHIFT;
        dst[k * dstStep] = static_cast<int16_t>(std::max(-32768, std::min(32767, result_k)));

        // Same for frequency k+1
        __m256i cwide1 = _mm256_cvtepi16_epi32(cos_k1);
        __m256i prod_k1 = _mm256_mullo_epi32(xwide, cwide1);
        __m128i lo1 = _mm256_castsi256_si128(prod_k1);
        __m128i hi1 = _mm256_extracti128_si256(prod_k1, 1);
        __m128i sum4_1 = _mm_add_epi32(lo1, hi1);
        __m128i sum2_1 = _mm_add_epi32(sum4_1, _mm_shuffle_epi32(sum4_1, 0x4E));
        __m128i sum1_1 = _mm_add_epi32(sum2_1, _mm_shuffle_epi32(sum2_1, 0x01));
        int32_t result_k1 = _mm_cvtsi128_si32(sum1_1);
        result_k1 = (result_k1 + FP_ROUND) >> FP_SHIFT;
        dst[(k + 1) * dstStep] = static_cast<int16_t>(std::max(-32768, std::min(32767, result_k1)));
    }
}

// AVX2 batch 1D DCT: Process all 8 rows of an 8x8 block simultaneously.
// Each row is an independent 1D DCT. We process all 8 rows in parallel
// by treating the 8x8 block as 8 rows of 8 values.
static void dct8x8_rows_avx2(const int16_t* src, int srcStride,
                               int16_t* dst, int dstStride) {
    // For each output frequency k, compute all 8 rows simultaneously.
    // Row y output[k] = sum_n(src[y][n] * cosTable[k][n])
    //
    // Load cos[k][0..7] as a broadcast across all rows,
    // multiply column-by-column, accumulate.

    // Load all 8 rows into registers
    __m128i rows[8];
    for (int y = 0; y < 8; ++y) {
        rows[y] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + y * srcStride));
    }

    for (int k = 0; k < 8; ++k) {
        // For this frequency, accumulate: result[y] = sum_n(rows[y][n] * cos[k][n])
        // Process by column pairs using _mm_madd_epi16

        // cos[k][0..7] as 16-bit
        int16_t cosK[8];
        for (int n = 0; n < 8; ++n) {
            cosK[n] = static_cast<int16_t>(cosTable8[k][n]);
        }
        __m128i cvec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cosK));

        for (int y = 0; y < 8; ++y) {
            // _mm_madd_epi16: [a0*b0+a1*b1, a2*b2+a3*b3, a4*b4+a5*b5, a6*b6+a7*b7]
            __m128i prod = _mm_madd_epi16(rows[y], cvec);
            // Horizontal sum of 4 int32 values
            __m128i sum2 = _mm_add_epi32(prod, _mm_shuffle_epi32(prod, 0x4E));
            __m128i sum1 = _mm_add_epi32(sum2, _mm_shuffle_epi32(sum2, 0x01));
            int32_t result = _mm_cvtsi128_si32(sum1);
            result = (result + FP_ROUND) >> FP_SHIFT;
            dst[y * dstStride + k] = static_cast<int16_t>(std::max(-32768, std::min(32767, result)));
        }
    }
}

// Column transform: same as row transform but with transposed access.
static void dct8x8_cols_avx2(const int16_t* src, int srcStride,
                               int16_t* dst, int dstStride) {
    // For column transform, we transpose, do row transform, transpose back.
    // Or equivalently, for each output frequency k and column x:
    // dst[k][x] = sum_y(src[y][x] * cos[k][y])

    // We can reuse the row approach by transposing first.
    // Transpose src into temp, do row DCT, transpose result.
    int16_t temp[64], temp2[64];

    // Transpose 8x8
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            temp[x * 8 + y] = src[y * srcStride + x];

    dct8x8_rows_avx2(temp, 8, temp2, 8);

    // Transpose result back
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            dst[y * dstStride + x] = temp2[x * 8 + y];
}

void dctForward(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize) {
    initCosTables();

    if (blockSize == 8) {
        // AVX2-optimized 8x8 DCT: rows then columns
        int16_t temp[64];
        dct8x8_rows_avx2(src, srcStride, temp, 8);
        dct8x8_cols_avx2(temp, 8, dst, dstStride);
        return;
    }

    // Fall back to scalar for 4x4 and 16x16
    omnidesk::dctForward(src, srcStride, dst, dstStride, blockSize);
}

void dctInverse(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize) {
    initCosTables();

    if (blockSize == 8) {
        // For inverse DCT with orthogonal basis, the formula is the same:
        // x[n] = sum_k(Y[k] * A[k][n])
        // which is the same multiply-accumulate pattern, just the roles of k,n swapped.
        // With our orthogonal table, IDCT columns then IDCT rows.
        int16_t temp[64];
        dct8x8_cols_avx2(src, srcStride, temp, 8);
        dct8x8_rows_avx2(temp, 8, dst, dstStride);
        return;
    }

    omnidesk::dctInverse(src, srcStride, dst, dstStride, blockSize);
}

} // namespace avx2
} // namespace omnidesk
