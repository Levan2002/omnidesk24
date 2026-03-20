#pragma once

#include "codec/decoder.h"
#include "codec/omni/omni_types.h"
#include "codec/omni/tile_decoder.h"
#include "codec/omni/bitstream.h"
#include <cstdint>
#include <vector>

namespace omnidesk {
namespace omni {

// OmniCodec Decoder -- implements IDecoder for the OmniDesk24 codec factory.
// Handles all 5 tile modes: SKIP, COPY, LOSSLESS, NEAR_LOSSLESS, LOSSY.
class OmniCodecDecoder : public IDecoder {
public:
    OmniCodecDecoder();
    ~OmniCodecDecoder() override;

    bool init(int width, int height) override;
    bool decode(const uint8_t* data, size_t size, Frame& out) override;
    void reset() override;

private:
    // Frame dimensions and tile grid
    int width_ = 0;
    int height_ = 0;
    int tilesX_ = 0;
    int tilesY_ = 0;
    int tileSize_ = DEFAULT_TILE_SIZE;
    bool initialized_ = false;

    // Reference frame (BGRA) for SKIP/COPY tile reconstruction
    std::vector<uint8_t> refFrame_;
    int refStride_ = 0;
    bool hasRef_ = false;

    // Tile decoder
    TileDecoder tileDecoder_;
};

} // namespace omni
} // namespace omnidesk
