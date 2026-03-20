#include "codec/omni/omni_encoder.h"
#include "core/simd_ycocg.h"
#include "core/logger.h"

#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace omni {

OmniCodecEncoder::OmniCodecEncoder() = default;
OmniCodecEncoder::~OmniCodecEncoder() = default;

bool OmniCodecEncoder::init(const EncoderConfig& cfg) {
    config_ = cfg;
    width_ = cfg.width;
    height_ = cfg.height;
    targetBitrateBps_ = cfg.targetBitrateBps;

    tileSize_ = omniConfig_.tileSize;
    setupTileGrid(width_, height_);

    // Initialize tile encoder
    tileEncoder_.init(tileSize_);

    // Initialize scroll detector
    if (omniConfig_.enableScrollDetection) {
        scrollDetector_.init(width_, height_, tileSize_);
    }

    // Allocate reference frame
    refStride_ = width_ * 4;
    refFrame_.resize(static_cast<size_t>(refStride_) * height_, 0);
    hasRef_ = false;

    // Allocate tile hash arrays
    int numTiles = tilesX_ * tilesY_;
    prevTileHashes_.assign(numTiles, 0);
    currTileHashes_.assign(numTiles, 0);

    keyFrameRequested_ = true;
    frameCounter_ = 0;

    LOG_INFO("OmniCodec encoder initialized: %dx%d, tile=%d, grid=%dx%d",
             width_, height_, tileSize_, tilesX_, tilesY_);
    return true;
}

void OmniCodecEncoder::setupTileGrid(int width, int height) {
    tilesX_ = (width + tileSize_ - 1) / tileSize_;
    tilesY_ = (height + tileSize_ - 1) / tileSize_;
}

uint64_t OmniCodecEncoder::computeTileHash(const uint8_t* bgra, int bgraStride,
                                            int tileW, int tileH) const {
    uint64_t h = 0x517CC1B727220A95ULL;
    for (int y = 0; y < tileH; ++y) {
        const uint8_t* row = bgra + y * bgraStride;
        int bytesPerRow = tileW * 4;
        for (int i = 0; i < bytesPerRow; ++i) {
            h ^= static_cast<uint64_t>(row[i]);
            h *= 0x100000001B3ULL;
        }
    }
    return h;
}

TileMode OmniCodecEncoder::decideTileMode(ContentType contentType, bool isScrolled) const {
    if (isScrolled) return TileMode::COPY;

    switch (contentType) {
        case ContentType::TEXT:
            return TileMode::LOSSLESS;
        case ContentType::STATIC:
            // Gradients and shadows: near-lossless saves bandwidth without visible loss
            return TileMode::NEAR_LOSSLESS;
        case ContentType::MOTION:
            return TileMode::LOSSY;
        case ContentType::UNKNOWN:
        default:
            // For unknown content, use lossless at high bitrate, lossy at low
            if (targetBitrateBps_ > 2000000) {
                return TileMode::LOSSLESS;
            }
            return TileMode::NEAR_LOSSLESS;
    }
}

int OmniCodecEncoder::getTileQP(ContentType contentType) const {
    int baseQP = omniConfig_.lossyBaseQP;
    switch (contentType) {
        case ContentType::TEXT:
            return std::max(1, baseQP + omniConfig_.textQPDelta);
        case ContentType::STATIC:
            return std::max(1, baseQP - 5);
        case ContentType::MOTION:
            return std::min(51, baseQP + 4);
        default:
            return baseQP;
    }
}

