#pragma once

#include <cstdint>
#include <vector>

namespace omnidesk {
namespace omni {

// Hash-based scroll detection for OmniCodec COPY mode.
// Detects vertical scrolling by comparing row hashes between current
// and reference frames. When a row hash matches a different row in
// the reference frame, the tile can be encoded as COPY + motion vector.

struct ScrollResult {
    bool detected = false;
    int16_t mvX = 0;   // horizontal motion (pixels), typically 0 for scroll
    int16_t mvY = 0;   // vertical motion (pixels)
    float confidence = 0.0f;  // match confidence [0, 1]
};

class ScrollDetector {
public:
    ScrollDetector();

    // Initialize for given frame dimensions and tile size.
    void init(int width, int height, int tileSize);

    // Update reference frame row hashes.
    void updateReference(const uint8_t* bgra, int stride);

    // Detect scroll for a specific tile.
    // Returns scroll motion vector if detected.
    ScrollResult detectTileScroll(const uint8_t* currentFrame, int currentStride,
                                  int tileX, int tileY, int tileW, int tileH) const;

    // Reset reference hashes.
    void reset();

private:
    static uint64_t hashRow(const uint8_t* row, int widthBytes);

    int width_ = 0;
    int height_ = 0;
    int tileSize_ = 0;

    // Per-row hashes for the reference frame
    std::vector<uint64_t> refRowHashes_;

    // Maximum scroll distance to search (pixels)
    static constexpr int MAX_SCROLL_DISTANCE = 512;
};

} // namespace omni
} // namespace omnidesk
