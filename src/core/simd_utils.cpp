#include "core/simd_utils.h"
#include "core/logger.h"

#ifdef OMNIDESK_X86_64
#include <immintrin.h>
#include <cpuid.h>
#endif

#include <cstring>

namespace omnidesk {

bool cpuSupportsAVX2() {
#ifdef OMNIDESK_X86_64
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx & (1 << 5)) != 0; // AVX2 bit
    }
#endif
    return false;
}

// SSE2 BGRA->I420 conversion — REAL SIMD implementation
// Processes 4 BGRA pixels (16 bytes) per SSE2 iteration for Y plane,
// and 2x2 pixel blocks for chroma with SSE2 averaging.
// BT.601 full range:
//   Y  = ( 77*R + 150*G +  29*B + 128) >> 8
//   Cb = (-43*R -  85*G + 128*B + 128) >> 8  + 128
//   Cr = (128*R - 107*G -  21*B + 128) >> 8  + 128
#ifdef OMNIDESK_X86_64
static void bgraToI420_sse2(const uint8_t* bgra, int width, int height, int bgraStride,
                            uint8_t* yPlane, int yStride,
                            uint8_t* uPlane, int uStride,
                            uint8_t* vPlane, int vStride) {
    // Fixed-point coefficients for Y = (77*R + 150*G + 29*B + 128) >> 8
    const __m128i coeff_r_y = _mm_set1_epi16(77);
    const __m128i coeff_g_y = _mm_set1_epi16(150);
    const __m128i coeff_b_y = _mm_set1_epi16(29);

    // Chroma coefficients:
    // U = (-43*R - 85*G + 128*B + 128) >> 8 + 128
    // V = (128*R - 107*G - 21*B + 128) >> 8 + 128
    const __m128i coeff_r_u = _mm_set1_epi16(-43);
    const __m128i coeff_g_u = _mm_set1_epi16(-85);
    const __m128i coeff_b_u = _mm_set1_epi16(128);
    const __m128i coeff_r_v = _mm_set1_epi16(128);
    const __m128i coeff_g_v = _mm_set1_epi16(-107);
    const __m128i coeff_b_v = _mm_set1_epi16(-21);

    const __m128i round_val = _mm_set1_epi16(128);
    const __m128i offset_128 = _mm_set1_epi16(128);
    const __m128i mask_00ff = _mm_set1_epi32(0x000000FF);
    const __m128i zero = _mm_setzero_si128();

    for (int y = 0; y < height; y += 2) {
        const uint8_t* row0 = bgra + y * bgraStride;
        const uint8_t* row1 = (y + 1 < height) ? bgra + (y + 1) * bgraStride : row0;
        uint8_t* yRow0 = yPlane + y * yStride;
        uint8_t* yRow1 = yPlane + (y + 1) * yStride;
        uint8_t* uRow  = uPlane + (y / 2) * uStride;
        uint8_t* vRow  = vPlane + (y / 2) * vStride;

        int x = 0;
        // SSE2 path: process 4 pixels at a time
        // Each iteration loads 4 BGRA pixels (16 bytes) = 1 __m128i
        for (; x + 3 < width; x += 4) {
            // --- Row 0: compute Y for 4 pixels ---
            __m128i pix0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row0 + x * 4));

            // Extract B, G, R from BGRA by shifting and masking
            __m128i b0_32 = _mm_and_si128(pix0, mask_00ff);                         // B
            __m128i g0_32 = _mm_and_si128(_mm_srli_epi32(pix0, 8), mask_00ff);      // G
            __m128i r0_32 = _mm_and_si128(_mm_srli_epi32(pix0, 16), mask_00ff);     // R

            // Pack 32-bit values down to 16-bit (we know values are 0-255)
            __m128i b0_16 = _mm_packs_epi32(b0_32, zero);  // 4 values in low 64 bits
            __m128i g0_16 = _mm_packs_epi32(g0_32, zero);
            __m128i r0_16 = _mm_packs_epi32(r0_32, zero);

            // Y = (77*R + 150*G + 29*B + 128) >> 8
            __m128i y0_16 = _mm_add_epi16(
                _mm_add_epi16(
                    _mm_mullo_epi16(r0_16, coeff_r_y),
                    _mm_mullo_epi16(g0_16, coeff_g_y)
                ),
                _mm_add_epi16(
                    _mm_mullo_epi16(b0_16, coeff_b_y),
                    round_val
                )
            );
            y0_16 = _mm_srli_epi16(y0_16, 8);
            __m128i y0_8 = _mm_packus_epi16(y0_16, zero);

            // Store 4 Y values for row 0
            *reinterpret_cast<uint32_t*>(yRow0 + x) =
                static_cast<uint32_t>(_mm_cvtsi128_si32(y0_8));

            // --- Row 1: compute Y for 4 pixels ---
            __m128i pix1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row1 + x * 4));

            __m128i b1_32 = _mm_and_si128(pix1, mask_00ff);
            __m128i g1_32 = _mm_and_si128(_mm_srli_epi32(pix1, 8), mask_00ff);
            __m128i r1_32 = _mm_and_si128(_mm_srli_epi32(pix1, 16), mask_00ff);

            __m128i b1_16 = _mm_packs_epi32(b1_32, zero);
            __m128i g1_16 = _mm_packs_epi32(g1_32, zero);
            __m128i r1_16 = _mm_packs_epi32(r1_32, zero);

            __m128i y1_16 = _mm_add_epi16(
                _mm_add_epi16(
                    _mm_mullo_epi16(r1_16, coeff_r_y),
                    _mm_mullo_epi16(g1_16, coeff_g_y)
                ),
                _mm_add_epi16(
                    _mm_mullo_epi16(b1_16, coeff_b_y),
                    round_val
                )
            );
            y1_16 = _mm_srli_epi16(y1_16, 8);
            __m128i y1_8 = _mm_packus_epi16(y1_16, zero);

            if (y + 1 < height) {
                *reinterpret_cast<uint32_t*>(yRow1 + x) =
                    static_cast<uint32_t>(_mm_cvtsi128_si32(y1_8));
            }

            // --- Chroma: average 2x2 blocks, produce 2 U and 2 V values ---
            // We have 4 pixels per row = 2 chroma pairs (x, x+1) and (x+2, x+3)
            // Average the 4 components of each 2x2 block across both rows.

            // Sum row0 and row1 channel values (still 16-bit, values 0-510)
            __m128i b_sum = _mm_add_epi16(b0_16, b1_16);
            __m128i g_sum = _mm_add_epi16(g0_16, g1_16);
            __m128i r_sum = _mm_add_epi16(r0_16, r1_16);

            // Horizontal pair-add: add adjacent pixels to get 2x2 block sums
            // b_sum has [b0, b1, b2, b3, 0, 0, 0, 0]
            // We want [(b0+b1), (b2+b3)] then divide by 4
            // Use hadd: _mm_hadd_epi16 is SSE3, so use manual shuffle for SSE2
            __m128i b_even = b_sum;                                    // [b0, b1, b2, b3, ...]
            __m128i b_odd  = _mm_srli_si128(b_sum, 2);                // [b1, b2, b3, 0, ...]
            __m128i b_pair = _mm_add_epi16(b_even, b_odd);            // [b0+b1, ...]
            // Extract pairs at positions 0 and 2
            int bAvg0 = (_mm_extract_epi16(b_pair, 0) + 2) >> 2; // /4 with rounding
            int bAvg1 = (_mm_extract_epi16(b_pair, 2) + 2) >> 2;

            __m128i g_odd  = _mm_srli_si128(g_sum, 2);
            __m128i g_pair = _mm_add_epi16(g_sum, g_odd);
            int gAvg0 = (_mm_extract_epi16(g_pair, 0) + 2) >> 2;
            int gAvg1 = (_mm_extract_epi16(g_pair, 2) + 2) >> 2;

            __m128i r_odd  = _mm_srli_si128(r_sum, 2);
            __m128i r_pair = _mm_add_epi16(r_sum, r_odd);
            int rAvg0 = (_mm_extract_epi16(r_pair, 0) + 2) >> 2;
            int rAvg1 = (_mm_extract_epi16(r_pair, 2) + 2) >> 2;

            // Compute U and V for each 2x2 block
            // U = (-43*R - 85*G + 128*B + 128) >> 8 + 128
            // V = (128*R - 107*G - 21*B + 128) >> 8 + 128
            __m128i rAvg_16 = _mm_set_epi16(0, 0, 0, 0, 0, 0, rAvg1, rAvg0);
            __m128i gAvg_16 = _mm_set_epi16(0, 0, 0, 0, 0, 0, gAvg1, gAvg0);
            __m128i bAvg_16 = _mm_set_epi16(0, 0, 0, 0, 0, 0, bAvg1, bAvg0);

            __m128i u_16 = _mm_add_epi16(
                _mm_add_epi16(
                    _mm_mullo_epi16(rAvg_16, coeff_r_u),
                    _mm_mullo_epi16(gAvg_16, coeff_g_u)
                ),
                _mm_add_epi16(
                    _mm_mullo_epi16(bAvg_16, coeff_b_u),
                    round_val
                )
            );
            u_16 = _mm_srai_epi16(u_16, 8);  // arithmetic shift (signed values)
            u_16 = _mm_add_epi16(u_16, offset_128);

            __m128i v_16 = _mm_add_epi16(
                _mm_add_epi16(
                    _mm_mullo_epi16(rAvg_16, coeff_r_v),
                    _mm_mullo_epi16(gAvg_16, coeff_g_v)
                ),
                _mm_add_epi16(
                    _mm_mullo_epi16(bAvg_16, coeff_b_v),
                    round_val
                )
            );
            v_16 = _mm_srai_epi16(v_16, 8);
            v_16 = _mm_add_epi16(v_16, offset_128);

            // Clamp to [0, 255] and store
            __m128i u_8 = _mm_packus_epi16(u_16, zero);
            __m128i v_8 = _mm_packus_epi16(v_16, zero);

            uRow[x / 2 + 0] = static_cast<uint8_t>(_mm_extract_epi16(u_8, 0) & 0xFF);
            uRow[x / 2 + 1] = static_cast<uint8_t>((_mm_extract_epi16(u_8, 0) >> 8) & 0xFF);
            vRow[x / 2 + 0] = static_cast<uint8_t>(_mm_extract_epi16(v_8, 0) & 0xFF);
            vRow[x / 2 + 1] = static_cast<uint8_t>((_mm_extract_epi16(v_8, 0) >> 8) & 0xFF);
        }

        // Scalar remainder for non-4-pixel-aligned width
        for (; x < width; ++x) {
            int b0s = row0[x * 4 + 0], g0s = row0[x * 4 + 1], r0s = row0[x * 4 + 2];
            int yVal = (77 * r0s + 150 * g0s + 29 * b0s + 128) >> 8;
            yRow0[x] = static_cast<uint8_t>(std::min(std::max(yVal, 0), 255));

            if (y + 1 < height) {
                int b1s = row1[x * 4 + 0], g1s = row1[x * 4 + 1], r1s = row1[x * 4 + 2];
                int yVal1 = (77 * r1s + 150 * g1s + 29 * b1s + 128) >> 8;
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
#else
// Non-x86 scalar fallback
static void bgraToI420_sse2(const uint8_t* bgra, int width, int height, int bgraStride,
                            uint8_t* yPlane, int yStride,
                            uint8_t* uPlane, int uStride,
                            uint8_t* vPlane, int vStride) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        uint8_t* yRow = yPlane + y * yStride;

        for (int x = 0; x < width; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];

            int yVal = (77 * r + 150 * g + 29 * b + 128) >> 8;
            yRow[x] = static_cast<uint8_t>(std::min(std::max(yVal, 0), 255));
        }

        if (y % 2 == 0 && y + 1 < height) {
            const uint8_t* row2 = bgra + (y + 1) * bgraStride;
            uint8_t* uRow = uPlane + (y / 2) * uStride;
            uint8_t* vRow = vPlane + (y / 2) * vStride;

            for (int x = 0; x < width - 1; x += 2) {
                int b = (row[x*4+0] + row[(x+1)*4+0] + row2[x*4+0] + row2[(x+1)*4+0]) / 4;
                int g = (row[x*4+1] + row[(x+1)*4+1] + row2[x*4+1] + row2[(x+1)*4+1]) / 4;
                int r = (row[x*4+2] + row[(x+1)*4+2] + row2[x*4+2] + row2[(x+1)*4+2]) / 4;

                int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                uRow[x / 2] = static_cast<uint8_t>(std::min(std::max(u, 0), 255));
                vRow[x / 2] = static_cast<uint8_t>(std::min(std::max(v, 0), 255));
            }
        }
    }
}
#endif

