#include "core/simd_utils.h"

#include <immintrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace omnidesk {
namespace avx2 {

// Helper: extract R, G, B from 8 BGRA pixels in a __m256i, return as packed 16-bit
// After _mm256_packus_epi32, the 8 values are split across 128-bit lanes:
// lane0: [px0 px1 px2 px3  0 0 0 0], lane1: [px4 px5 px6 px7  0 0 0 0]
static inline void extractBGR_avx2(const __m256i& pixels,
                                    __m256i& b16, __m256i& g16, __m256i& r16) {
    const __m256i mask_ff = _mm256_set1_epi32(0xFF);
    __m256i b32 = _mm256_and_si256(pixels, mask_ff);
    __m256i g32 = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), mask_ff);
    __m256i r32 = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), mask_ff);
    b16 = _mm256_packus_epi32(b32, _mm256_setzero_si256());
    g16 = _mm256_packus_epi32(g32, _mm256_setzero_si256());
    r16 = _mm256_packus_epi32(r32, _mm256_setzero_si256());
}

// Helper: compute Y for 8 pixels (values in packed 16-bit lanes),
// store 8 bytes to yRow. Handles AVX2 lane-crossing in packus.
static inline void computeAndStoreY_avx2(const __m256i& r16, const __m256i& g16, const __m256i& b16,
                                          const __m256i& cr, const __m256i& cg, const __m256i& cb,
                                          const __m256i& rnd, uint8_t* yRow) {
    __m256i yy = _mm256_add_epi16(
        _mm256_add_epi16(
            _mm256_mullo_epi16(r16, cr),
            _mm256_mullo_epi16(g16, cg)
        ),
        _mm256_add_epi16(
            _mm256_mullo_epi16(b16, cb),
            rnd
        )
    );
    yy = _mm256_srli_epi16(yy, 8);

    __m256i y8 = _mm256_packus_epi16(yy, _mm256_setzero_si256());
    // After packus: lane0=[px0..px3, 0,0,0,0], lane1=[px4..px7, 0,0,0,0]
    alignas(32) uint8_t yBuf[32];
    _mm256_store_si256(reinterpret_cast<__m256i*>(yBuf), y8);
    yRow[0] = yBuf[0]; yRow[1] = yBuf[1];
    yRow[2] = yBuf[2]; yRow[3] = yBuf[3];
    yRow[4] = yBuf[16]; yRow[5] = yBuf[17];
    yRow[6] = yBuf[18]; yRow[7] = yBuf[19];
}

