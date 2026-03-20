#include "codec/omni/omni_decoder.h"
#include "core/simd_ycocg.h"
#include "core/logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>

namespace omnidesk {
namespace omni {

OmniCodecDecoder::OmniCodecDecoder() = default;
OmniCodecDecoder::~OmniCodecDecoder() = default;

bool OmniCodecDecoder::init(int width, int height) {
    width_ = width;
    height_ = height;
    tileSize_ = DEFAULT_TILE_SIZE;
    tilesX_ = (width + tileSize_ - 1) / tileSize_;
    tilesY_ = (height + tileSize_ - 1) / tileSize_;
    refStride_ = width * 4;
    refFrame_.resize(static_cast<size_t>(refStride_) * height, 0);
    initialized_ = true;
    hasRef_ = false;

    // Initialize thread pool and per-thread tile decoders
    numThreads_ = std::max(1u, std::thread::hardware_concurrency());
    threadPool_ = std::make_unique<ThreadPool>(numThreads_);

    tileDecoders_.resize(numThreads_);
    for (auto& dec : tileDecoders_) {
        dec.init(DEFAULT_TILE_SIZE);
    }

    LOG_INFO("OmniCodec decoder initialized: %dx%d, threads=%zu", width, height, numThreads_);
    return true;
}

void OmniCodecDecoder::reset() {
    hasRef_ = false;
    refFrame_.clear();
}

bool OmniCodecDecoder::decode(const uint8_t* data, size_t size, Frame& out) {
    if (size < OmniFrameHeader::SERIALIZED_SIZE) return false;

    BitstreamReader bs(data, size);

    // Read and verify header
    uint8_t hdrBuf[OmniFrameHeader::SERIALIZED_SIZE];
    bs.readBytes(hdrBuf, OmniFrameHeader::SERIALIZED_SIZE);
    if (bs.hasError()) return false;

    OmniFrameHeader hdr = OmniFrameHeader::deserialize(hdrBuf);
    if (hdr.magic != OMNI_MAGIC) {
        LOG_WARN("OmniCodec: invalid magic 0x%08X", hdr.magic);
        return false;
    }

    int width = hdr.width;
    int height = hdr.height;
    int tileSize = hdr.tileSize;
    int tilesX = hdr.tilesX;
    int tilesY = hdr.tilesY;
    int numTiles = tilesX * tilesY;
    (void)hdr.isKeyFrame();

    // Update decoder state if dimensions changed
    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        tileSize_ = tileSize;
        tilesX_ = tilesX;
        tilesY_ = tilesY;
        refStride_ = width * 4;
        refFrame_.resize(static_cast<size_t>(refStride_) * height, 0);
        hasRef_ = false;
        for (auto& dec : tileDecoders_) {
            dec.init(tileSize);
        }
    }

    // Allocate output frame as BGRA
    out.allocate(width, height, PixelFormat::BGRA);
    out.frameId = hdr.frameId;

    // Read tile mode map
    std::vector<TileMode> tileModes(numTiles);
    for (int i = 0; i < numTiles; ++i) {
        tileModes[i] = static_cast<TileMode>(bs.readBits(3));
        if (bs.hasError()) return false;
    }
    bs.alignToByte();

    // Pass 1 (sequential): Handle SKIP and COPY tiles, collect encoded tile info
    struct EncodedTileInfo {
        int tileIdx;
        int tx, ty;
        int px, py;
        int tw, th;
        TileMode mode;
    };
    std::vector<EncodedTileInfo> encodedTiles;
    encodedTiles.reserve(numTiles);

    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            int tileIdx = ty * tilesX + tx;
            int px = tx * tileSize;
            int py = ty * tileSize;
            int tw = std::min(tileSize, width - px);
            int th = std::min(tileSize, height - py);

            uint8_t* outTile = out.data.data() + py * out.stride + px * 4;