void bgraToI420(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uPlane, int uStride,
                uint8_t* vPlane, int vStride) {
    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        avx2::bgraToI420(bgra, width, height, bgraStride,
                         yPlane, yStride, uPlane, uStride, vPlane, vStride);
        return;
    }
    bgraToI420_sse2(bgra, width, height, bgraStride,
                    yPlane, yStride, uPlane, uStride, vPlane, vStride);
}

// ---- BGRA → NV12 conversion ----

static void bgraToNV12_scalar(const uint8_t* bgra, int width, int height, int bgraStride,
                               uint8_t* yPlane, int yStride,
                               uint8_t* uvPlane, int uvStride) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        uint8_t* yRow = yPlane + y * yStride;

        for (int x = 0; x < width; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];
            int yVal = (77 * r + 150 * g + 29 * b + 128) >> 8;
            yRow[x] = static_cast<uint8_t>(std::min(std::max(yVal, 0), 255));
        }

        if (y % 2 == 0 && y + 1 < height) {
            const uint8_t* row2 = bgra + (y + 1) * bgraStride;
            uint8_t* uvRow = uvPlane + (y / 2) * uvStride;

            for (int x = 0; x < width - 1; x += 2) {
                int b = (row[x*4+0] + row[(x+1)*4+0] + row2[x*4+0] + row2[(x+1)*4+0]) / 4;
                int g = (row[x*4+1] + row[(x+1)*4+1] + row2[x*4+1] + row2[(x+1)*4+1]) / 4;
                int r = (row[x*4+2] + row[(x+1)*4+2] + row2[x*4+2] + row2[(x+1)*4+2]) / 4;

                int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                uvRow[x + 0] = static_cast<uint8_t>(std::min(std::max(u, 0), 255));
                uvRow[x + 1] = static_cast<uint8_t>(std::min(std::max(v, 0), 255));
            }
        }
    }
}

