#include "codec/omni/omni_decoder.h"
#include "core/simd_ycocg.h"
#include "core/logger.h"

#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace omni {

OmniCodecDecoder::OmniCodecDecoder() = default;
OmniCodecDecoder::~OmniCodecDecoder() = default;

bool OmniCodecDecoder::init(int width, int height) {
    width_ = width;
    height_ = height;
    initialized_ = true;
    hasRef_ = false;

    tileDecoder_.init(DEFAULT_TILE_SIZE);

    LOG_INFO("OmniCodec decoder initialized");
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
        tileDecoder_.init(tileSize);
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

    // Decode each tile
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
                    // Read motion vector
                    int8_t mvX = static_cast<int8_t>(bs.readU8());
                    int16_t mvY = static_cast<int16_t>(bs.readU16());
                    if (bs.hasError()) return false;

                    if (hasRef_) {
                        // Copy from reference with motion vector offset
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

                case TileMode::LOSSLESS: {
                    if (!tileDecoder_.decodeLossless(bs, outTile, out.stride,
                                                     tw, th,
                                                     nullptr, nullptr, nullptr,
                                                     nullptr, nullptr, nullptr,
                                                     0, 0, 0)) {
                        LOG_WARN("OmniCodec: lossless tile decode failed at (%d,%d)", tx, ty);
                        return false;
                    }
                    break;
                }

                case TileMode::NEAR_LOSSLESS: {
                    if (!tileDecoder_.decodeNearLossless(bs, outTile, out.stride,
                                                         tw, th,
                                                         nullptr, nullptr, nullptr,
                                                         nullptr, nullptr, nullptr,
                                                         0, 0, 0)) {
                        LOG_WARN("OmniCodec: near-lossless tile decode failed at (%d,%d)", tx, ty);
                        return false;
                    }
                    break;
                }

                case TileMode::LOSSY: {
                    if (!tileDecoder_.decodeLossy(bs, outTile, out.stride, tw, th)) {
                        LOG_WARN("OmniCodec: lossy tile decode failed at (%d,%d)", tx, ty);
                        return false;
                    }
                    break;
                }
            }
        }
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
