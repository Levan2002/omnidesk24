#include "codec/omni/tile_encoder.h"
#include "core/simd_ycocg.h"
#include "core/simd_predict.h"
#include "core/simd_dct.h"

#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace omni {

TileEncoder::TileEncoder() = default;

void TileEncoder::init(int maxTileSize) {
    int maxPixels = maxTileSize * maxTileSize;
    yBuf_.resize(maxPixels);
    coBuf_.resize(maxPixels);
    cgBuf_.resize(maxPixels);
    residualBuf_.resize(maxPixels * 3);
    predBuf_.resize(maxPixels);
    dctBuf_.resize(maxPixels);
    symbolBuf_.resize(maxPixels * 3 * 2);
}

PredMode TileEncoder::selectPredMode(const int16_t* channel, int w, int h,
                                      const int16_t* top, const int16_t* left,
                                      int16_t topLeft) {
    int numPixels = w * h;

    // Fast path: when no neighbor context is available, DC/H/V/Planar all
    // predict zero, so only compare "predict-zero" vs LEFT_PIXEL.
    if (!top && !left) {
        // SAD against zero = sum of absolute values
        uint64_t zeroSAD = 0;
        for (int i = 0; i < numPixels; ++i) {
            zeroSAD += static_cast<uint64_t>(std::abs(channel[i]));
        }

        // LEFT_PIXEL SAD
        predictLeftPixel(channel, predBuf_.data(), w, h);
        uint64_t leftSAD = 0;
        for (int i = 0; i < numPixels; ++i) {
            leftSAD += static_cast<uint64_t>(std::abs(predBuf_[i]));
        }

        return (leftSAD < zeroSAD) ? PredMode::LEFT_PIXEL : PredMode::DC;
    }

    uint64_t bestSAD = UINT64_MAX;
    PredMode bestMode = PredMode::LEFT_PIXEL;

    // Try DC prediction
    predictDC(top, left, w, h, predBuf_.data());
    uint64_t sad = computeSAD(channel, predBuf_.data(), numPixels);
    if (sad < bestSAD) { bestSAD = sad; bestMode = PredMode::DC; }

    // Try Horizontal prediction
    if (left) {
        predictH(left, w, h, predBuf_.data());
        sad = computeSAD(channel, predBuf_.data(), numPixels);
        if (sad < bestSAD) { bestSAD = sad; bestMode = PredMode::HORIZONTAL; }
    }

    // Try Vertical prediction
    if (top) {
        predictV(top, w, h, predBuf_.data());
        sad = computeSAD(channel, predBuf_.data(), numPixels);
        if (sad < bestSAD) { bestSAD = sad; bestMode = PredMode::VERTICAL; }
    }

    // Try Planar prediction
    if (top && left) {
        predictPlanar(top, left, topLeft, w, h, predBuf_.data());
        sad = computeSAD(channel, predBuf_.data(), numPixels);
        if (sad < bestSAD) { bestSAD = sad; bestMode = PredMode::PLANAR; }
    }

    // Try left-pixel prediction (sequential, always available)
    predictLeftPixel(channel, predBuf_.data(), w, h);
    uint64_t leftSAD = 0;
    for (int i = 0; i < numPixels; ++i) {
        leftSAD += std::abs(predBuf_[i]);
    }
    if (leftSAD < bestSAD) {
        bestSAD = leftSAD;
        bestMode = PredMode::LEFT_PIXEL;
    }

    return bestMode;
}

void TileEncoder::applyPrediction(const int16_t* src, int16_t* residual,
                                   int w, int h, PredMode mode,
                                   const int16_t* top, const int16_t* left,
                                   int16_t topLeft) {
    int numPixels = w * h;

    if (mode == PredMode::LEFT_PIXEL) {
        predictLeftPixel(src, residual, w, h);
        return;
    }

    // Generate prediction buffer
    switch (mode) {
        case PredMode::DC:
            predictDC(top, left, w, h, predBuf_.data());
            break;
        case PredMode::HORIZONTAL:
            predictH(left, w, h, predBuf_.data());
            break;
        case PredMode::VERTICAL:
            predictV(top, w, h, predBuf_.data());
            break;
        case PredMode::PLANAR:
            predictPlanar(top, left, topLeft, w, h, predBuf_.data());
            break;
        default:
            break;
    }

    // Compute residual = src - prediction
    for (int i = 0; i < numPixels; ++i) {
        residual[i] = src[i] - predBuf_[i];
    }
}