void bgraToNV12(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uvPlane, int uvStride) {
    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        avx2::bgraToNV12(bgra, width, height, bgraStride,
                         yPlane, yStride, uvPlane, uvStride);
        return;
    }
    bgraToNV12_scalar(bgra, width, height, bgraStride,
                      yPlane, yStride, uvPlane, uvStride);
}

void convertDirtyRegionsToNV12(const Frame& src, Frame& dst,
                               const std::vector<Rect>& dirtyRects) {
    if (src.format != PixelFormat::BGRA && src.format != PixelFormat::RGBA) {
        LOG_ERROR("convertDirtyRegionsToNV12: unsupported source format");
        return;
    }

    bool freshAlloc = dst.data.empty() ||
                      dst.width != src.width || dst.height != src.height;
    dst.allocate(src.width, src.height, PixelFormat::NV12);
    dst.timestampUs = src.timestampUs;
    dst.frameId = src.frameId;

    if (freshAlloc) {
        bgraToNV12(src.data.data(), src.width, src.height, src.stride,
                   dst.plane(0), dst.stride,
                   dst.plane(1), dst.stride);
        return;
    }

    // Convert only dirty regions
    for (const auto& rect : dirtyRects) {
        int rx = rect.x & ~1;
        int ry = rect.y & ~1;
        int rr = std::min((rect.right() + 1) & ~1, static_cast<int>(src.width));
        int rb = std::min((rect.bottom() + 1) & ~1, static_cast<int>(src.height));
        int rw = rr - rx;
        int rh = rb - ry;
        if (rw <= 0 || rh <= 0) continue;

        const uint8_t* regionBgra = src.data.data() + ry * src.stride + rx * 4;
        uint8_t* regionY = dst.plane(0) + ry * dst.stride + rx;
        uint8_t* regionUV = dst.plane(1) + (ry / 2) * dst.stride + rx;

        bgraToNV12_scalar(regionBgra, rw, rh, src.stride,
                          regionY, dst.stride, regionUV, dst.stride);
    }
}

