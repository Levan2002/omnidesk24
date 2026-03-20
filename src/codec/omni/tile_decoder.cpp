#include "codec/omni/tile_decoder.h"
#include "core/simd_ycocg.h"
#include "core/simd_predict.h"
#include "core/simd_dct.h"
#include "core/logger.h"

#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace omni {

TileDecoder::TileDecoder() = default;

void TileDecoder::init(int maxTileSize) {
    int maxPixels = maxTileSize * maxTileSize;
    yBuf_.resize(maxPixels);
    coBuf_.resize(maxPixels);
    cgBuf_.resize(maxPixels);
    residualBuf_.resize(maxPixels * 3);
    predBuf_.resize(maxPixels);
    symbolBuf_.resize(maxPixels * 3 * 2);
}

void TileDecoder::inversePrediction(const int16_t* residual, int16_t* out,
                                     int w, int h, PredMode mode,
                                     const int16_t* top, const int16_t* left,
                                     int16_t topLeft) {
    int numPixels = w * h;

    if (mode == PredMode::LEFT_PIXEL) {
        inversePredictLeftPixel(residual, out, w, h);
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

    // Reconstruct: out = residual + prediction
    for (int i = 0; i < numPixels; ++i) {
        out[i] = residual[i] + predBuf_[i];
    }
}

bool TileDecoder::decodeResiduals(BitstreamReader& bs, size_t totalSymbols) {
    // If a shared decode table is set, use it (no per-tile freq table in stream)
    if (sharedDecodeTable_) {
        return decodeResidualsShared(bs, totalSymbols, sharedDecodeTable_);
    }

    // Read per-tile frequency table
    uint16_t numNonZero = bs.readU16();
    if (bs.hasError()) return false;

    std::vector<RANSSymbol> freqTable(256, {0, 0});
    for (int i = 0; i < numNonZero; ++i) {
        uint8_t sym = bs.readU8();
        uint16_t freq = bs.readU16();
        freqTable[sym].freq = freq;
        if (bs.hasError()) return false;
    }

    // Rebuild cumulative frequencies
    uint16_t cumFreq = 0;
    for (int i = 0; i < 256; ++i) {
        freqTable[i].cumFreq = cumFreq;
        cumFreq += freqTable[i].freq;
    }

    // Build decode table
    auto decodeTable = buildDecodeTable(freqTable, 256);

    // Read rANS encoded data
    uint32_t ransSize = bs.readU32();
    uint32_t symCount = bs.readU32();
    if (bs.hasError() || symCount != totalSymbols) return false;

    std::vector<uint8_t> ransData(ransSize);
    bs.readBytes(ransData.data(), ransSize);
    if (bs.hasError()) return false;

    // 4-way interleaved rANS decode
    if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);

    if (!ransDecoder_.decodeInterleaved(ransData.data(), ransData.size(),
                                         decodeTable.data(), totalSymbols,
                                         symbolBuf_.data())) {
        LOG_WARN("TileDecoder: rANS interleaved decode failed");
        return false;
    }

    return true;
}

bool TileDecoder::decodeResidualsShared(BitstreamReader& bs, size_t totalSymbols,
                                         const RANSDecodeEntry* sharedDecodeTable) {
    // Read rANS encoded data (no frequency table — use shared decode table)
    uint32_t ransSize = bs.readU32();
    uint32_t symCount = bs.readU32();
    if (bs.hasError() || symCount != totalSymbols) return false;

    std::vector<uint8_t> ransData(ransSize);
    bs.readBytes(ransData.data(), ransSize);
    if (bs.hasError()) return false;

    if (symbolBuf_.size() < totalSymbols) symbolBuf_.resize(totalSymbols);

    if (!ransDecoder_.decodeInterleaved(ransData.data(), ransData.size(),
                                         sharedDecodeTable, totalSymbols,
                                         symbolBuf_.data())) {
        LOG_WARN("TileDecoder: shared rANS interleaved decode failed");
        return false;
    }

    return true;
}

bool TileDecoder::decodeLossless(BitstreamReader& bs, uint8_t* bgra, int bgraStride,
                                  int tileW, int tileH,
                                  const int16_t* topY, const int16_t* topCo,
                                  const int16_t* topCg,
                                  const int16_t* leftY, const int16_t* leftCo,
                                  const int16_t* leftCg,
                                  int16_t topLeftY, int16_t topLeftCo,
                                  int16_t topLeftCg) {
    int numPixels = tileW * tileH;
    size_t totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;

    // Read prediction modes (3 bits each)
    PredMode yMode = static_cast<PredMode>(bs.readBits(3));
    PredMode coMode = static_cast<PredMode>(bs.readBits(3));
    PredMode cgMode = static_cast<PredMode>(bs.readBits(3));
    if (bs.hasError()) return false;

    // Decode rANS-compressed symbols
    bs.alignToByte();
    if (!decodeResiduals(bs, totalSymbols)) return false;

    // Reconstruct int16_t residuals from two-byte encoding
    int16_t* yRes = residualBuf_.data();
    int16_t* coRes = residualBuf_.data() + numPixels;
    int16_t* cgRes = residualBuf_.data() + numPixels * 2;

    size_t si = 0;
    for (int c = 0; c < 3; ++c) {
        int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
        for (int i = 0; i < numPixels; ++i) {
            uint16_t lo = symbolBuf_[si++];
            uint16_t hi = symbolBuf_[si++];
            res[i] = static_cast<int16_t>(lo | (hi << 8));
        }
    }

    // Inverse prediction
    inversePrediction(yRes, yBuf_.data(), tileW, tileH, yMode, topY, leftY, topLeftY);
    inversePrediction(coRes, coBuf_.data(), tileW, tileH, coMode, topCo, leftCo, topLeftCo);
    inversePrediction(cgRes, cgBuf_.data(), tileW, tileH, cgMode, topCg, leftCg, topLeftCg);

    // Convert YCoCg-R to BGRA
    yCoCgRToBgra(yBuf_.data(), coBuf_.data(), cgBuf_.data(),
                  tileW, tileH, bgra, bgraStride);

    return true;
}