void TileEncoder::encodeResiduals(const uint8_t* symbols, size_t totalSymbols,
                                   BitstreamWriter& bs) {
    // If a shared frequency table is set, use it (no per-tile table serialization)
    if (sharedFreqTable_) {
        encodeResidualsShared(symbols, totalSymbols, sharedFreqTable_, bs);
        return;
    }

    // Build per-tile frequency table
    uint32_t counts[256] = {};
    for (size_t i = 0; i < totalSymbols; ++i) {
        counts[symbols[i]]++;
    }
    freqTable_ = buildFrequencyTable(counts, 256);

    // Serialize frequency table
    bs.flushBits();
    uint16_t numNonZero = 0;
    for (int i = 0; i < 256; ++i) {
        if (freqTable_[i].freq > 0) ++numNonZero;
    }
    bs.writeU16(numNonZero);
    for (int i = 0; i < 256; ++i) {
        if (freqTable_[i].freq > 0) {
            bs.writeU8(static_cast<uint8_t>(i));
            bs.writeU16(freqTable_[i].freq);
        }
    }

    // 4-way interleaved rANS encode
    std::vector<uint8_t> ransData;
    ransData.reserve(totalSymbols);
    ransEncoder_.encodeInterleaved(symbols, totalSymbols, freqTable_.data(), 256, ransData);

    // Write encoded data
    bs.writeU32(static_cast<uint32_t>(ransData.size()));
    bs.writeU32(static_cast<uint32_t>(totalSymbols));
    bs.writeBytes(ransData.data(), ransData.size());
}

void TileEncoder::encodeLossless(const uint8_t* bgra, int bgraStride,
                                  int tileW, int tileH,
                                  const int16_t* topY, const int16_t* topCo,
                                  const int16_t* topCg,
                                  const int16_t* leftY, const int16_t* leftCo,
                                  const int16_t* leftCg,
                                  int16_t topLeftY, int16_t topLeftCo,
                                  int16_t topLeftCg,
                                  BitstreamWriter& bs) {
    int numPixels = tileW * tileH;

    // Step 1: Convert tile BGRA to YCoCg-R
    bgraToYCoCgR(bgra, tileW, tileH, bgraStride,
                  yBuf_.data(), coBuf_.data(), cgBuf_.data());

    // Step 2: Select best prediction mode per channel
    PredMode yMode = selectPredMode(yBuf_.data(), tileW, tileH, topY, leftY, topLeftY);
    PredMode coMode = selectPredMode(coBuf_.data(), tileW, tileH, topCo, leftCo, topLeftCo);
    PredMode cgMode = selectPredMode(cgBuf_.data(), tileW, tileH, topCg, leftCg, topLeftCg);

    // Write prediction modes (3 bits each = 9 bits)
    bs.writeBits(static_cast<uint8_t>(yMode), 3);
    bs.writeBits(static_cast<uint8_t>(coMode), 3);
    bs.writeBits(static_cast<uint8_t>(cgMode), 3);

    // Step 3: Compute residuals
    int16_t* yRes = residualBuf_.data();
    int16_t* coRes = residualBuf_.data() + numPixels;
    int16_t* cgRes = residualBuf_.data() + numPixels * 2;

    applyPrediction(yBuf_.data(), yRes, tileW, tileH, yMode, topY, leftY, topLeftY);
    applyPrediction(coBuf_.data(), coRes, tileW, tileH, coMode, topCo, leftCo, topLeftCo);
    applyPrediction(cgBuf_.data(), cgRes, tileW, tileH, cgMode, topCg, leftCg, topLeftCg);

    // Step 4: Check if all residuals fit in [-128, 127] for single-byte encoding
    bool singleByte = true;
    for (int i = 0; i < numPixels * 3 && singleByte; ++i) {
        int16_t v = residualBuf_[i];
        if (v < -128 || v > 127) singleByte = false;
    }

    // Write single-byte flag (1 bit)
    bs.writeBits(singleByte ? 1 : 0, 1);

    size_t totalSymbols;
    if (singleByte) {
        totalSymbols = static_cast<size_t>(numPixels) * 3;
        if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);
        size_t si = 0;
        for (int c = 0; c < 3; ++c) {
            const int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
            for (int i = 0; i < numPixels; ++i) {
                symbolBuf_[si++] = static_cast<uint8_t>(static_cast<int8_t>(res[i]));
            }
        }
    } else {
        totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;
        if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);
        size_t si = 0;
        for (int c = 0; c < 3; ++c) {
            const int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
            for (int i = 0; i < numPixels; ++i) {
                uint16_t u = static_cast<uint16_t>(res[i]);
                symbolBuf_[si++] = static_cast<uint8_t>(u & 0xFF);
                symbolBuf_[si++] = static_cast<uint8_t>((u >> 8) & 0xFF);
            }
        }
    }

    // Step 5: rANS encode and write
    encodeResiduals(symbolBuf_.data(), totalSymbols, bs);
}