void bgraToI420Region(const uint8_t* bgra, int width, int height, int bgraStride,
                      uint8_t* yPlane, int yStride,
                      uint8_t* uPlane, int uStride,
                      uint8_t* vPlane, int vStride,
                      const Rect& region) {
    // Align region to 2x2 chroma boundaries
    int rx = region.x & ~1;
    int ry = region.y & ~1;
    int rr = std::min((region.right() + 1) & ~1, width);
    int rb = std::min((region.bottom() + 1) & ~1, height);
    int rw = rr - rx;
    int rh = rb - ry;

    if (rw <= 0 || rh <= 0) return;

    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        avx2::bgraToI420Region(bgra, width, height, bgraStride,
                               yPlane, yStride, uPlane, uStride, vPlane, vStride,
                               rx, ry, rw, rh);
        return;
    }

    // SSE2/scalar path: convert only the dirty region
    const uint8_t* regionBgra = bgra + ry * bgraStride + rx * 4;
    uint8_t* regionY = yPlane + ry * yStride + rx;
    uint8_t* regionU = uPlane + (ry / 2) * uStride + (rx / 2);
    uint8_t* regionV = vPlane + (ry / 2) * vStride + (rx / 2);

    bgraToI420_sse2(regionBgra, rw, rh, bgraStride,
                    regionY, yStride, regionU, uStride, regionV, vStride);
}