void bgraToI420(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uPlane, int uStride,
                uint8_t* vPlane, int vStride) {
    // BT.601 full range: Y = (77*R + 150*G + 29*B + 128) >> 8
    const __m256i coeff_r_y = _mm256_set1_epi16(77);
    const __m256i coeff_g_y = _mm256_set1_epi16(150);
    const __m256i coeff_b_y = _mm256_set1_epi16(29);
    const __m256i round_add = _mm256_set1_epi16(128);

    // Chroma coefficients (signed 16-bit):
    // U = (-43*R - 85*G + 128*B + 128) >> 8 + 128
    // V = (128*R - 107*G - 21*B + 128) >> 8 + 128
    const __m256i coeff_r_u = _mm256_set1_epi16(-43);
    const __m256i coeff_g_u = _mm256_set1_epi16(-85);
    const __m256i coeff_b_u = _mm256_set1_epi16(128);
    const __m256i coeff_r_v = _mm256_set1_epi16(128);
    const __m256i coeff_g_v = _mm256_set1_epi16(-107);
    const __m256i coeff_b_v = _mm256_set1_epi16(-21);
    const __m256i offset_128 = _mm256_set1_epi16(128);

    // Process two rows at a time (needed for 2x2 chroma subsampling)
    for (int y = 0; y < height; y += 2) {
        const uint8_t* row0 = bgra + y * bgraStride;
        const uint8_t* row1 = (y + 1 < height) ? bgra + (y + 1) * bgraStride : row0;
        uint8_t* yRow0 = yPlane + y * yStride;
        uint8_t* yRow1 = yPlane + (y + 1) * yStride;
        uint8_t* uRow  = uPlane + (y / 2) * uStride;
        uint8_t* vRow  = vPlane + (y / 2) * vStride;

        int x = 0;
        // AVX2 main loop: 8 pixels at a time (32 bytes)
        for (; x + 7 < width; x += 8) {
            // --- Row 0: Y for 8 pixels ---
            __m256i pix0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row0 + x * 4));
            __m256i b0_16, g0_16, r0_16;
            extractBGR_avx2(pix0, b0_16, g0_16, r0_16);
            computeAndStoreY_avx2(r0_16, g0_16, b0_16,
                                   coeff_r_y, coeff_g_y, coeff_b_y, round_add, yRow0 + x);

            // --- Row 1: Y for 8 pixels ---
            __m256i pix1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row1 + x * 4));
            __m256i b1_16, g1_16, r1_16;
            extractBGR_avx2(pix1, b1_16, g1_16, r1_16);
            if (y + 1 < height) {
                computeAndStoreY_avx2(r1_16, g1_16, b1_16,
                                       coeff_r_y, coeff_g_y, coeff_b_y, round_add, yRow1 + x);
            }

            // --- Chroma: AVX2 vectorized 2x2 subsampling ---
            // Sum row0 + row1 vertically (values in 16-bit, max 510)
            __m256i b_vsum = _mm256_add_epi16(b0_16, b1_16);
            __m256i g_vsum = _mm256_add_epi16(g0_16, g1_16);
            __m256i r_vsum = _mm256_add_epi16(r0_16, r1_16);

            // Horizontal pair-add using AVX2 hadd:
            // After packus, layout in each 128-bit lane is [px0 px1 px2 px3 0 0 0 0].
            // We need (px0+px1)/4 and (px2+px3)/4 per lane.
            // hadd adds adjacent 16-bit pairs within each lane.
            __m256i b_hadd = _mm256_hadd_epi16(b_vsum, _mm256_setzero_si256());
            __m256i g_hadd = _mm256_hadd_epi16(g_vsum, _mm256_setzero_si256());
            __m256i r_hadd = _mm256_hadd_epi16(r_vsum, _mm256_setzero_si256());

            // Now hadd result has 2x2 block sums in positions 0,1 of each lane.
            // Add rounding (+2) and divide by 4 (>>2) to get averages.
            const __m256i round2 = _mm256_set1_epi16(2);
            __m256i bAvg = _mm256_srli_epi16(_mm256_add_epi16(b_hadd, round2), 2);
            __m256i gAvg = _mm256_srli_epi16(_mm256_add_epi16(g_hadd, round2), 2);
            __m256i rAvg = _mm256_srli_epi16(_mm256_add_epi16(r_hadd, round2), 2);

            // Compute U = (-43*R - 85*G + 128*B + 128) >> 8 + 128
            __m256i u_val = _mm256_add_epi16(
                _mm256_add_epi16(
                    _mm256_mullo_epi16(rAvg, coeff_r_u),
                    _mm256_mullo_epi16(gAvg, coeff_g_u)
                ),
                _mm256_add_epi16(
                    _mm256_mullo_epi16(bAvg, coeff_b_u),
                    round_add
                )
            );
            u_val = _mm256_srai_epi16(u_val, 8);
            u_val = _mm256_add_epi16(u_val, offset_128);

            // Compute V = (128*R - 107*G - 21*B + 128) >> 8 + 128
            __m256i v_val = _mm256_add_epi16(
                _mm256_add_epi16(
                    _mm256_mullo_epi16(rAvg, coeff_r_v),
                    _mm256_mullo_epi16(gAvg, coeff_g_v)
                ),
                _mm256_add_epi16(
                    _mm256_mullo_epi16(bAvg, coeff_b_v),
                    round_add
                )
            );
            v_val = _mm256_srai_epi16(v_val, 8);
            v_val = _mm256_add_epi16(v_val, offset_128);

            // Clamp to [0, 255]
            __m256i u_8 = _mm256_packus_epi16(u_val, _mm256_setzero_si256());
            __m256i v_8 = _mm256_packus_epi16(v_val, _mm256_setzero_si256());

            // Store 4 U and 4 V values (8 pixels -> 4 chroma samples)
            // packus layout: lane0=[u0,u1,0..], lane1=[u2,u3,0..]
            alignas(32) uint8_t uBuf[32], vBuf[32];
            _mm256_store_si256(reinterpret_cast<__m256i*>(uBuf), u_8);
            _mm256_store_si256(reinterpret_cast<__m256i*>(vBuf), v_8);
            int cx = x / 2;
            uRow[cx+0] = uBuf[0]; uRow[cx+1] = uBuf[1];
            uRow[cx+2] = uBuf[16]; uRow[cx+3] = uBuf[17];
            vRow[cx+0] = vBuf[0]; vRow[cx+1] = vBuf[1];
            vRow[cx+2] = vBuf[16]; vRow[cx+3] = vBuf[17];
        }

        // Scalar remainder for Y and chroma
        for (; x < width; ++x) {
            int b = row0[x*4+0], g = row0[x*4+1], r = row0[x*4+2];
            int yVal = (77 * r + 150 * g + 29 * b + 128) >> 8;
            yRow0[x] = static_cast<uint8_t>(std::min(std::max(yVal, 0), 255));

            if (y + 1 < height) {
                int b1 = row1[x*4+0], g1 = row1[x*4+1], r1 = row1[x*4+2];
                int yVal1 = (77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8;
                yRow1[x] = static_cast<uint8_t>(std::min(std::max(yVal1, 0), 255));
            }

            if (x % 2 == 0 && x + 1 < width && y + 1 < height) {
                int b = (row0[x*4+0] + row0[(x+1)*4+0] + row1[x*4+0] + row1[(x+1)*4+0]) / 4;
                int g = (row0[x*4+1] + row0[(x+1)*4+1] + row1[x*4+1] + row1[(x+1)*4+1]) / 4;
                int r = (row0[x*4+2] + row0[(x+1)*4+2] + row1[x*4+2] + row1[(x+1)*4+2]) / 4;

                int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                uRow[x / 2] = static_cast<uint8_t>(std::min(std::max(u, 0), 255));
                vRow[x / 2] = static_cast<uint8_t>(std::min(std::max(v, 0), 255));
            }
        }
    }
}