void TileEncoder::encodeNearLossless(const uint8_t* bgra, int bgraStride,
                                      int tileW, int tileH,
                                      const int16_t* topY, const int16_t* topCo,
                                      const int16_t* topCg,
                                      const int16_t* leftY, const int16_t* leftCo,
                                      const int16_t* leftCg,
                                      int16_t topLeftY, int16_t topLeftCo,
                                      int16_t topLeftCg,
                                      int maxError, BitstreamWriter& bs) {
    int numPixels = tileW * tileH;

    // Convert BGRA to YCoCg-R
    bgraToYCoCgR(bgra, tileW, tileH, bgraStride,
                  yBuf_.data(), coBuf_.data(), cgBuf_.data());

    // Select prediction modes
    PredMode yMode = selectPredMode(yBuf_.data(), tileW, tileH, topY, leftY, topLeftY);
    PredMode coMode = selectPredMode(coBuf_.data(), tileW, tileH, topCo, leftCo, topLeftCo);
    PredMode cgMode = selectPredMode(cgBuf_.data(), tileW, tileH, topCg, leftCg, topLeftCg);

    bs.writeBits(static_cast<uint8_t>(yMode), 3);
    bs.writeBits(static_cast<uint8_t>(coMode), 3);
    bs.writeBits(static_cast<uint8_t>(cgMode), 3);

    // Compute residuals
    int16_t* yRes = residualBuf_.data();
    int16_t* coRes = residualBuf_.data() + numPixels;
    int16_t* cgRes = residualBuf_.data() + numPixels * 2;

    applyPrediction(yBuf_.data(), yRes, tileW, tileH, yMode, topY, leftY, topLeftY);
    applyPrediction(coBuf_.data(), coRes, tileW, tileH, coMode, topCo, leftCo, topLeftCo);
    applyPrediction(cgBuf_.data(), cgRes, tileW, tileH, cgMode, topCg, leftCg, topLeftCg);

    // Quantize residuals by maxError to reduce entropy
    int qStep = std::max(1, maxError);
    for (int c = 0; c < 3; ++c) {
        int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
        for (int i = 0; i < numPixels; ++i) {
            // Round-to-nearest quantization
            if (res[i] >= 0) {
                res[i] = static_cast<int16_t>((res[i] + qStep / 2) / qStep);
            } else {
                res[i] = static_cast<int16_t>(-(((-res[i]) + qStep / 2) / qStep));
            }
        }
    }

    // Write maxError so decoder can dequantize
    bs.flushBits();
    bs.writeU8(static_cast<uint8_t>(qStep));

    // Check if all residuals fit in [-128, 127] for single-byte encoding
    bool singleByte = true;
    for (int i = 0; i < numPixels * 3 && singleByte; ++i) {
        int16_t v = residualBuf_[i];
        if (v < -128 || v > 127) singleByte = false;
    }

    // Write single-byte flag (1 bit)
    bs.writeBits(singleByte ? 1 : 0, 1);

    size_t totalSymbols;
    if (singleByte) {
        totalSymbols = static_cast<size_t>(numPixels) * 3;
        if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);
        size_t si = 0;
        for (int c = 0; c < 3; ++c) {
            const int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
            for (int i = 0; i < numPixels; ++i) {
                symbolBuf_[si++] = static_cast<uint8_t>(static_cast<int8_t>(res[i]));
            }
        }
    } else {
        totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;
        if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);
        size_t si = 0;
        for (int c = 0; c < 3; ++c) {
            const int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
            for (int i = 0; i < numPixels; ++i) {
                uint16_t u = static_cast<uint16_t>(res[i]);
                symbolBuf_[si++] = static_cast<uint8_t>(u & 0xFF);
                symbolBuf_[si++] = static_cast<uint8_t>((u >> 8) & 0xFF);
            }
        }
    }

    encodeResiduals(symbolBuf_.data(), totalSymbols, bs);
}

