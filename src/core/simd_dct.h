#pragma once

#include <cstdint>

namespace omnidesk {

// DCT-II forward and inverse transforms for lossy tile encoding.
// Operates on int16_t data blocks. Supports 4x4, 8x8, and 16x16 block sizes.
//
// The DCT coefficients are scaled by a factor so that computation uses
// only integer arithmetic (fixed-point). Output is int16_t.

// Forward DCT-II on a blockSize x blockSize block.
// src: input block (row-major, stride = srcStride elements)
// dst: output coefficients (row-major, stride = dstStride elements)
void dctForward(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize);

// Inverse DCT-II (reconstruct from coefficients).
void dctInverse(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize);

// Quantize DCT coefficients in-place.
// qp: quantization parameter (higher = more loss, smaller output values)
// blockSize: 4, 8, or 16
void quantize(int16_t* coeffs, int stride, int blockSize, int qp);

// Dequantize DCT coefficients in-place.
void dequantize(int16_t* coeffs, int stride, int blockSize, int qp);

namespace avx2 {
    void dctForward(const int16_t* src, int srcStride,
                    int16_t* dst, int dstStride, int blockSize);
    void dctInverse(const int16_t* src, int srcStride,
                    int16_t* dst, int dstStride, int blockSize);
}

} // namespace omnidesk