bool OmniCodecEncoder::encode(const Frame& frame,
                               const std::vector<RegionInfo>& /*regions*/,
                               EncodedPacket& out) {
    if (frame.width != width_ || frame.height != height_) {
        LOG_WARN("OmniCodec: frame size mismatch %dx%d vs %dx%d",
                 frame.width, frame.height, width_, height_);
        return false;
    }

    if (frame.format != PixelFormat::BGRA && frame.format != PixelFormat::RGBA) {
        LOG_ERROR("OmniCodec: requires BGRA/RGBA input, got format %d",
                  static_cast<int>(frame.format));
        return false;
    }

    bool isKeyFrame = keyFrameRequested_ || !hasRef_;
    keyFrameRequested_ = false;

    BitstreamWriter bs(width_ * height_);

    // Write frame header
    OmniFrameHeader hdr;
    hdr.magic = OMNI_MAGIC;
    hdr.frameId = ++frameCounter_;
    hdr.width = static_cast<uint16_t>(width_);
    hdr.height = static_cast<uint16_t>(height_);
    hdr.setKeyFrame(isKeyFrame);
    hdr.tileSize = static_cast<uint8_t>(tileSize_);
    hdr.tilesX = static_cast<uint16_t>(tilesX_);
    hdr.tilesY = static_cast<uint16_t>(tilesY_);

    uint8_t hdrBuf[OmniFrameHeader::SERIALIZED_SIZE];
    hdr.serialize(hdrBuf);
    bs.writeBytes(hdrBuf, OmniFrameHeader::SERIALIZED_SIZE);

    // Update scroll detector reference
    if (omniConfig_.enableScrollDetection && hasRef_) {
        scrollDetector_.updateReference(refFrame_.data(), refStride_);
    }

    // Content classification for mode decision
    contentClassifier_.updateTemporalState(
        // Build temp frames for classifier -- reuse refFrame_ as prev
        Frame{}, frame);  // prev frame not available as Frame here; skip temporal

    // Compute tile hashes and decide modes
    int numTiles = tilesX_ * tilesY_;
    std::vector<TileMode> tileModes(numTiles, TileMode::LOSSLESS);
    std::vector<ScrollResult> scrollResults(numTiles);
    std::vector<ContentType> tileContentTypes(numTiles, ContentType::UNKNOWN);

    for (int ty = 0; ty < tilesY_; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            int tileIdx = ty * tilesX_ + tx;
            int px = tx * tileSize_;
            int py = ty * tileSize_;
            int tw = std::min(tileSize_, width_ - px);
            int th = std::min(tileSize_, height_ - py);

            const uint8_t* tilePtr = frame.data.data() + py * frame.stride + px * 4;
            currTileHashes_[tileIdx] = computeTileHash(tilePtr, frame.stride, tw, th);

            // Skip detection
            if (!isKeyFrame && hasRef_ &&
                currTileHashes_[tileIdx] == prevTileHashes_[tileIdx]) {
                tileModes[tileIdx] = TileMode::SKIP;
                continue;
            }

            // Scroll detection
            bool isScrolled = false;
            if (!isKeyFrame && hasRef_ && omniConfig_.enableScrollDetection) {
                scrollResults[tileIdx] = scrollDetector_.detectTileScroll(
                    frame.data.data(), frame.stride, tx, ty, tw, th);
                isScrolled = scrollResults[tileIdx].detected;
            }

            // Content classification
            Rect tileRect{px, py, tw, th};
            ContentType ct = contentClassifier_.classify(frame, tileRect);
            tileContentTypes[tileIdx] = ct;

            // Mode decision
            tileModes[tileIdx] = decideTileMode(ct, isScrolled);
        }
    }

    // Write tile mode map (3 bits per tile)
    for (int i = 0; i < numTiles; ++i) {
        bs.writeBits(static_cast<uint8_t>(tileModes[i]), 3);
    }
    bs.flushBits();

    // Encode each non-SKIP tile
    for (int ty = 0; ty < tilesY_; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            int tileIdx = ty * tilesX_ + tx;
            if (tileModes[tileIdx] == TileMode::SKIP) continue;

            int px = tx * tileSize_;
            int py = ty * tileSize_;
            int tw = std::min(tileSize_, width_ - px);
            int th = std::min(tileSize_, height_ - py);

            const uint8_t* tilePtr = frame.data.data() + py * frame.stride + px * 4;

            switch (tileModes[tileIdx]) {
                case TileMode::COPY: {
                    // Write motion vector (2 bytes: mvX as int8, mvY as int16)
                    const auto& sr = scrollResults[tileIdx];
                    bs.writeU8(static_cast<uint8_t>(static_cast<int8_t>(sr.mvX)));
                    bs.writeU16(static_cast<uint16_t>(sr.mvY));
                    break;
                }

                case TileMode::LOSSLESS: {
                    // Use tile encoder with neighbor context (simplified: no cross-tile prediction for now)
                    tileEncoder_.encodeLossless(tilePtr, frame.stride,
                                                tw, th,
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr,
                                                0, 0, 0, bs);
                    break;
                }

                case TileMode::NEAR_LOSSLESS: {
                    tileEncoder_.encodeNearLossless(tilePtr, frame.stride,
                                                    tw, th,
                                                    nullptr, nullptr, nullptr,
                                                    nullptr, nullptr, nullptr,
                                                    0, 0, 0,
                                                    omniConfig_.nearLosslessMaxError, bs);
                    break;
                }

                case TileMode::LOSSY: {
                    int qp = getTileQP(tileContentTypes[tileIdx]);
                    tileEncoder_.encodeLossy(tilePtr, frame.stride,
                                             tw, th, qp, bs);
                    break;
                }

                default:
                    break;
            }
        }
    }

    // Update reference frame
    std::memcpy(refFrame_.data(), frame.data.data(),
                std::min(refFrame_.size(),
                         static_cast<size_t>(frame.stride) * frame.height));
    std::swap(prevTileHashes_, currTileHashes_);
    hasRef_ = true;

    // Golden frame: periodically save a full reference for error recovery
    uint32_t goldenInterval = static_cast<uint32_t>(
        omniConfig_.goldenFrameIntervalSec * 30);  // ~30 fps
    if (isKeyFrame || (frameCounter_ - lastGoldenFrameId_ >= goldenInterval)) {
        goldenFrame_ = refFrame_;
        lastGoldenFrameId_ = frameCounter_;
    }

    // Build output packet
    out.data = std::move(bs.data());
    out.frameId = frameCounter_;
    out.timestampUs = frame.timestampUs;
    out.isKeyFrame = isKeyFrame;
    out.temporalLayer = 0;

    return true;
}

void OmniCodecEncoder::requestKeyFrame() {
    keyFrameRequested_ = true;
}

void OmniCodecEncoder::updateBitrate(uint32_t bps) {
    targetBitrateBps_ = bps;

    // Adjust QP based on bitrate (higher bitrate = lower QP = better quality)
    // Rough mapping: 500kbps->40, 2Mbps->26, 8Mbps->15
    if (bps < 500000) {
        omniConfig_.lossyBaseQP = 40;
    } else if (bps < 2000000) {
        omniConfig_.lossyBaseQP = 26 + static_cast<int>((2000000.0 - bps) / 100000.0);
    } else if (bps < 8000000) {
        omniConfig_.lossyBaseQP = 15 + static_cast<int>((8000000.0 - bps) / 550000.0);
    } else {
        omniConfig_.lossyBaseQP = 15;
    }
}

EncoderInfo OmniCodecEncoder::getInfo() {
    EncoderInfo info;
    info.name = "OmniCodec";
    info.isHardware = false;
    info.supportsROI = true;
    info.supportsSVC = false;
    info.maxWidth = 7680;
    info.maxHeight = 4320;
    info.preferredInputFormat = PixelFormat::BGRA;
    return info;
}

} // namespace omni
} // namespace omnidesk