bool TileDecoder::decodeNearLossless(BitstreamReader& bs, uint8_t* bgra, int bgraStride,
                                      int tileW, int tileH,
                                      const int16_t* topY, const int16_t* topCo,
                                      const int16_t* topCg,
                                      const int16_t* leftY, const int16_t* leftCo,
                                      const int16_t* leftCg,
                                      int16_t topLeftY, int16_t topLeftCo,
                                      int16_t topLeftCg) {
    int numPixels = tileW * tileH;
    size_t totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;

    // Read prediction modes
    PredMode yMode = static_cast<PredMode>(bs.readBits(3));
    PredMode coMode = static_cast<PredMode>(bs.readBits(3));
    PredMode cgMode = static_cast<PredMode>(bs.readBits(3));
    if (bs.hasError()) return false;

    // Read quantization step
    bs.alignToByte();
    uint8_t qStep = bs.readU8();
    if (bs.hasError() || qStep == 0) return false;

    // Decode rANS-compressed symbols
    if (!decodeResiduals(bs, totalSymbols)) return false;

    // Reconstruct quantized residuals
    int16_t* yRes = residualBuf_.data();
    int16_t* coRes = residualBuf_.data() + numPixels;
    int16_t* cgRes = residualBuf_.data() + numPixels * 2;

    size_t si = 0;
    for (int c = 0; c < 3; ++c) {
        int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
        for (int i = 0; i < numPixels; ++i) {
            uint16_t lo = symbolBuf_[si++];
            uint16_t hi = symbolBuf_[si++];
            res[i] = static_cast<int16_t>(lo | (hi << 8));
        }
    }

    // Dequantize residuals
    for (int c = 0; c < 3; ++c) {
        int16_t* res = (c == 0) ? yRes : (c == 1) ? coRes : cgRes;
        for (int i = 0; i < numPixels; ++i) {
            res[i] = static_cast<int16_t>(res[i] * qStep);
        }
    }

    // Inverse prediction
    inversePrediction(yRes, yBuf_.data(), tileW, tileH, yMode, topY, leftY, topLeftY);
    inversePrediction(coRes, coBuf_.data(), tileW, tileH, coMode, topCo, leftCo, topLeftCo);
    inversePrediction(cgRes, cgBuf_.data(), tileW, tileH, cgMode, topCg, leftCg, topLeftCg);

    // Convert YCoCg-R to BGRA
    yCoCgRToBgra(yBuf_.data(), coBuf_.data(), cgBuf_.data(),
                  tileW, tileH, bgra, bgraStride);

    return true;
}

bool TileDecoder::decodeLossy(BitstreamReader& bs, uint8_t* bgra, int bgraStride,
                               int tileW, int tileH) {
    int numPixels = tileW * tileH;
    size_t totalSymbols = static_cast<size_t>(numPixels) * 3 * 2;

    // Read block size (2 bits)
    uint8_t bsCode = bs.readBits(2);
    int blockSize = (bsCode == 0) ? 4 : (bsCode == 1) ? 8 : 16;
    if (bs.hasError()) return false;

    // Read QP
    bs.alignToByte();
    uint8_t qp = bs.readU8();
    if (bs.hasError()) return false;

    // Decode rANS-compressed symbols
    if (!decodeResiduals(bs, totalSymbols)) return false;

    // Reconstruct quantized DCT coefficients
    for (int c = 0; c < 3; ++c) {
        int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();
        size_t base = static_cast<size_t>(c) * numPixels * 2;
        for (int i = 0; i < numPixels; ++i) {
            uint16_t lo = symbolBuf_[base + i * 2];
            uint16_t hi = symbolBuf_[base + i * 2 + 1];
            ch[i] = static_cast<int16_t>(lo | (hi << 8));
        }
    }

    // Dequantize and inverse DCT per block
    int blocksX = tileW / blockSize;
    int blocksY = tileH / blockSize;

    for (int c = 0; c < 3; ++c) {
        int16_t* ch = (c == 0) ? yBuf_.data() : (c == 1) ? coBuf_.data() : cgBuf_.data();

        for (int by = 0; by < blocksY; ++by) {
            for (int bx = 0; bx < blocksX; ++bx) {
                // Extract block
                int16_t coeffs[16 * 16];
                for (int y = 0; y < blockSize; ++y) {
                    for (int x = 0; x < blockSize; ++x) {
                        coeffs[y * blockSize + x] =
                            ch[(by * blockSize + y) * tileW + (bx * blockSize + x)];
                    }
                }

                // Dequantize
                dequantize(coeffs, blockSize, blockSize, qp);

                // Inverse DCT
                int16_t block[16 * 16];
                dctInverse(coeffs, blockSize, block, blockSize, blockSize);

                // Clamp to [0, 255] for Y, [-255, 255] for Co/Cg and write back
                for (int y = 0; y < blockSize; ++y) {
                    for (int x = 0; x < blockSize; ++x) {
                        ch[(by * blockSize + y) * tileW + (bx * blockSize + x)] =
                            block[y * blockSize + x];
                    }
                }
            }
        }
    }

    // Convert YCoCg-R to BGRA
    yCoCgRToBgra(yBuf_.data(), coBuf_.data(), cgBuf_.data(),
                  tileW, tileH, bgra, bgraStride);

    return true;
}

} // namespace omni
} // namespace omnidesk