void TileEncoder::encodeLossy(const uint8_t* bgra, int bgraStride,
                                int tileW, int tileH, int qp,
                                BitstreamWriter& bs) {
    int numPixels = tileW * tileH;

    // Convert BGRA to YCoCg-R
    bgraToYCoCgR(bgra, tileW, tileH, bgraStride,
                  yBuf_.data(), coBuf_.data(), cgBuf_.data());

    // Choose DCT block size based on tile dimensions
    // Use largest power-of-2 block that divides tile evenly, up to 16
    int blockSize = 8;
    if (tileW % 16 == 0 && tileH % 16 == 0) blockSize = 16;
    else if (tileW % 8 == 0 && tileH % 8 == 0) blockSize = 8;
    else blockSize = 4;

    // Write block size indicator (2 bits)
    uint8_t bsCode = (blockSize == 4) ? 0 : (blockSize == 8) ? 1 : 2;
    bs.writeBits(bsCode, 2);
    bs.flushBits();

    // For each channel, partition into DCT blocks, transform, quantize
    // Then two-byte encode all quantized coefficients and rANS compress.
    int blocksX = tileW / blockSize;
    int blocksY = tileH / blockSize;
    int coeffsPerBlock = blockSize * blockSize;
    int totalCoeffs = blocksX * blocksY * coeffsPerBlock;

    // Process each channel: DCT + quantize in-place
    // Store quantized coefficients back into yBuf_, coBuf_, cgBuf_
    for (int c = 0; c < 3; ++c) {
        int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();

        for (int by = 0; by < blocksY; ++by) {
            for (int bx = 0; bx < blocksX; ++bx) {
                // Extract block from channel (stored in row-major tileW stride)
                int16_t block[16 * 16];
                for (int y = 0; y < blockSize; ++y) {
                    for (int x = 0; x < blockSize; ++x) {
                        block[y * blockSize + x] =
                            ch[(by * blockSize + y) * tileW + (bx * blockSize + x)];
                    }
                }

                // Forward DCT
                int16_t coeffs[16 * 16];
                dctForward(block, blockSize, coeffs, blockSize, blockSize);

                // Quantize
                quantize(coeffs, blockSize, blockSize, qp);

                // Write back to channel buffer
                for (int y = 0; y < blockSize; ++y) {
                    for (int x = 0; x < blockSize; ++x) {
                        ch[(by * blockSize + y) * tileW + (bx * blockSize + x)] =
                            coeffs[y * blockSize + x];
                    }
                }
            }
        }
    }

    // Check if all quantized coefficients fit in [-128, 127] for single-byte encoding
    bool singleByte = true;
    for (int c = 0; c < 3 && singleByte; ++c) {
        const int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();
        for (int i = 0; i < numPixels; ++i) {
            if (ch[i] < -128 || ch[i] > 127) { singleByte = false; break; }
        }
    }

    // Write QP so decoder knows how to dequantize
    bs.writeU8(static_cast<uint8_t>(qp));

    // Write single-byte flag (1 bit)
    bs.writeBits(singleByte ? 1 : 0, 1);

    size_t totalSymbols;
    if (singleByte) {
        totalSymbols = static_cast<size_t>(numPixels) * 3;
        if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);
        size_t si = 0;
        for (int c = 0; c < 3; ++c) {
            const int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();
            for (int i = 0; i < numPixels; ++i) {
                symbolBuf_[si++] = static_cast<uint8_t>(static_cast<int8_t>(ch[i]));
            }
        }
    } else {
        totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;
        if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);
        size_t si = 0;
        for (int c = 0; c < 3; ++c) {
            const int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();
            for (int i = 0; i < numPixels; ++i) {
                uint16_t u = static_cast<uint16_t>(ch[i]);
                symbolBuf_[si++] = static_cast<uint8_t>(u & 0xFF);
                symbolBuf_[si++] = static_cast<uint8_t>((u >> 8) & 0xFF);
            }
        }
    }

    encodeResiduals(symbolBuf_.data(), totalSymbols, bs);
}

void TileEncoder::encodeResidualsShared(const uint8_t* symbols, size_t totalSymbols,
                                         const RANSSymbol* sharedTable,
                                         BitstreamWriter& bs) {
    // 4-way interleaved rANS encode using the shared table (no frequency table serialization)
    std::vector<uint8_t> ransData;
    ransData.reserve(totalSymbols);
    ransEncoder_.encodeInterleaved(symbols, totalSymbols, sharedTable, 256, ransData);

    // Write only encoded data size and symbol count (no frequency table)
    bs.flushBits();
    bs.writeU32(static_cast<uint32_t>(ransData.size()));
    bs.writeU32(static_cast<uint32_t>(totalSymbols));
    bs.writeBytes(ransData.data(), ransData.size());
}

