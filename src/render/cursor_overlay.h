#pragma once

#include "core/types.h"
#include <cstdint>
#include <unordered_map>

namespace omnidesk {

// Renders cursor as a separate GL overlay, updated at 120Hz independently
// of the video frame rate. Supports cursor shape caching.
class CursorOverlay {
public:
    CursorOverlay();
    ~CursorOverlay();

    bool init();

    // Update cursor position (high frequency, 120Hz)
    void updatePosition(int x, int y);

    // Update cursor shape (only when changed, identified by hash)
    void updateShape(const CursorInfo& cursor);

    // Render cursor on top of the desktop frame
    void render(int viewportWidth, int viewportHeight,
                int frameWidth, int frameHeight);

    void destroy();

private:
    int cursorX_ = 0;
    int cursorY_ = 0;
    int hotspotX_ = 0;
    int hotspotY_ = 0;
    int cursorW_ = 0;
    int cursorH_ = 0;
    bool visible_ = true;

    uint32_t cursorTexture_ = 0;
    uint32_t shader_ = 0;
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;

    // Shape cache: hash → texture ID
    std::unordered_map<uint64_t, uint32_t> shapeCache_;
    uint64_t currentShapeHash_ = 0;
    bool initialized_ = false;
};

} // namespace omnidesk
