#pragma once

#include <cstdint>

namespace omnidesk {

// Contrast Adaptive Sharpening (CAS) post-processing shader.
// Applied to text regions to compensate for chroma subsampling artifacts.
class SharpeningFilter {
public:
    bool init();
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void setStrength(float strength) { strength_ = strength; } // 0.0 - 1.0

    // Apply sharpening to a texture region, output to another texture
    void apply(uint32_t inputTexture, uint32_t outputTexture,
               int width, int height);

    void destroy();

private:
    uint32_t shader_ = 0;
    uint32_t fbo_ = 0;
    uint32_t vao_ = 0;
    float strength_ = 0.5f;
    bool enabled_ = true;
    bool initialized_ = false;
};

} // namespace omnidesk
