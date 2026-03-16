#include "core/simd_utils.h"

#ifdef OMNIDESK_X86_64
#include <immintrin.h>
#endif

namespace omnidesk {
namespace avx2 {

#ifdef OMNIDESK_X86_64

void bgraToI420(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uPlane, int uStride,
                uint8_t* vPlane, int vStride) {
    // AVX2 processes 8 BGRA pixels (32 bytes) at a time for Y plane
    // BT.601: Y = (66*R + 129*G + 25*B + 128) >> 8 + 16

    const __m256i coeff_r = _mm256_set1_epi16(66);
    const __m256i coeff_g = _mm256_set1_epi16(129);
    const __m256i coeff_b = _mm256_set1_epi16(25);
    const __m256i offset_y = _mm256_set1_epi16(16);
    const __m256i round_add = _mm256_set1_epi16(128);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        uint8_t* yRow = yPlane + y * yStride;

        int x = 0;
        // AVX2 path: process 8 pixels per iteration
        for (; x + 7 < width; x += 8) {
            // Load 8 BGRA pixels (32 bytes)
            __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + x * 4));

            // Extract B, G, R channels and zero-extend to 16-bit
            // BGRA layout: B0 G0 R0 A0 B1 G1 R1 A1 ...
            __m256i b16 = _mm256_and_si256(pixels, _mm256_set1_epi32(0xFF));
            __m256i g16 = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), _mm256_set1_epi32(0xFF));
            __m256i r16 = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), _mm256_set1_epi32(0xFF));

            // Pack 32-bit to 16-bit (we only need lower 16 bits)
            // Use packs with saturation
            __m256i b_lo = _mm256_packus_epi32(b16, _mm256_setzero_si256());
            __m256i g_lo = _mm256_packus_epi32(g16, _mm256_setzero_si256());
            __m256i r_lo = _mm256_packus_epi32(r16, _mm256_setzero_si256());

            // Y = (66*R + 129*G + 25*B + 128) >> 8 + 16
            __m256i yy = _mm256_add_epi16(
                _mm256_add_epi16(
                    _mm256_mullo_epi16(r_lo, coeff_r),
                    _mm256_mullo_epi16(g_lo, coeff_g)
                ),
                _mm256_add_epi16(
                    _mm256_mullo_epi16(b_lo, coeff_b),
                    round_add
                )
            );
            yy = _mm256_add_epi16(_mm256_srli_epi16(yy, 8), offset_y);

            // Pack to uint8 and store
            __m256i y8 = _mm256_packus_epi16(yy, _mm256_setzero_si256());

            // Store 8 Y values (scattered due to 256-bit lane crossing)
            // Extract and store manually for correctness
            alignas(32) uint8_t yBuf[32];
            _mm256_store_si256(reinterpret_cast<__m256i*>(yBuf), y8);
            // Lane 0: indices 0-3, Lane 1: indices 16-19
            yRow[x+0] = yBuf[0]; yRow[x+1] = yBuf[1];
            yRow[x+2] = yBuf[2]; yRow[x+3] = yBuf[3];
            yRow[x+4] = yBuf[16]; yRow[x+5] = yBuf[17];
            yRow[x+6] = yBuf[18]; yRow[x+7] = yBuf[19];
        }

        // Scalar remainder
        for (; x < width; ++x) {
            int b = row[x*4+0], g = row[x*4+1], r = row[x*4+2];
            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[x] = static_cast<uint8_t>(std::min(std::max(yVal, 16), 235));
        }

        // Chroma (U, V) - same as SSE2 path but could be AVX2-optimized later
        if (y % 2 == 0 && y + 1 < height) {
            const uint8_t* row2 = bgra + (y + 1) * bgraStride;
            uint8_t* uRow = uPlane + (y / 2) * uStride;
            uint8_t* vRow = vPlane + (y / 2) * vStride;

            for (int cx = 0; cx < width - 1; cx += 2) {
                int b = (row[cx*4+0] + row[(cx+1)*4+0] + row2[cx*4+0] + row2[(cx+1)*4+0]) / 4;
                int g = (row[cx*4+1] + row[(cx+1)*4+1] + row2[cx*4+1] + row2[(cx+1)*4+1]) / 4;
                int r = (row[cx*4+2] + row[(cx+1)*4+2] + row2[cx*4+2] + row2[(cx+1)*4+2]) / 4;

                int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                uRow[cx / 2] = static_cast<uint8_t>(std::min(std::max(u, 16), 240));
                vRow[cx / 2] = static_cast<uint8_t>(std::min(std::max(v, 16), 240));
            }
        }
    }
}

bool blocksDiffer(const uint8_t* blockA, const uint8_t* blockB,
                  int stride, int blockSize, int threshold) {
    int diffCount = 0;
    const __m256i tolerance = _mm256_set1_epi8(2);

    for (int y = 0; y < blockSize; ++y) {
        const uint8_t* a = blockA + y * stride;
        const uint8_t* b = blockB + y * stride;
        int bytesPerRow = blockSize * 4;

        int x = 0;
        for (; x + 31 < bytesPerRow; x += 32) {
            __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + x));
            __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + x));

            // Absolute difference
            __m256i diff = _mm256_or_si256(
                _mm256_subs_epu8(va, vb),
                _mm256_subs_epu8(vb, va)
            );

            // Compare with tolerance
            __m256i exceed = _mm256_subs_epu8(diff, tolerance);
            int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(exceed, _mm256_setzero_si256()));
            diffCount += 32 - __builtin_popcount(mask);

            if (diffCount > threshold) return true;
        }

        // Scalar remainder
        for (; x < bytesPerRow; ++x) {
            if (std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x])) > 2) {
                if (++diffCount > threshold) return true;
            }
        }
    }
    return diffCount > threshold;
}

#else

// Non-x86: stubs that should never be called (cpuSupportsAVX2 returns false)
void bgraToI420(const uint8_t*, int, int, int,
                uint8_t*, int, uint8_t*, int, uint8_t*, int) {}
bool blocksDiffer(const uint8_t*, const uint8_t*, int, int, int) { return false; }

#endif

} // namespace avx2
} // namespace omnidesk
