#pragma once

#include "core/types.h"
#include <cstddef>
#include <cstdint>

namespace omnidesk {

// Runtime CPU feature detection
bool cpuSupportsAVX2();

// BGRA to I420 color space conversion
// Converts a BGRA frame to I420 (YCbCr 4:2:0) for H.264 encoding.
// Uses SSE2 baseline with AVX2 fast path when available.
void bgraToI420(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uPlane, int uStride,
                uint8_t* vPlane, int vStride);

// Convert a Frame from BGRA to I420 in-place (allocates new buffer)
void convertFrameToI420(const Frame& src, Frame& dst);

// Fast block comparison: compare two 16x16 pixel blocks (BGRA)
// Returns true if blocks differ by more than threshold.
bool blocksDiffer(const uint8_t* blockA, const uint8_t* blockB,
                  int stride, int blockSize, int threshold);

// Compute hash of a pixel block (for change detection)
uint64_t blockHash(const uint8_t* block, int stride, int blockSize);

// Resize an I420 frame using bilinear interpolation.
// Destination frame is allocated and filled. Widths/heights must be even.
void resizeI420(const Frame& src, Frame& dst, int dstWidth, int dstHeight);

// AVX2 accelerated versions (in simd_utils_avx2.cpp, compiled with -mavx2)
namespace avx2 {
    void bgraToI420(const uint8_t* bgra, int width, int height, int bgraStride,
                    uint8_t* yPlane, int yStride,
                    uint8_t* uPlane, int uStride,
                    uint8_t* vPlane, int vStride);

    bool blocksDiffer(const uint8_t* blockA, const uint8_t* blockB,
                      int stride, int blockSize, int threshold);
}

} // namespace omnidesk