void bgraToNV12(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uvPlane, int uvStride) {
    const __m256i coeff_r_y = _mm256_set1_epi16(77);
    const __m256i coeff_g_y = _mm256_set1_epi16(150);
    const __m256i coeff_b_y = _mm256_set1_epi16(29);
    const __m256i round_add = _mm256_set1_epi16(128);

    const __m256i coeff_r_u = _mm256_set1_epi16(-43);
    const __m256i coeff_g_u = _mm256_set1_epi16(-85);
    const __m256i coeff_b_u = _mm256_set1_epi16(128);
    const __m256i coeff_r_v = _mm256_set1_epi16(128);
    const __m256i coeff_g_v = _mm256_set1_epi16(-107);
    const __m256i coeff_b_v = _mm256_set1_epi16(-21);
    const __m256i offset_128 = _mm256_set1_epi16(128);

    for (int y = 0; y < height; y += 2) {
        const uint8_t* row0 = bgra + y * bgraStride;
        const uint8_t* row1 = (y + 1 < height) ? bgra + (y + 1) * bgraStride : row0;
        uint8_t* yRow0 = yPlane + y * yStride;
        uint8_t* yRow1 = yPlane + (y + 1) * yStride;
        uint8_t* uvRow = uvPlane + (y / 2) * uvStride;

        int x = 0;
        for (; x + 7 < width; x += 8) {
            // Row 0: Y
            __m256i pix0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row0 + x * 4));
            __m256i b0_16, g0_16, r0_16;
            extractBGR_avx2(pix0, b0_16, g0_16, r0_16);
            computeAndStoreY_avx2(r0_16, g0_16, b0_16,
                                   coeff_r_y, coeff_g_y, coeff_b_y, round_add, yRow0 + x);

            // Row 1: Y
            __m256i pix1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row1 + x * 4));
            __m256i b1_16, g1_16, r1_16;
            extractBGR_avx2(pix1, b1_16, g1_16, r1_16);
            if (y + 1 < height) {
                computeAndStoreY_avx2(r1_16, g1_16, b1_16,
                                       coeff_r_y, coeff_g_y, coeff_b_y, round_add, yRow1 + x);
            }

            // Chroma: 2x2 subsampling → interleaved UV
            __m256i b_vsum = _mm256_add_epi16(b0_16, b1_16);
            __m256i g_vsum = _mm256_add_epi16(g0_16, g1_16);
            __m256i r_vsum = _mm256_add_epi16(r0_16, r1_16);

            __m256i b_hadd = _mm256_hadd_epi16(b_vsum, _mm256_setzero_si256());
            __m256i g_hadd = _mm256_hadd_epi16(g_vsum, _mm256_setzero_si256());
            __m256i r_hadd = _mm256_hadd_epi16(r_vsum, _mm256_setzero_si256());

            const __m256i round2 = _mm256_set1_epi16(2);
            __m256i bAvg = _mm256_srli_epi16(_mm256_add_epi16(b_hadd, round2), 2);
            __m256i gAvg = _mm256_srli_epi16(_mm256_add_epi16(g_hadd, round2), 2);
            __m256i rAvg = _mm256_srli_epi16(_mm256_add_epi16(r_hadd, round2), 2);

            // U values
            __m256i u_val = _mm256_add_epi16(
                _mm256_add_epi16(
                    _mm256_mullo_epi16(rAvg, coeff_r_u),
                    _mm256_mullo_epi16(gAvg, coeff_g_u)
                ),
                _mm256_add_epi16(
                    _mm256_mullo_epi16(bAvg, coeff_b_u),
                    round_add
                )
            );
            u_val = _mm256_srai_epi16(u_val, 8);
            u_val = _mm256_add_epi16(u_val, offset_128);

            // V values
            __m256i v_val = _mm256_add_epi16(
                _mm256_add_epi16(
                    _mm256_mullo_epi16(rAvg, coeff_r_v),
                    _mm256_mullo_epi16(gAvg, coeff_g_v)
                ),
                _mm256_add_epi16(
                    _mm256_mullo_epi16(bAvg, coeff_b_v),
                    round_add
                )
            );
            v_val = _mm256_srai_epi16(v_val, 8);
            v_val = _mm256_add_epi16(v_val, offset_128);

            // Clamp and interleave UV
            __m256i u_8 = _mm256_packus_epi16(u_val, _mm256_setzero_si256());
            __m256i v_8 = _mm256_packus_epi16(v_val, _mm256_setzero_si256());

            alignas(32) uint8_t uBuf[32], vBuf[32];
            _mm256_store_si256(reinterpret_cast<__m256i*>(uBuf), u_8);
            _mm256_store_si256(reinterpret_cast<__m256i*>(vBuf), v_8);

            // Interleave into NV12 UV plane: U0 V0 U1 V1 U2 V2 U3 V3
            int cx = x;  // NV12: UV row has same width as Y row
            uvRow[cx+0] = uBuf[0]; uvRow[cx+1] = vBuf[0];
            uvRow[cx+2] = uBuf[1]; uvRow[cx+3] = vBuf[1];
            uvRow[cx+4] = uBuf[16]; uvRow[cx+5] = vBuf[16];
            uvRow[cx+6] = uBuf[17]; uvRow[cx+7] = vBuf[17];
        }

        // Scalar remainder
        for (; x < width; ++x) {
            int b = row0[x*4+0], g = row0[x*4+1], r = row0[x*4+2];
            int yVal = (77 * r + 150 * g + 29 * b + 128) >> 8;
            yRow0[x] = static_cast<uint8_t>(std::min(std::max(yVal, 0), 255));

            if (y + 1 < height) {
                int b1 = row1[x*4+0], g1 = row1[x*4+1], r1 = row1[x*4+2];
                int yVal1 = (77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8;
                yRow1[x] = static_cast<uint8_t>(std::min(std::max(yVal1, 0), 255));
            }

            if (x % 2 == 0 && x + 1 < width && y + 1 < height) {
                int b = (row0[x*4+0] + row0[(x+1)*4+0] + row1[x*4+0] + row1[(x+1)*4+0]) / 4;
                int g = (row0[x*4+1] + row0[(x+1)*4+1] + row1[x*4+1] + row1[(x+1)*4+1]) / 4;
                int r = (row0[x*4+2] + row0[(x+1)*4+2] + row1[x*4+2] + row1[(x+1)*4+2]) / 4;
                int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                uvRow[x + 0] = static_cast<uint8_t>(std::min(std::max(u, 0), 255));
                uvRow[x + 1] = static_cast<uint8_t>(std::min(std::max(v, 0), 255));
            }
        }
    }
}

