#include "codec/omni/omni_encoder.h"
#include "core/simd_ycocg.h"
#include "core/logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>

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

    // Initialize thread pool and per-thread tile encoders
    numThreads_ = std::max(1u, std::thread::hardware_concurrency());
    threadPool_ = std::make_unique<ThreadPool>(numThreads_);

    tileEncoders_.resize(numThreads_);
    for (auto& enc : tileEncoders_) {
        enc.init(tileSize_);
    }

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

    LOG_INFO("OmniCodec encoder initialized: %dx%d, tile=%d, grid=%dx%d, threads=%zu",
             width_, height_, tileSize_, tilesX_, tilesY_, numThreads_);
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

    // Write frame header (with shared freq table flag)
    OmniFrameHeader hdr;
    hdr.magic = OMNI_MAGIC;
    hdr.frameId = ++frameCounter_;
    hdr.width = static_cast<uint16_t>(width_);
    hdr.height = static_cast<uint16_t>(height_);
    hdr.setKeyFrame(isKeyFrame);
    hdr.setSharedFreqTable(false);
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
        Frame{}, frame);

    // Compute tile hashes in parallel (batched across threads)
    int numTiles = tilesX_ * tilesY_;
    std::vector<TileMode> tileModes(numTiles, TileMode::LOSSLESS);
    std::vector<ScrollResult> scrollResults(numTiles);
    std::vector<ContentType> tileContentTypes(numTiles, ContentType::UNKNOWN);

    {
        size_t batchSize = (static_cast<size_t>(numTiles) + numThreads_ - 1) / numThreads_;
        std::vector<std::future<void>> hashFutures;
        hashFutures.reserve(numThreads_);
        for (size_t t = 0; t < numThreads_; ++t) {
            size_t start = t * batchSize;
            size_t end = std::min(start + batchSize, static_cast<size_t>(numTiles));
            if (start >= end) break;
            hashFutures.push_back(threadPool_->submit([this, &frame, start, end]() {
                for (size_t tileIdx = start; tileIdx < end; ++tileIdx) {
                    int tx = static_cast<int>(tileIdx) % tilesX_;
                    int ty = static_cast<int>(tileIdx) / tilesX_;
                    int px = tx * tileSize_;
                    int py = ty * tileSize_;
                    int tw = std::min(tileSize_, width_ - px);
                    int th = std::min(tileSize_, height_ - py);
                    const uint8_t* tilePtr = frame.data.data() + py * frame.stride + px * 4;
                    currTileHashes_[tileIdx] = computeTileHash(tilePtr, frame.stride, tw, th);
                }
            }));
        }
        for (auto& f : hashFutures) f.get();
    }

    // Mode decision (sequential — content classifier may have state)
    for (int tileIdx = 0; tileIdx < numTiles; ++tileIdx) {
        if (!isKeyFrame && hasRef_ &&
            currTileHashes_[tileIdx] == prevTileHashes_[tileIdx]) {
            tileModes[tileIdx] = TileMode::SKIP;
            continue;
        }

        int tx = tileIdx % tilesX_;
        int ty = tileIdx / tilesX_;
        int px = tx * tileSize_;
        int py = ty * tileSize_;
        int tw = std::min(tileSize_, width_ - px);
        int th = std::min(tileSize_, height_ - py);

        bool isScrolled = false;
        if (!isKeyFrame && hasRef_ && omniConfig_.enableScrollDetection) {
            scrollResults[tileIdx] = scrollDetector_.detectTileScroll(
                frame.data.data(), frame.stride, tx, ty, tw, th);
            isScrolled = scrollResults[tileIdx].detected;
        }

        Rect tileRect{px, py, tw, th};
        ContentType ct = contentClassifier_.classify(frame, tileRect);
        tileContentTypes[tileIdx] = ct;
        tileModes[tileIdx] = decideTileMode(ct, isScrolled);
    }

    // Write tile mode map (3 bits per tile)
    for (int i = 0; i < numTiles; ++i) {
        bs.writeBits(static_cast<uint8_t>(tileModes[i]), 3);
    }
    bs.flushBits();

    // Collect encode-candidate tiles and write COPY MVs
    struct TileJob {
        int tileIdx;
        int tx, ty;
        int px, py;
        int tw, th;
    };
    std::vector<TileJob> encodeJobs;
    encodeJobs.reserve(numTiles);

    for (int ty = 0; ty < tilesY_; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            int tileIdx = ty * tilesX_ + tx;
            TileMode mode = tileModes[tileIdx];

            if (mode == TileMode::SKIP) continue;

            if (mode == TileMode::COPY) {
                const auto& sr = scrollResults[tileIdx];
                bs.writeU8(static_cast<uint8_t>(static_cast<int8_t>(sr.mvX)));
                bs.writeU16(static_cast<uint16_t>(sr.mvY));
                continue;
            }

            int px = tx * tileSize_;
            int py = ty * tileSize_;
            int tw = std::min(tileSize_, width_ - px);
            int th = std::min(tileSize_, height_ - py);
            encodeJobs.push_back({tileIdx, tx, ty, px, py, tw, th});
        }
    }

    size_t jobCount = encodeJobs.size();

    if (jobCount > 0) {
        // Batched parallel tile encoding — submit numThreads_ tasks, each
        // processing a range of tiles. This minimizes thread pool overhead
        // (4 task submissions instead of 510 for 1080p).
        std::vector<BitstreamWriter> tileStreams(jobCount);
        {
            size_t batchSize = (jobCount + numThreads_ - 1) / numThreads_;
            std::vector<std::future<void>> encodeFutures;
            encodeFutures.reserve(numThreads_);

            for (size_t t = 0; t < numThreads_; ++t) {
                size_t jStart = t * batchSize;
                size_t jEnd = std::min(jStart + batchSize, jobCount);
                if (jStart >= jEnd) break;

                encodeFutures.push_back(threadPool_->submit([this, &encodeJobs, &tileModes,
                                                              &tileContentTypes, &frame,
                                                              &tileStreams, t, jStart, jEnd]() {
                    TileEncoder& enc = tileEncoders_[t];

                    for (size_t j = jStart; j < jEnd; ++j) {
                        const auto& job = encodeJobs[j];
                        const uint8_t* tilePtr = frame.data.data() +
                            job.py * frame.stride + job.px * 4;
                        BitstreamWriter& tileBs = tileStreams[j];

                        switch (tileModes[job.tileIdx]) {
                            case TileMode::LOSSLESS:
                                enc.encodeLossless(tilePtr, frame.stride, job.tw, job.th,
                                                   nullptr, nullptr, nullptr,
                                                   nullptr, nullptr, nullptr,
                                                   0, 0, 0, tileBs);
                                break;

                            case TileMode::NEAR_LOSSLESS:
                                enc.encodeNearLossless(tilePtr, frame.stride, job.tw, job.th,
                                                       nullptr, nullptr, nullptr,
                                                       nullptr, nullptr, nullptr,
                                                       0, 0, 0,
                                                       omniConfig_.nearLosslessMaxError, tileBs);
                                break;

                            case TileMode::LOSSY: {
                                int qp = getTileQP(tileContentTypes[job.tileIdx]);
                                enc.encodeLossy(tilePtr, frame.stride, job.tw, job.th, qp, tileBs);
                                break;
                            }

                            default:
                                break;
                        }
                    }
                }));
            }
            for (auto& f : encodeFutures) f.get();
        }

        // Write tile size table + tile data
        bs.writeU16(static_cast<uint16_t>(jobCount));
        for (size_t j = 0; j < jobCount; ++j) {
            bs.writeU32(static_cast<uint32_t>(tileStreams[j].size()));
        }
        for (size_t j = 0; j < jobCount; ++j) {
            const auto& tileData = tileStreams[j].data();
            if (!tileData.empty()) {
                bs.writeBytes(tileData.data(), tileData.size());
            }
        }
    } else {
        bs.writeU16(0);  // 0 encoded tiles
    }

    // Update reference frame
    std::memcpy(refFrame_.data(), frame.data.data(),
                std::min(refFrame_.size(),
                         static_cast<size_t>(frame.stride) * frame.height));
    std::swap(prevTileHashes_, currTileHashes_);
    hasRef_ = true;

    // Golden frame
    uint32_t goldenInterval = static_cast<uint32_t>(
        omniConfig_.goldenFrameIntervalSec * 30);
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