void convertDirtyRegionsToI420(const Frame& src, Frame& dst,
                               const std::vector<Rect>& dirtyRects) {
    if (src.format != PixelFormat::BGRA && src.format != PixelFormat::RGBA) {
        LOG_ERROR("convertDirtyRegionsToI420: unsupported source format");
        return;
    }

    // Allocate/reuse the destination buffer at full frame size.
    // Frame::allocate only resizes if dimensions changed, so this is cheap.
    bool freshAlloc = dst.data.empty() ||
                      dst.width != src.width || dst.height != src.height;
    dst.allocate(src.width, src.height, PixelFormat::I420);
    dst.timestampUs = src.timestampUs;
    dst.frameId = src.frameId;

    if (freshAlloc) {
        // First frame or resolution change: convert the full frame
        bgraToI420(src.data.data(), src.width, src.height, src.stride,
                   dst.plane(0), dst.stride,
                   dst.plane(1), dst.stride / 2,
                   dst.plane(2), dst.stride / 2);
        return;
    }

    // Convert only the dirty regions
    for (const auto& rect : dirtyRects) {
        bgraToI420Region(src.data.data(), src.width, src.height, src.stride,
                         dst.plane(0), dst.stride,
                         dst.plane(1), dst.stride / 2,
                         dst.plane(2), dst.stride / 2,
                         rect);
    }
}

