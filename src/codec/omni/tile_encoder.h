#pragma once

#include "codec/omni/omni_types.h"
#include "codec/omni/rans_codec.h"
#include "codec/omni/bitstream.h"
#include <cstdint>
#include <vector>

namespace omnidesk {
namespace omni {

// Per-tile encoding logic with prediction mode selection.
// Supports LOSSLESS, NEAR_LOSSLESS (bounded error), and LOSSY (DCT) modes.
class TileEncoder {
public:
    TileEncoder();

    // Initialize for a given maximum tile size.
    void init(int maxTileSize);

    // Encode a lossless tile. Selects the best prediction mode and writes
    // the compressed data to the bitstream.
    void encodeLossless(const uint8_t* bgra, int bgraStride,
                        int tileW, int tileH,
                        const int16_t* topY, const int16_t* topCo, const int16_t* topCg,
                        const int16_t* leftY, const int16_t* leftCo, const int16_t* leftCg,
                        int16_t topLeftY, int16_t topLeftCo, int16_t topLeftCg,
                        BitstreamWriter& bs);

    // Encode a near-lossless tile. Like lossless but quantizes residuals
    // by maxError to improve compression while bounding the per-component error.
    void encodeNearLossless(const uint8_t* bgra, int bgraStride,
                            int tileW, int tileH,
                            const int16_t* topY, const int16_t* topCo, const int16_t* topCg,
                            const int16_t* leftY, const int16_t* leftCo, const int16_t* leftCg,
                            int16_t topLeftY, int16_t topLeftCo, int16_t topLeftCg,
                            int maxError, BitstreamWriter& bs);

    // Encode a lossy tile using DCT + quantization.
    void encodeLossy(const uint8_t* bgra, int bgraStride,
                     int tileW, int tileH, int qp,
                     BitstreamWriter& bs);

    // Get the last-encoded YCoCg-R planes (for use as neighbor context).
    const int16_t* lastY() const { return yBuf_.data(); }
    const int16_t* lastCo() const { return coBuf_.data(); }
    const int16_t* lastCg() const { return cgBuf_.data(); }

private:
    // Select the best prediction mode by trying all modes and picking lowest SAD.
    PredMode selectPredMode(const int16_t* channel, int w, int h,
                            const int16_t* top, const int16_t* left, int16_t topLeft);

    // Apply prediction and store residuals.
    void applyPrediction(const int16_t* src, int16_t* residual,
                         int w, int h, PredMode mode,
                         const int16_t* top, const int16_t* left, int16_t topLeft);

    // Encode residual symbols via rANS and write to bitstream.
    void encodeResiduals(const uint8_t* symbols, size_t totalSymbols,
                         BitstreamWriter& bs);

    // Working buffers
    std::vector<int16_t> yBuf_, coBuf_, cgBuf_;
    std::vector<int16_t> residualBuf_;    // 3 channels
    std::vector<int16_t> predBuf_;        // temp for prediction
    std::vector<int16_t> dctBuf_;         // DCT coefficients
    std::vector<uint8_t> symbolBuf_;
    std::vector<RANSSymbol> freqTable_;

    RANSEncoder ransEncoder_;
};

} // namespace omni
} // namespace omnidesk