            switch (tileModes[tileIdx]) {
                case TileMode::SKIP: {
                    if (hasRef_) {
                        const uint8_t* refTile = refFrame_.data() + py * refStride_ + px * 4;
                        for (int row = 0; row < th; ++row) {
                            std::memcpy(outTile + row * out.stride,
                                        refTile + row * refStride_,
                                        tw * 4);
                        }
                    } else {
                        for (int row = 0; row < th; ++row) {
                            std::memset(outTile + row * out.stride, 0, tw * 4);
                        }
                    }
                    break;
                }

                case TileMode::COPY: {
                    int8_t mvX = static_cast<int8_t>(bs.readU8());
                    int16_t mvY = static_cast<int16_t>(bs.readU16());
                    if (bs.hasError()) return false;

                    if (hasRef_) {
                        for (int row = 0; row < th; ++row) {
                            int srcY = py + row + mvY;
                            int srcX = px + mvX;
                            if (srcY >= 0 && srcY < height && srcX >= 0 && srcX + tw <= width) {
                                const uint8_t* refRow = refFrame_.data() + srcY * refStride_ + srcX * 4;
                                std::memcpy(outTile + row * out.stride, refRow, tw * 4);
                            } else {
                                std::memset(outTile + row * out.stride, 0, tw * 4);
                            }
                        }
                    } else {
                        for (int row = 0; row < th; ++row) {
                            std::memset(outTile + row * out.stride, 0, tw * 4);
                        }
                    }
                    break;
                }

                default:
                    encodedTiles.push_back({tileIdx, tx, ty, px, py, tw, th, tileModes[tileIdx]});
                    break;
            }
        }
    }

    // Read shared frequency table (if present in header flags)
    bool useSharedTable = hdr.hasSharedFreqTable();
    std::vector<RANSSymbol> sharedFreqTable;
    std::vector<RANSDecodeEntry> sharedDecodeTable;

    if (useSharedTable) {
        uint16_t numNonZero = bs.readU16();
        if (bs.hasError()) return false;

        sharedFreqTable.resize(256, {0, 0, 0, 0});
        for (int i = 0; i < numNonZero; ++i) {
            uint8_t sym = bs.readU8();
            uint16_t freq = bs.readU16();
            sharedFreqTable[sym].freq = freq;
            if (bs.hasError()) return false;
        }

        // Rebuild cumulative frequencies
        uint16_t cumFreq = 0;
        for (int i = 0; i < 256; ++i) {
            sharedFreqTable[i].cumFreq = cumFreq;
            cumFreq += sharedFreqTable[i].freq;
        }

        // Build decode table once for all tiles
        sharedDecodeTable = buildDecodeTable(sharedFreqTable, 256);
    }

    // Read tile data size table
    uint16_t numEncodedTiles = bs.readU16();
    if (bs.hasError()) return false;

    if (static_cast<size_t>(numEncodedTiles) != encodedTiles.size()) {
        LOG_WARN("OmniCodec: tile count mismatch: expected %zu, got %u",
                 encodedTiles.size(), numEncodedTiles);
        return false;
    }

    std::vector<uint32_t> tileSizes(numEncodedTiles);
    for (uint16_t i = 0; i < numEncodedTiles; ++i) {
        tileSizes[i] = bs.readU32();
        if (bs.hasError()) return false;
    }

    size_t tileDataStart = bs.position();
    std::vector<size_t> tileOffsets(numEncodedTiles);
    size_t offset = tileDataStart;
    for (uint16_t i = 0; i < numEncodedTiles; ++i) {
        tileOffsets[i] = offset;
        offset += tileSizes[i];
    }

    if (offset > size) {
        LOG_WARN("OmniCodec: tile data extends beyond packet (need %zu, have %zu)", offset, size);
        return false;
    }

    // Pass 2 (parallel): Decode tiles with shared decode table
    // Set shared decode table on all decoders
    if (useSharedTable && !sharedDecodeTable.empty()) {
        for (auto& dec : tileDecoders_) {
            dec.setSharedDecodeTable(sharedDecodeTable.data());
        }
    }

    std::atomic<bool> decodeError{false};

    if (numEncodedTiles > 0) {
        std::vector<std::future<void>> futures;
        futures.reserve(numEncodedTiles);

        for (uint16_t i = 0; i < numEncodedTiles; ++i) {
            futures.push_back(threadPool_->submit([this, i, &encodedTiles, &tileOffsets,
                                                    &tileSizes, data, &out, &decodeError]() {
                if (decodeError.load(std::memory_order_relaxed)) return;

                const auto& tile = encodedTiles[i];
                BitstreamReader tileBs(data + tileOffsets[i], tileSizes[i]);
                uint8_t* outTile = out.data.data() + tile.py * out.stride + tile.px * 4;

                thread_local size_t tlDecIdx = []() {
                    static std::atomic<size_t> counter{0};
                    return counter.fetch_add(1, std::memory_order_relaxed);
                }();
                size_t decIdx = tlDecIdx % numThreads_;
                TileDecoder& dec = tileDecoders_[decIdx];

                bool ok = false;
                switch (tile.mode) {
                    case TileMode::LOSSLESS:
                        ok = dec.decodeLossless(tileBs, outTile, out.stride,
                                                tile.tw, tile.th,
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr,
                                                0, 0, 0);
                        break;

                    case TileMode::NEAR_LOSSLESS:
                        ok = dec.decodeNearLossless(tileBs, outTile, out.stride,
                                                    tile.tw, tile.th,
                                                    nullptr, nullptr, nullptr,
                                                    nullptr, nullptr, nullptr,
                                                    0, 0, 0);
                        break;

                    case TileMode::LOSSY:
                        ok = dec.decodeLossy(tileBs, outTile, out.stride,
                                             tile.tw, tile.th);
                        break;

                    default:
                        break;
                }

                if (!ok) {
                    decodeError.store(true, std::memory_order_relaxed);
                }
            }));
        }

        for (auto& f : futures) f.get();
    }

    // Clear shared decode table
    for (auto& dec : tileDecoders_) {
        dec.clearSharedDecodeTable();
    }

    if (decodeError.load()) {
        LOG_WARN("OmniCodec: one or more tiles failed to decode");
        return false;
    }

    // Update reference frame
    for (int y = 0; y < height; ++y) {
        std::memcpy(refFrame_.data() + y * refStride_,
                    out.data.data() + y * out.stride,
                    width * 4);
    }
    hasRef_ = true;

    return true;
}

} // namespace omni
} // namespace omnidesk
