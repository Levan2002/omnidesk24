#pragma once

#include "core/types.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace omnidesk {
namespace omni {

// Frame header magic: "OCOD" (OmniCODec)
constexpr uint32_t OMNI_MAGIC = 0x4F434F44;

// Tile encoding modes (3 bits)
enum class TileMode : uint8_t {
    SKIP          = 0,  // Unchanged, 0 bytes payload
    COPY          = 1,  // Motion vector reference to previous frame
    LOSSLESS      = 2,  // Pixel-perfect: prediction + rANS
    NEAR_LOSSLESS = 3,  // Quantized prediction + rANS (max error bounded)
    LOSSY         = 4,  // DCT + quantization + rANS
};

// Intra prediction modes (3 bits)
enum class PredMode : uint8_t {
    DC         = 0,  // Fill with average
    HORIZONTAL = 1,  // Copy left column across
    VERTICAL   = 2,  // Copy top row down
    PLANAR     = 3,  // Bilinear from top + left edges
    LEFT_PIXEL = 4,  // Sequential left/top neighbor (best for arbitrary content)
};

// DCT block sizes for lossy mode
enum class BlockSize : uint8_t {
    B4x4   = 0,
    B8x8   = 1,
    B16x16 = 2,
};

// Per-tile metadata (encoder-side)
struct TileInfo {
    int16_t tileX = 0;           // tile column index
    int16_t tileY = 0;           // tile row index
    TileMode mode = TileMode::SKIP;
    PredMode predMode = PredMode::DC;
    BlockSize blockSize = BlockSize::B8x8;
    int16_t mvX = 0, mvY = 0;   // COPY mode motion vector (pixels)
    uint8_t qp = 26;            // quantization parameter for LOSSY/NEAR_LOSSLESS
    ContentType contentType = ContentType::UNKNOWN;
};

// Frame header (16 bytes serialized)
struct OmniFrameHeader {
    uint32_t magic = OMNI_MAGIC;
    uint32_t frameId = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t flags = 0;       // bit0: keyFrame, bit1: goldenRef, bit2: usesSubTiles, bit3: sharedFreqTable
    uint8_t tileSize = 64;
    uint16_t tilesX = 0;
    uint16_t tilesY = 0;

    static constexpr size_t SERIALIZED_SIZE = 16;

    bool isKeyFrame() const { return (flags & 0x01) != 0; }
    void setKeyFrame(bool v) { if (v) flags |= 0x01; else flags &= ~0x01; }

    bool hasSharedFreqTable() const { return (flags & 0x08) != 0; }
    void setSharedFreqTable(bool v) { if (v) flags |= 0x08; else flags &= ~0x08; }

    void serialize(uint8_t* buf) const {
        writeU32(buf + 0, magic);
        writeU32(buf + 4, frameId);
        writeU16(buf + 8, width);
        writeU16(buf + 10, height);
        buf[12] = flags;
        buf[13] = tileSize;
        writeU16(buf + 14, static_cast<uint16_t>((tilesX << 8) | (tilesY & 0xFF)));
    }

    static OmniFrameHeader deserialize(const uint8_t* buf) {
        OmniFrameHeader h;
        h.magic = readU32(buf + 0);
        h.frameId = readU32(buf + 4);
        h.width = readU16(buf + 8);
        h.height = readU16(buf + 10);
        h.flags = buf[12];
        h.tileSize = buf[13];
        uint16_t packed = readU16(buf + 14);
        h.tilesX = (packed >> 8) & 0xFF;
        h.tilesY = packed & 0xFF;
        return h;
    }
};

// OmniCodec-specific encoder configuration
struct OmniEncoderConfig {
    int tileSize = 64;                     // 32 or 64
    int nearLosslessMaxError = 2;          // max per-component error
    int lossyBaseQP = 26;                  // base QP for lossy tiles
    int textQPDelta = -15;                 // QP reduction for text-adjacent lossy
    float scrollDetectionThreshold = 0.9f; // hash match confidence
    bool enableSubTileSplit = true;        // allow 64->32 splitting
    bool enableScrollDetection = true;
    int goldenFrameIntervalSec = 3;
};

// Default tile size
constexpr int DEFAULT_TILE_SIZE = 64;
constexpr int MIN_TILE_SIZE = 32;
constexpr int MAX_TILES_X = 256;
constexpr int MAX_TILES_Y = 256;

} // namespace omni
} // namespace omnidesk
