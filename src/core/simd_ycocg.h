#pragma once

#include <cstdint>

namespace omnidesk {

// YCoCg-R (Reversible Color Transform) - lossless, integer-only.
// Better decorrelation than BT.601 for screen content, fully reversible.
//
// Forward: BGRA -> Y, Co, Cg (in-place on 3 planes + alpha passthrough)
//   Co = R - B
//   t  = B + (Co >> 1)
//   Cg = G - t
//   Y  = t + (Cg >> 1)
//
// Inverse: Y, Co, Cg -> BGRA
//   t  = Y - (Cg >> 1)
//   G  = Cg + t
//   B  = t - (Co >> 1)
//   R  = Co + B

// Convert a tile/block from BGRA (4 bytes/pixel) to YCoCg-R (3 int16 planes).
// yPlane, coPlane, cgPlane: output planes (int16_t), must hold width*height elements each.
// bgra: input BGRA pixels, bgraStride in bytes.
// width, height: dimensions (must be even for optimal SIMD).
void bgraToYCoCgR(const uint8_t* bgra, int width, int height, int bgraStride,
                   int16_t* yPlane, int16_t* coPlane, int16_t* cgPlane);

// Convert from YCoCg-R (3 int16 planes) back to BGRA (4 bytes/pixel).
// Alpha channel is set to 255.
void yCoCgRToBgra(const int16_t* yPlane, const int16_t* coPlane, const int16_t* cgPlane,
                   int width, int height,
                   uint8_t* bgra, int bgraStride);

// AVX2 accelerated versions
namespace avx2 {
    void bgraToYCoCgR(const uint8_t* bgra, int width, int height, int bgraStride,
                       int16_t* yPlane, int16_t* coPlane, int16_t* cgPlane);

    void yCoCgRToBgra(const int16_t* yPlane, const int16_t* coPlane, const int16_t* cgPlane,
                       int width, int height,
                       uint8_t* bgra, int bgraStride);
}

} // namespace omnidesk