void TileEncoder::collectStatistics(const uint8_t* bgra, int bgraStride,
                                     int tileW, int tileH, TileMode mode,
                                     int maxError, int qp,
                                     uint32_t* counts256) {
    int numPixels = tileW * tileH;

    // Convert to YCoCg-R
    bgraToYCoCgR(bgra, tileW, tileH, bgraStride,
                  yBuf_.data(), coBuf_.data(), cgBuf_.data());

    if (mode == TileMode::LOSSY) {
        // DCT + quantize path
        int blockSize = 8;
        if (tileW % 16 == 0 && tileH % 16 == 0) blockSize = 16;
        else if (tileW % 8 == 0 && tileH % 8 == 0) blockSize = 8;
        else blockSize = 4;

        int blocksX = tileW / blockSize;
        int blocksY = tileH / blockSize;

        for (int c = 0; c < 3; ++c) {
            int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();
            for (int by = 0; by < blocksY; ++by) {
                for (int bx = 0; bx < blocksX; ++bx) {
                    int16_t block[16 * 16], coeffs[16 * 16];
                    for (int y = 0; y < blockSize; ++y)
                        for (int x = 0; x < blockSize; ++x)
                            block[y * blockSize + x] = ch[(by * blockSize + y) * tileW + (bx * blockSize + x)];
                    dctForward(block, blockSize, coeffs, blockSize, blockSize);
                    quantize(coeffs, blockSize, blockSize, qp);
                    for (int y = 0; y < blockSize; ++y)
                        for (int x = 0; x < blockSize; ++x)
                            ch[(by * blockSize + y) * tileW + (bx * blockSize + x)] = coeffs[y * blockSize + x];
                }
            }
        }
    } else {
        // Prediction path (lossless/near-lossless)
        PredMode yMode = selectPredMode(yBuf_.data(), tileW, tileH, nullptr, nullptr, 0);
        PredMode coMode = selectPredMode(coBuf_.data(), tileW, tileH, nullptr, nullptr, 0);
        PredMode cgMode = selectPredMode(cgBuf_.data(), tileW, tileH, nullptr, nullptr, 0);

        int16_t* yRes = residualBuf_.data();
        int16_t* coRes = residualBuf_.data() + numPixels;
        int16_t* cgRes = residualBuf_.data() + numPixels * 2;

        applyPrediction(yBuf_.data(), yRes, tileW, tileH, yMode, nullptr, nullptr, 0);
        applyPrediction(coBuf_.data(), coRes, tileW, tileH, coMode, nullptr, nullptr, 0);
        applyPrediction(cgBuf_.data(), cgRes, tileW, tileH, cgMode, nullptr, nullptr, 0);

        if (mode == TileMode::NEAR_LOSSLESS && maxError > 0) {
            int qStep = std::max(1, maxError);
            for (int c = 0; c < 3; ++c) {
                int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
                for (int i = 0; i < numPixels; ++i) {
                    if (res[i] >= 0) res[i] = static_cast<int16_t>((res[i] + qStep / 2) / qStep);
                    else res[i] = static_cast<int16_t>(-(((-res[i]) + qStep / 2) / qStep));
                }
            }
        }

        // Copy residuals to yBuf/coBuf/cgBuf for symbol generation
        // (in lossy path, yBuf_ already has quantized DCT coefficients)
        std::memcpy(yBuf_.data(), residualBuf_.data(), numPixels * sizeof(int16_t));
        std::memcpy(coBuf_.data(), residualBuf_.data() + numPixels, numPixels * sizeof(int16_t));
        std::memcpy(cgBuf_.data(), residualBuf_.data() + numPixels * 2, numPixels * sizeof(int16_t));
    }

    // Two-byte encode into symbolBuf_
    size_t totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;
    if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);

    size_t si = 0;
    for (int c = 0; c < 3; ++c) {
        const int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();
        for (int i = 0; i < numPixels; ++i) {
            uint16_t u = static_cast<uint16_t>(ch[i]);
            symbolBuf_[si++] = static_cast<uint8_t>(u & 0xFF);
            symbolBuf_[si++] = static_cast<uint8_t>((u >> 8) & 0xFF);
        }
    }
    lastSymbolCount_ = totalSymbols;

    // Accumulate counts
    for (size_t i = 0; i < totalSymbols; ++i) {
        counts256[symbolBuf_[i]]++;
    }
}

} // namespace omni
} // namespace omnidesk