void bgraToI420Region(const uint8_t* bgra, int /*width*/, int /*height*/, int bgraStride,
                      uint8_t* yPlane, int yStride,
                      uint8_t* uPlane, int uStride,
                      uint8_t* vPlane, int vStride,
                      int rx, int ry, int rw, int rh) {
    // Delegate to the full-frame AVX2 converter, but offset the pointers
    // so it only processes the sub-rectangle.
    const uint8_t* regionBgra = bgra + ry * bgraStride + rx * 4;
    uint8_t* regionY = yPlane + ry * yStride + rx;
    uint8_t* regionU = uPlane + (ry / 2) * uStride + (rx / 2);
    uint8_t* regionV = vPlane + (ry / 2) * vStride + (rx / 2);

    bgraToI420(regionBgra, rw, rh, bgraStride,
               regionY, yStride, regionU, uStride, regionV, vStride);
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

float blockChangeRatio(const uint8_t* blockA, const uint8_t* blockB,
                       int stride, int blockWidth, int blockHeight, int tolerance) {
    int changedBytes = 0;
    const __m256i tol = _mm256_set1_epi8(static_cast<char>(tolerance));
    const int bytesPerRow = blockWidth * 4;

    for (int y = 0; y < blockHeight; ++y) {
        const uint8_t* a = blockA + y * stride;
        const uint8_t* b = blockB + y * stride;

        int x = 0;
        for (; x + 31 < bytesPerRow; x += 32) {
            __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + x));
            __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + x));
            __m256i diff = _mm256_or_si256(
                _mm256_subs_epu8(va, vb),
                _mm256_subs_epu8(vb, va)
            );
            __m256i exceed = _mm256_subs_epu8(diff, tol);
            int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(exceed, _mm256_setzero_si256()));
            changedBytes += 32 - __builtin_popcount(mask);
        }

        for (; x < bytesPerRow; ++x) {
            if (std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x])) > tolerance) {
                ++changedBytes;
            }
        }
    }

    // Convert byte-level changes to pixel-level: a pixel is changed if any
    // of its 4 bytes (BGRA) exceeds tolerance. Approximate by dividing byte
    // count by 4 (bytes per pixel) — this slightly over-counts but is fast.
    int totalPixels = blockWidth * blockHeight;
    int changedPixels = changedBytes / 4;
    if (changedPixels > totalPixels) changedPixels = totalPixels;
    return totalPixels > 0 ? static_cast<float>(changedPixels) / static_cast<float>(totalPixels) : 0.0f;
}

} // namespace avx2
} // namespace omnidesk
