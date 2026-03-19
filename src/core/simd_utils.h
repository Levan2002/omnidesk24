#pragma once

#include "core/types.h"
#include <cstddef>
#include <cstdint>
#include <vector>

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

// Convert only the specified rectangular region from BGRA to I420.
// The rect is automatically aligned to 2x2 chroma boundaries.
// yPlane/uPlane/vPlane must already be allocated at full frame dimensions.
void bgraToI420Region(const uint8_t* bgra, int width, int height, int bgraStride,
                      uint8_t* yPlane, int yStride,
                      uint8_t* uPlane, int uStride,
                      uint8_t* vPlane, int vStride,
                      const Rect& region);

// Convert only dirty regions from BGRA to I420. The destination frame is
// allocated (or reused) at full frame size. Only the dirty rectangles are
// converted — unchanged regions preserve their previous I420 data.
void convertDirtyRegionsToI420(const Frame& src, Frame& dst,
                               const std::vector<Rect>& dirtyRects);

// BGRA to NV12 color space conversion (Y plane + interleaved UV plane).
// Used for hardware encoders (NVENC, VAAPI) that prefer NV12 over I420.
void bgraToNV12(const uint8_t* bgra, int width, int height, int bgraStride,
                uint8_t* yPlane, int yStride,
                uint8_t* uvPlane, int uvStride);

// Convert only dirty regions from BGRA to NV12.
void convertDirtyRegionsToNV12(const Frame& src, Frame& dst,
                               const std::vector<Rect>& dirtyRects);

// Fast block comparison: compare two 16x16 pixel blocks (BGRA)
// Returns true if blocks differ by more than threshold.
bool blocksDiffer(const uint8_t* blockA, const uint8_t* blockB,
                  int stride, int blockSize, int threshold);

// Compute hash of a pixel block (for change detection)
uint64_t blockHash(const uint8_t* block, int stride, int blockSize);

// Compute the fraction of pixels that differ between two blocks (BGRA).
// Returns a value in [0.0, 1.0]. Uses SIMD when available.
// tolerance: per-channel difference threshold to consider a pixel "changed".
float blockChangeRatio(const uint8_t* blockA, const uint8_t* blockB,
                       int stride, int blockWidth, int blockHeight, int tolerance = 4);

// Resize an I420 frame using bilinear interpolation.
// Destination frame is allocated and filled. Widths/heights must be even.
void resizeI420(const Frame& src, Frame& dst, int dstWidth, int dstHeight);

// AVX2 accelerated versions (in simd_utils_avx2.cpp, compiled with -mavx2)
namespace avx2 {
    void bgraToI420(const uint8_t* bgra, int width, int height, int bgraStride,
                    uint8_t* yPlane, int yStride,
                    uint8_t* uPlane, int uStride,
                    uint8_t* vPlane, int vStride);

    void bgraToNV12(const uint8_t* bgra, int width, int height, int bgraStride,
                    uint8_t* yPlane, int yStride,
                    uint8_t* uvPlane, int uvStride);

    void bgraToI420Region(const uint8_t* bgra, int width, int height, int bgraStride,
                          uint8_t* yPlane, int yStride,
                          uint8_t* uPlane, int uStride,
                          uint8_t* vPlane, int vStride,
                          int rx, int ry, int rw, int rh);

    bool blocksDiffer(const uint8_t* blockA, const uint8_t* blockB,
                      int stride, int blockSize, int threshold);

    float blockChangeRatio(const uint8_t* blockA, const uint8_t* blockB,
                           int stride, int blockWidth, int blockHeight, int tolerance);
}

} // namespace omnidesk