void convertFrameToI420(const Frame& src, Frame& dst) {
    if (src.format == PixelFormat::I420) {
        dst = src; // Already I420
        return;
    }
    if (src.format != PixelFormat::BGRA && src.format != PixelFormat::RGBA) {
        LOG_ERROR("convertFrameToI420: unsupported source format");
        return;
    }
    dst.allocate(src.width, src.height, PixelFormat::I420);
    dst.timestampUs = src.timestampUs;
    dst.frameId = src.frameId;

    bgraToI420(src.data.data(), src.width, src.height, src.stride,
               dst.plane(0), dst.stride,
               dst.plane(1), dst.stride / 2,
               dst.plane(2), dst.stride / 2);
}

bool blocksDiffer(const uint8_t* blockA, const uint8_t* blockB,
                  int stride, int blockSize, int threshold) {
#ifdef OMNIDESK_X86_64
    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        return avx2::blocksDiffer(blockA, blockB, stride, blockSize, threshold);
    }
#endif
    // Scalar fallback
    int diffCount = 0;
    for (int y = 0; y < blockSize; ++y) {
        const uint8_t* a = blockA + y * stride;
        const uint8_t* b = blockB + y * stride;
        for (int x = 0; x < blockSize * 4; ++x) { // BGRA = 4 bytes per pixel
            if (std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x])) > 2) {
                ++diffCount;
                if (diffCount > threshold) return true;
            }
        }
    }
    return diffCount > threshold;
}

float blockChangeRatio(const uint8_t* blockA, const uint8_t* blockB,
                       int stride, int blockWidth, int blockHeight, int tolerance) {
#ifdef OMNIDESK_X86_64
    static bool hasAVX2 = cpuSupportsAVX2();
    if (hasAVX2) {
        return avx2::blockChangeRatio(blockA, blockB, stride, blockWidth, blockHeight, tolerance);
    }
#endif
    // Scalar fallback: count pixels where any RGB channel differs by > tolerance
    int changedPixels = 0;
    int totalPixels = blockWidth * blockHeight;
    for (int y = 0; y < blockHeight; ++y) {
        const uint8_t* a = blockA + y * stride;
        const uint8_t* b = blockB + y * stride;
        for (int x = 0; x < blockWidth; ++x) {
            int off = x * 4;
            bool differs = false;
            for (int c = 0; c < 3; ++c) {
                if (std::abs(static_cast<int>(a[off + c]) - static_cast<int>(b[off + c])) > tolerance) {
                    differs = true;
                    break;
                }
            }
            if (differs) ++changedPixels;
        }
    }
    return totalPixels > 0 ? static_cast<float>(changedPixels) / static_cast<float>(totalPixels) : 0.0f;
}

