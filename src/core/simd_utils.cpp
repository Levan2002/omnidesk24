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

// SSE2 BGRA→I420 conversion
// Y  = 0.299*R + 0.587*G + 0.114*B
// Cb = -0.169*R - 0.331*G + 0.500*B + 128
// Cr = 0.500*R - 0.419*G - 0.081*B + 128
// Using fixed-point: multiply by 256, shift right 8
static void bgraToI420_sse2(const uint8_t* bgra, int width, int height, int bgraStride,
                            uint8_t* yPlane, int yStride,
                            uint8_t* uPlane, int uStride,
                            uint8_t* vPlane, int vStride) {
    // BT.601 full range conversion
    // Y  =  0.299*R + 0.587*G + 0.114*B
    // Cb = -0.169*R - 0.331*G + 0.500*B + 128
    // Cr =  0.500*R - 0.419*G - 0.081*B + 128
    // Fixed point *256: Y = (77*R + 150*G + 29*B + 128) >> 8
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

        // Chroma subsampling: one U,V per 2x2 block
        if (y % 2 == 0 && y + 1 < height) {
            const uint8_t* row2 = bgra + (y + 1) * bgraStride;
            uint8_t* uRow = uPlane + (y / 2) * uStride;
            uint8_t* vRow = vPlane + (y / 2) * vStride;

            for (int x = 0; x < width - 1; x += 2) {
                // Average 2x2 block
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

// Resize a single plane using bilinear interpolation.
static void resizePlane(const uint8_t* src, int srcW, int srcH, int srcStride,
                        uint8_t* dst, int dstW, int dstH, int dstStride) {
    // Fixed-point scale factors (16.16 format)
    const uint32_t xRatio = ((static_cast<uint32_t>(srcW) << 16) / dstW);
    const uint32_t yRatio = ((static_cast<uint32_t>(srcH) << 16) / dstH);

    for (int y = 0; y < dstH; ++y) {
        uint32_t srcY = y * yRatio;
        int sy = static_cast<int>(srcY >> 16);
        int yFrac = static_cast<int>(srcY & 0xFFFF);

        // Clamp source row to valid range
        int sy1 = std::min(sy + 1, srcH - 1);

        const uint8_t* row0 = src + sy * srcStride;
        const uint8_t* row1 = src + sy1 * srcStride;
        uint8_t* dstRow = dst + y * dstStride;

        for (int x = 0; x < dstW; ++x) {
            uint32_t srcX = x * xRatio;
            int sx = static_cast<int>(srcX >> 16);
            int xFrac = static_cast<int>(srcX & 0xFFFF);
            int sx1 = std::min(sx + 1, srcW - 1);

            // Bilinear interpolation of 4 source pixels
            int a = row0[sx];
            int b = row0[sx1];
            int c = row1[sx];
            int d = row1[sx1];

            // Interpolate horizontally, then vertically
            int top = a + (((b - a) * xFrac + 0x8000) >> 16);
            int bot = c + (((d - c) * xFrac + 0x8000) >> 16);
            int val = top + (((bot - top) * yFrac + 0x8000) >> 16);

            dstRow[x] = static_cast<uint8_t>(std::min(std::max(val, 0), 255));
        }
    }
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
