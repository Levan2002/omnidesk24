#pragma once

#include "core/types.h"
#include <cstdint>

namespace omnidesk {

// OpenGL renderer for displaying decoded remote desktop frames.
// Uses I420→RGB shader for GPU color conversion and partial texture updates.
class GlRenderer {
public:
    GlRenderer();
    ~GlRenderer();

    bool init(int width, int height);

    // Upload a decoded I420 frame (or just dirty regions)
    void uploadFrame(const Frame& frame, const std::vector<Rect>& dirtyRects = {});

    // Render the current frame to screen
    void render(int viewportWidth, int viewportHeight);

    // Get texture ID for ImGui::Image()
    uint32_t textureId() const { return rgbTexture_; }

    // Update frame dimensions (on resolution change)
    void resize(int width, int height);

    void destroy();

private:
    bool createShader();
    void createTextures(int width, int height);

    int frameWidth_ = 0;
    int frameHeight_ = 0;

    // OpenGL objects
    uint32_t yTexture_ = 0;    // Y plane texture
    uint32_t uTexture_ = 0;    // U plane texture
    uint32_t vTexture_ = 0;    // V plane texture
    uint32_t rgbTexture_ = 0;  // Final RGB output (FBO)
    uint32_t fbo_ = 0;         // Framebuffer object for I420→RGB
    uint32_t shader_ = 0;      // I420→RGB shader program
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    bool initialized_ = false;
    bool dirty_ = false;  // true when new frame uploaded, cleared after render
};

} // namespace omnidesk