uint64_t blockHash(const uint8_t* block, int stride, int blockSize) {
    // FNV-1a hash over pixel data
    uint64_t hash = 14695981039346656037ULL;
    for (int y = 0; y < blockSize; ++y) {
        const uint8_t* row = block + y * stride;
        for (int x = 0; x < blockSize * 4; ++x) {
            hash ^= static_cast<uint64_t>(row[x]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

// ---- I420 bilinear resize ----

// Scalar fallback: resize a single plane using bilinear interpolation.
static void resizePlane_scalar(const uint8_t* src, int srcW, int srcH, int srcStride,
                               uint8_t* dst, int dstW, int dstH, int dstStride) {
    const uint32_t xRatio = ((static_cast<uint32_t>(srcW) << 16) / dstW);
    const uint32_t yRatio = ((static_cast<uint32_t>(srcH) << 16) / dstH);

    for (int y = 0; y < dstH; ++y) {
        uint32_t srcY = y * yRatio;
        int sy = static_cast<int>(srcY >> 16);
        int yFrac = static_cast<int>(srcY & 0xFFFF);
        int sy1 = std::min(sy + 1, srcH - 1);

        const uint8_t* row0 = src + sy * srcStride;
        const uint8_t* row1 = src + sy1 * srcStride;
        uint8_t* dstRow = dst + y * dstStride;

        for (int x = 0; x < dstW; ++x) {
            uint32_t srcX = x * xRatio;
            int sx = static_cast<int>(srcX >> 16);
            int xFrac = static_cast<int>(srcX & 0xFFFF);
            int sx1 = std::min(sx + 1, srcW - 1);

            int a = row0[sx], b = row0[sx1];
            int c = row1[sx], d = row1[sx1];

            int top = a + (((b - a) * xFrac + 0x8000) >> 16);
            int bot = c + (((d - c) * xFrac + 0x8000) >> 16);
            int val = top + (((bot - top) * yFrac + 0x8000) >> 16);

            dstRow[x] = static_cast<uint8_t>(std::min(std::max(val, 0), 255));
        }
    }
}

#ifdef OMNIDESK_X86_64
// SSE2 compat: _mm_mullo_epi32 is SSE4.1, emulate for SSE2
static inline __m128i _mm_mullo_epi32_compat(__m128i a, __m128i b) {
    __m128i tmp1 = _mm_mul_epu32(a, b);
    __m128i tmp2 = _mm_mul_epu32(_mm_srli_si128(a, 4), _mm_srli_si128(b, 4));
    return _mm_unpacklo_epi32(
        _mm_shuffle_epi32(tmp1, _MM_SHUFFLE(0, 0, 2, 0)),
        _mm_shuffle_epi32(tmp2, _MM_SHUFFLE(0, 0, 2, 0))
    );
}

// SSE2-accelerated bilinear resize: processes 4 destination pixels per iteration.
// Uses 16-bit fixed-point (8.8) for the interpolation fractions so that
// multiply + shift fits in 16-bit SSE2 arithmetic.
static void resizePlane_sse2(const uint8_t* src, int srcW, int srcH, int srcStride,
                              uint8_t* dst, int dstW, int dstH, int dstStride) {
    const uint32_t xRatio = ((static_cast<uint32_t>(srcW) << 16) / dstW);
    const uint32_t yRatio = ((static_cast<uint32_t>(srcH) << 16) / dstH);
    const __m128i zero = _mm_setzero_si128();

    for (int y = 0; y < dstH; ++y) {
        uint32_t srcY = y * yRatio;
        int sy = static_cast<int>(srcY >> 16);
        // Downscale yFrac from 16.16 to 8.8 for 16-bit arithmetic
        int yFrac8 = static_cast<int>((srcY & 0xFFFF) >> 8);
        int sy1 = std::min(sy + 1, srcH - 1);

        const uint8_t* row0 = src + sy * srcStride;
        const uint8_t* row1 = src + sy1 * srcStride;
        uint8_t* dstRow = dst + y * dstStride;

        __m128i yf = _mm_set1_epi16(static_cast<int16_t>(yFrac8));
        __m128i inv_yf = _mm_set1_epi16(static_cast<int16_t>(256 - yFrac8));

        int x = 0;
        for (; x + 3 < dstW; x += 4) {
            // Compute source X positions for 4 destination pixels
            alignas(16) int16_t xfArr[4];
            alignas(16) uint8_t aArr[4], bArr[4], cArr[4], dArr[4];

            for (int i = 0; i < 4; ++i) {
                uint32_t srcX = (x + i) * xRatio;
                int sx = static_cast<int>(srcX >> 16);
                int sx1 = std::min(sx + 1, srcW - 1);
                xfArr[i] = static_cast<int16_t>((srcX & 0xFFFF) >> 8);
                aArr[i] = row0[sx];
                bArr[i] = row0[sx1];
                cArr[i] = row1[sx];
                dArr[i] = row1[sx1];
            }

            // Load into SSE registers as 16-bit
            __m128i a16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*reinterpret_cast<int32_t*>(aArr)), zero);
            __m128i b16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*reinterpret_cast<int32_t*>(bArr)), zero);
            __m128i c16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*reinterpret_cast<int32_t*>(cArr)), zero);
            __m128i d16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*reinterpret_cast<int32_t*>(dArr)), zero);

            __m128i xf = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(xfArr));
            __m128i inv_xf = _mm_sub_epi16(_mm_set1_epi16(256), xf);

            // Horizontal interpolation: top = a*(256-xf) + b*xf
            __m128i top = _mm_add_epi16(
                _mm_mullo_epi16(a16, inv_xf),
                _mm_mullo_epi16(b16, xf)
            );
            // bot = c*(256-xf) + d*xf
            __m128i bot = _mm_add_epi16(
                _mm_mullo_epi16(c16, inv_xf),
                _mm_mullo_epi16(d16, xf)
            );

            // Vertical interpolation: val = top*(256-yf) + bot*yf, then >> 16
            // top and bot are in 8.8 format, multiply by yf (8-bit) gives 8.16
            // Use 32-bit arithmetic to avoid overflow
            __m128i top_lo = _mm_unpacklo_epi16(top, zero);
            __m128i bot_lo = _mm_unpacklo_epi16(bot, zero);
            __m128i inv_yf32 = _mm_unpacklo_epi16(inv_yf, zero);
            __m128i yf32 = _mm_unpacklo_epi16(yf, zero);

            __m128i val32 = _mm_add_epi32(
                _mm_mullo_epi32_compat(top_lo, inv_yf32),
                _mm_mullo_epi32_compat(bot_lo, yf32)
            );
            // Result is in 8.16 format, shift right by 16
            val32 = _mm_srli_epi32(val32, 16);

            // Pack 32→16→8 and store 4 bytes
            __m128i val16 = _mm_packs_epi32(val32, zero);
            __m128i val8 = _mm_packus_epi16(val16, zero);

            *reinterpret_cast<uint32_t*>(dstRow + x) =
                static_cast<uint32_t>(_mm_cvtsi128_si32(val8));
        }

        // Scalar remainder
        for (; x < dstW; ++x) {
            uint32_t srcX = x * xRatio;
            int sx = static_cast<int>(srcX >> 16);
            int xFrac = static_cast<int>(srcX & 0xFFFF);
            int sx1 = std::min(sx + 1, srcW - 1);

            int a = row0[sx], b = row0[sx1];
            int c = row1[sx], d = row1[sx1];

            int top = a + (((b - a) * xFrac + 0x8000) >> 16);
            int bot = c + (((d - c) * xFrac + 0x8000) >> 16);
            int val = top + (((bot - top) * yFrac8 * 256 + 0x8000) >> 16);

            dstRow[x] = static_cast<uint8_t>(std::min(std::max(val, 0), 255));
        }
    }
}

