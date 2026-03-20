#pragma once

#include "codec/omni/omni_types.h"
#include "codec/omni/rans_codec.h"
#include "codec/omni/bitstream.h"
#include <cstdint>
#include <vector>

namespace omnidesk {
namespace omni {

// Per-tile decoding logic with prediction mode support.
// Supports LOSSLESS, NEAR_LOSSLESS, and LOSSY decode.
class TileDecoder {
public:
    TileDecoder();

    void init(int maxTileSize);

    // Decode a lossless tile from the bitstream.
    bool decodeLossless(BitstreamReader& bs, uint8_t* bgra, int bgraStride,
                        int tileW, int tileH,
                        const int16_t* topY, const int16_t* topCo, const int16_t* topCg,
                        const int16_t* leftY, const int16_t* leftCo, const int16_t* leftCg,
                        int16_t topLeftY, int16_t topLeftCo, int16_t topLeftCg);

    // Decode a near-lossless tile (quantized residuals).
    bool decodeNearLossless(BitstreamReader& bs, uint8_t* bgra, int bgraStride,
                            int tileW, int tileH,
                            const int16_t* topY, const int16_t* topCo, const int16_t* topCg,
                            const int16_t* leftY, const int16_t* leftCo, const int16_t* leftCg,
                            int16_t topLeftY, int16_t topLeftCo, int16_t topLeftCg);

    // Decode a lossy tile (DCT-based).
    bool decodeLossy(BitstreamReader& bs, uint8_t* bgra, int bgraStride,
                     int tileW, int tileH);

    // Get the last-decoded YCoCg-R planes (for use as neighbor context).
    const int16_t* lastY() const { return yBuf_.data(); }
    const int16_t* lastCo() const { return coBuf_.data(); }
    const int16_t* lastCg() const { return cgBuf_.data(); }

private:
    // Inverse prediction: reconstruct channel from residuals + prediction mode.
    void inversePrediction(const int16_t* residual, int16_t* out,
                           int w, int h, PredMode mode,
                           const int16_t* top, const int16_t* left, int16_t topLeft);

    // Decode rANS-compressed symbols from bitstream.
    bool decodeResiduals(BitstreamReader& bs, size_t totalSymbols);

    std::vector<int16_t> yBuf_, coBuf_, cgBuf_;
    std::vector<int16_t> residualBuf_;
    std::vector<int16_t> predBuf_;
    std::vector<uint8_t> symbolBuf_;

    RANSDecoder ransDecoder_;
};

} // namespace omni
} // namespace omnidesk
