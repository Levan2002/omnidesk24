#include "core/simd_ycocg.h"
#include "core/simd_utils.h"  // cpuSupportsAVX2()

#ifdef OMNIDESK_X86_64
#include <immintrin.h>
#endif

#include <algorithm>

namespace omnidesk {

// Scalar fallback / SSE2 baseline for YCoCg-R forward transform
static void bgraToYCoCgR_scalar(const uint8_t* bgra, int width, int height, int bgraStride,
                                 int16_t* yPlane, int16_t* coPlane, int16_t* cgPlane) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        int16_t* yRow = yPlane + y * width;
        int16_t* coRow = coPlane + y * width;
        int16_t* cgRow = cgPlane + y * width;

        for (int x = 0; x < width; ++x) {
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

#ifdef OMNIDESK_X86_64
static void bgraToYCoCgR_sse2(const uint8_t* bgra, int width, int height, int bgraStride,
                                int16_t* yPlane, int16_t* coPlane, int16_t* cgPlane) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i mask_ff = _mm_set1_epi32(0xFF);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        int16_t* yRow = yPlane + y * width;
        int16_t* coRow = coPlane + y * width;
        int16_t* cgRow = cgPlane + y * width;

        int x = 0;
        // Process 4 pixels at a time (16 bytes = 1 SSE register of BGRA)
        for (; x + 3 < width; x += 4) {
            __m128i pix = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x * 4));

            // Extract B, G, R as 32-bit integers
            __m128i b32 = _mm_and_si128(pix, mask_ff);
            __m128i g32 = _mm_and_si128(_mm_srli_epi32(pix, 8), mask_ff);
            __m128i r32 = _mm_and_si128(_mm_srli_epi32(pix, 16), mask_ff);

            // Co = R - B (signed 32-bit)
            __m128i co32 = _mm_sub_epi32(r32, b32);

            // t = B + (Co >> 1)   (arithmetic shift)
            __m128i t32 = _mm_add_epi32(b32, _mm_srai_epi32(co32, 1));

            // Cg = G - t
            __m128i cg32 = _mm_sub_epi32(g32, t32);

            // Y = t + (Cg >> 1)
            __m128i y32 = _mm_add_epi32(t32, _mm_srai_epi32(cg32, 1));

            // Pack 32-bit to 16-bit (signed pack)
            __m128i y16 = _mm_packs_epi32(y32, zero);   // [y0,y1,y2,y3,0,0,0,0]
            __m128i co16 = _mm_packs_epi32(co32, zero);
            __m128i cg16 = _mm_packs_epi32(cg32, zero);

            // Store 4 int16_t values (8 bytes)
            _mm_storel_epi64(reinterpret_cast<__m128i*>(yRow + x), y16);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(coRow + x), co16);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(cgRow + x), cg16);
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
#endif

void bgraToYCoCgR(const uint8_t* bgra, int width, int height, int bgraStride,
                   int16_t* yPlane, int16_t* coPlane, int16_t* cgPlane) {
    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        avx2::bgraToYCoCgR(bgra, width, height, bgraStride, yPlane, coPlane, cgPlane);
        return;
    }
#ifdef OMNIDESK_X86_64
    bgraToYCoCgR_sse2(bgra, width, height, bgraStride, yPlane, coPlane, cgPlane);
#else
    bgraToYCoCgR_scalar(bgra, width, height, bgraStride, yPlane, coPlane, cgPlane);
#endif
}

// Scalar fallback / SSE2 baseline for YCoCg-R inverse transform
static void yCoCgRToBgra_scalar(const int16_t* yPlane, const int16_t* coPlane, const int16_t* cgPlane,
                                 int width, int height,
                                 uint8_t* bgra, int bgraStride) {
    for (int y = 0; y < height; ++y) {
        const int16_t* yRow = yPlane + y * width;
        const int16_t* coRow = coPlane + y * width;
        const int16_t* cgRow = cgPlane + y * width;
        uint8_t* row = bgra + y * bgraStride;

        for (int x = 0; x < width; ++x) {
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

#ifdef OMNIDESK_X86_64
static void yCoCgRToBgra_sse2(const int16_t* yPlane, const int16_t* coPlane, const int16_t* cgPlane,
                                int width, int height,
                                uint8_t* bgra, int bgraStride) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i alpha = _mm_set1_epi32(0xFF000000);

    for (int y = 0; y < height; ++y) {
        const int16_t* yRow = yPlane + y * width;
        const int16_t* coRow = coPlane + y * width;
        const int16_t* cgRow = cgPlane + y * width;
        uint8_t* row = bgra + y * bgraStride;

        int x = 0;
        for (; x + 3 < width; x += 4) {
            // Load 4 values from each plane
            __m128i y16 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(yRow + x));
            __m128i co16 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(coRow + x));
            __m128i cg16 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(cgRow + x));

            // Sign-extend 16->32
            __m128i y32 = _mm_srai_epi32(_mm_unpacklo_epi16(y16, y16), 16);
            __m128i co32 = _mm_srai_epi32(_mm_unpacklo_epi16(co16, co16), 16);
            __m128i cg32 = _mm_srai_epi32(_mm_unpacklo_epi16(cg16, cg16), 16);

            // t = Y - (Cg >> 1)
            __m128i t32 = _mm_sub_epi32(y32, _mm_srai_epi32(cg32, 1));

            // G = Cg + t
            __m128i g32 = _mm_add_epi32(cg32, t32);

            // B = t - (Co >> 1)
            __m128i b32 = _mm_sub_epi32(t32, _mm_srai_epi32(co32, 1));

            // R = Co + B
            __m128i r32 = _mm_add_epi32(co32, b32);

            // Clamp to [0, 255] using 32-bit ops.
            // For valid YCoCg-R input from [0,255] BGRA, values are always in range,
            // but clamp for safety. Use packus for correct clamping:
            // pack 32->16 signed, then 16->8 unsigned saturating, then unpack back.
            // Simpler: just mask to 8 bits since reversible transform guarantees range.
            __m128i mask8 = _mm_set1_epi32(0xFF);
            b32 = _mm_and_si128(b32, mask8);
            g32 = _mm_and_si128(g32, mask8);
            r32 = _mm_and_si128(r32, mask8);

            // Pack to BGRA: B | (G << 8) | (R << 16) | (0xFF << 24)
            __m128i bgra32 = _mm_or_si128(
                _mm_or_si128(b32, _mm_slli_epi32(g32, 8)),
                _mm_or_si128(_mm_slli_epi32(r32, 16), alpha)
            );

            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x * 4), bgra32);
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
#endif

void yCoCgRToBgra(const int16_t* yPlane, const int16_t* coPlane, const int16_t* cgPlane,
                   int width, int height,
                   uint8_t* bgra, int bgraStride) {
    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        avx2::yCoCgRToBgra(yPlane, coPlane, cgPlane, width, height, bgra, bgraStride);
        return;
    }
#ifdef OMNIDESK_X86_64
    yCoCgRToBgra_sse2(yPlane, coPlane, cgPlane, width, height, bgra, bgraStride);
#else
    yCoCgRToBgra_scalar(yPlane, coPlane, cgPlane, width, height, bgra, bgraStride);
#endif
}

} // namespace omnidesk
