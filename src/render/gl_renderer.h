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

    // Upload a decoded frame (I420 or BGRA). BGRA frames skip the I420→RGB
    // shader and upload directly to the RGB texture.
    void uploadFrame(const Frame& frame, const std::vector<Rect>& dirtyRects = {});

    // Render the current frame to screen. Returns true if a new frame was displayed.
    bool render(int viewportWidth, int viewportHeight);

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
    bool dirty_ = false;       // true when new frame uploaded, cleared after render
    bool needsShader_ = false; // true when I420 upload needs shader conversion

    // PBO (Pixel Buffer Object) for async texture upload.
    // Double-buffered: one PBO is being filled by CPU while GPU reads the other.
    uint32_t pboIds_[2] = {0, 0};
    int pboIndex_ = 0;           // toggles between 0 and 1 each frame
    size_t pboSize_ = 0;         // allocated size of each PBO
    bool pboSupported_ = false;  // set to true if GL functions are available
};

} // namespace omnidesk
