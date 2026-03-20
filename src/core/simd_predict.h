#pragma once

#include <cstdint>

namespace omnidesk {

// Intra prediction modes for OmniCodec tile encoding.
// Each mode predicts pixel values from neighboring pixels,
// and the encoder stores only the residual (prediction error).
//
// Prediction operates on int16_t planes (one YCoCg-R channel at a time).

// DC prediction: fill tile with average of top and left border pixels.
// top: pointer to the row immediately above the tile (width elements), or nullptr.
// left: pointer to the column immediately left of the tile (height elements), or nullptr.
// out: predicted tile values (width * height).
void predictDC(const int16_t* top, const int16_t* left,
               int width, int height, int16_t* out);

// Horizontal prediction: each row is filled with the left border value.
void predictH(const int16_t* left, int width, int height, int16_t* out);

// Vertical prediction: each column is filled with the top border value.
void predictV(const int16_t* top, int width, int height, int16_t* out);

// Planar prediction: bilinear interpolation between top, left, and top-left corner.
void predictPlanar(const int16_t* top, const int16_t* left, int16_t topLeft,
                   int width, int height, int16_t* out);

// Left-pixel prediction (simple sequential, used in lossless mode).
// Computes residuals in-place: residual[i] = src[i] - predicted[i]
// where predicted is left neighbor (or top, or 0).
void predictLeftPixel(const int16_t* src, int16_t* residual, int width, int height);

// Inverse left-pixel prediction: reconstruct from residuals.
void inversePredictLeftPixel(const int16_t* residual, int16_t* out, int width, int height);

// Compute SAD (Sum of Absolute Differences) between two int16_t buffers.
uint64_t computeSAD(const int16_t* a, const int16_t* b, int count);

namespace avx2 {
    void predictDC(const int16_t* top, const int16_t* left,
                   int width, int height, int16_t* out);
    void predictH(const int16_t* left, int width, int height, int16_t* out);
    void predictV(const int16_t* top, int width, int height, int16_t* out);
    uint64_t computeSAD(const int16_t* a, const int16_t* b, int count);
}

} // namespace omnidesk