#endif

static void resizePlane(const uint8_t* src, int srcW, int srcH, int srcStride,
                        uint8_t* dst, int dstW, int dstH, int dstStride) {
#ifdef OMNIDESK_X86_64
    resizePlane_sse2(src, srcW, srcH, srcStride, dst, dstW, dstH, dstStride);
#else
    resizePlane_scalar(src, srcW, srcH, srcStride, dst, dstW, dstH, dstStride);
#endif
}

void resizeI420(const Frame& src, Frame& dst, int dstWidth, int dstHeight) {
    if (src.format != PixelFormat::I420) {
        LOG_ERROR("resizeI420: source frame must be I420");
        return;
    }

    // Ensure even dimensions (required for I420 chroma subsampling)
    dstWidth &= ~1;
    dstHeight &= ~1;

    dst.allocate(dstWidth, dstHeight, PixelFormat::I420);
    dst.timestampUs = src.timestampUs;
    dst.frameId = src.frameId;

    int srcW = src.width;
    int srcH = src.height;

    // Resize Y plane
    resizePlane(src.plane(0), srcW, srcH, src.stride,
                dst.plane(0), dstWidth, dstHeight, dst.stride);

    // Resize U plane (half resolution)
    resizePlane(src.plane(1), srcW / 2, srcH / 2, src.stride / 2,
                dst.plane(1), dstWidth / 2, dstHeight / 2, dst.stride / 2);

    // Resize V plane (half resolution)
    resizePlane(src.plane(2), srcW / 2, srcH / 2, src.stride / 2,
                dst.plane(2), dstWidth / 2, dstHeight / 2, dst.stride / 2);
}

} // namespace omnidesk
