#pragma once

#include "codec/encoder.h"
#include "codec/omni/omni_types.h"
#include "codec/omni/rans_codec.h"
#include "codec/omni/bitstream.h"
#include "codec/omni/tile_encoder.h"
#include "codec/omni/scroll_detector.h"
#include "diff/content_classifier.h"
#include "core/thread_pool.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace omnidesk {
namespace omni {

// OmniCodec Encoder -- implements IEncoder for the OmniDesk24 codec factory.
// Hybrid tile-based codec with 5 modes: SKIP, COPY, LOSSLESS, NEAR_LOSSLESS, LOSSY.
class OmniCodecEncoder : public IEncoder {
public:
    OmniCodecEncoder();
    ~OmniCodecEncoder() override;

    bool init(const EncoderConfig& cfg) override;
    bool encode(const Frame& frame, const std::vector<RegionInfo>& regions,
                EncodedPacket& out) override;
    void requestKeyFrame() override;
    void updateBitrate(uint32_t bps) override;
    EncoderInfo getInfo() override;

private:
    // Tile grid setup
    void setupTileGrid(int width, int height);

    // Compute tile hash for skip detection
    uint64_t computeTileHash(const uint8_t* bgra, int bgraStride,
                              int tileW, int tileH) const;

    // Decide tile mode based on content classification and bitrate budget
    TileMode decideTileMode(ContentType contentType, bool isScrolled) const;

    // Get QP for a tile based on content type and bitrate
    int getTileQP(ContentType contentType) const;

    // Configuration
    EncoderConfig config_;
    OmniEncoderConfig omniConfig_;

    // Frame dimensions and tile grid
    int width_ = 0;
    int height_ = 0;
    int tilesX_ = 0;
    int tilesY_ = 0;
    int tileSize_ = DEFAULT_TILE_SIZE;

    // Reference frame (BGRA) for skip detection
    std::vector<uint8_t> refFrame_;
    int refStride_ = 0;
    bool hasRef_ = false;

    // Per-tile hash array for fast skip detection
    std::vector<uint64_t> prevTileHashes_;
    std::vector<uint64_t> currTileHashes_;

    // Tile encoders -- one per thread for parallel encoding
    std::vector<TileEncoder> tileEncoders_;

    // Thread pool for parallel tile encoding
    std::unique_ptr<ThreadPool> threadPool_;
    size_t numThreads_ = 1;

    // Scroll detector for COPY mode
    ScrollDetector scrollDetector_;

    // Content classifier for per-tile mode decision
    ContentClassifier contentClassifier_;

    // Frame counter and state
    uint32_t frameCounter_ = 0;
    bool keyFrameRequested_ = true;
    uint32_t targetBitrateBps_ = 4000000;

    // Golden frame for error recovery (periodic full-quality reference)
    std::vector<uint8_t> goldenFrame_;
    uint32_t lastGoldenFrameId_ = 0;
};

} // namespace omni
} // namespace omnidesk
