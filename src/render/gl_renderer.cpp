#include "render/gl_renderer.h"
#include "core/logger.h"

#include "render/gl_proc.h"
#include <GLFW/glfw3.h>
#include <cstring>

// We need GL function pointers for GL 3.3 core
// On Linux these come from GL/glext.h loaded via glXGetProcAddress
// In practice, GLFW + glad/glew would be cleaner, but we keep it simple

namespace omnidesk {

// I420 to RGB fragment shader
static const char* kVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

// BT.601 full-range YCbCr to RGB conversion
// OpenH264 SCREEN_CONTENT mode outputs full-range Y (0-255), so no
// limited-range offset/scale is needed.  Using limited-range math on
// full-range data lifts blacks and clips whites, making text look faded.
static const char* kFragmentShader = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D yTex;
uniform sampler2D uTex;
uniform sampler2D vTex;

void main() {
    float y = texture(yTex, vTexCoord).r;
    float u = texture(uTex, vTexCoord).r - 128.0/255.0;
    float v = texture(vTex, vTexCoord).r - 128.0/255.0;

    // BT.601 full range
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    fragColor = vec4(clamp(r, 0.0, 1.0),
                     clamp(g, 0.0, 1.0),
                     clamp(b, 0.0, 1.0), 1.0);
}
)glsl";

GlRenderer::GlRenderer() = default;
GlRenderer::~GlRenderer() { destroy(); }

bool GlRenderer::init(int width, int height) {
    if (initialized_) destroy();
    frameWidth_ = width;
    frameHeight_ = height;

    if (!createShader()) {
        LOG_ERROR("Failed to create I420->RGB shader");
        return false;
    }

    createTextures(width, height);

    // Full-screen quad
    // DXGI captures top-to-bottom (row 0 = top of screen), but OpenGL
    // texture row 0 is the bottom.  Flip V so the image appears right-side up.
    float vertices[] = {
        // pos       // texcoord
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Initialize PBOs for async texture upload.
    // Total I420 frame size = Y + U + V = w*h + w*h/4 + w*h/4 = w*h*3/2
    pboSupported_ = (glMapBuffer != nullptr && glUnmapBuffer != nullptr);
    if (pboSupported_) {
        pboSize_ = static_cast<size_t>(width) * height * 3 / 2;
        glGenBuffers(2, pboIds_);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds_[i]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize_, nullptr, GL_STREAM_DRAW);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        pboIndex_ = 0;
        LOG_INFO("PBO async upload enabled: %zu bytes x2", pboSize_);
    }

    initialized_ = true;
    LOG_INFO("GL renderer initialized: %dx%d", width, height);
    return true;
}

void GlRenderer::createTextures(int width, int height) {
    auto createTex = [](uint32_t& tex, int w, int h) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    };

    createTex(yTexture_, width, height);
    createTex(uTexture_, width / 2, height / 2);
    createTex(vTexture_, width / 2, height / 2);

    // RGB output texture + FBO for I420→RGB pass
    glGenTextures(1, &rgbTexture_);
    glBindTexture(GL_TEXTURE_2D, rgbTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgbTexture_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool GlRenderer::createShader() {
    auto compile = [](uint32_t type, const char* src) -> uint32_t {
        uint32_t s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        int ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            LOG_ERROR("Shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    uint32_t vs = compile(GL_VERTEX_SHADER, kVertexShader);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;

    shader_ = glCreateProgram();
    glAttachShader(shader_, vs);
    glAttachShader(shader_, fs);
    glLinkProgram(shader_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    int ok;
    glGetProgramiv(shader_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(shader_, 512, nullptr, log);
        LOG_ERROR("Shader link error: %s", log);
        return false;
    }

    // Set texture unit uniforms
    glUseProgram(shader_);
    glUniform1i(glGetUniformLocation(shader_, "yTex"), 0);
    glUniform1i(glGetUniformLocation(shader_, "uTex"), 1);
    glUniform1i(glGetUniformLocation(shader_, "vTex"), 2);

    return true;
}

void GlRenderer::uploadFrame(const Frame& frame, const std::vector<Rect>& dirtyRects) {
    if (!initialized_ || frame.format != PixelFormat::I420) return;
    dirty_ = true;  // Mark that we need to re-render the I420->RGB pass

    if (frame.width != frameWidth_ || frame.height != frameHeight_) {
        resize(frame.width, frame.height);
    }

    // --- PBO async upload path (full frames only) ---
    // For full frame uploads, use double-buffered PBO to overlap CPU memcpy
    // with GPU DMA. For partial uploads, fall through to the direct path.
    if (dirtyRects.empty() && pboSupported_ && pboSize_ > 0) {
        size_t ySize = static_cast<size_t>(frame.width) * frame.height;
        size_t uvSize = static_cast<size_t>(frame.width / 2) * (frame.height / 2);
        size_t totalSize = ySize + uvSize * 2;

        if (totalSize <= pboSize_) {
            // Flip PBO index: GPU reads from [pboIndex_], CPU writes to [nextPbo]
            int nextPbo = 1 - pboIndex_;

            // 1) Initiate texture upload from the PREVIOUS PBO (GPU async DMA)
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds_[pboIndex_]);

            glBindTexture(GL_TEXTURE_2D, yTexture_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                            GL_RED, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(0));

            glBindTexture(GL_TEXTURE_2D, uTexture_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width / 2, frame.height / 2,
                            GL_RED, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(ySize));

            glBindTexture(GL_TEXTURE_2D, vTexture_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width / 2, frame.height / 2,
                            GL_RED, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(ySize + uvSize));

            // 2) Map the NEXT PBO and copy new frame data into it (CPU side)
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds_[nextPbo]);
            // Orphan the buffer to avoid GPU stall (driver can allocate a new one)
            glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize_, nullptr, GL_STREAM_DRAW);
            void* mapped = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
            if (mapped) {
                uint8_t* dst = static_cast<uint8_t*>(mapped);
                std::memcpy(dst, frame.plane(0), ySize);
                std::memcpy(dst + ySize, frame.plane(1), uvSize);
                std::memcpy(dst + ySize + uvSize, frame.plane(2), uvSize);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            }

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            pboIndex_ = nextPbo;
            return;
        }
    }

    // --- Direct upload path (non-PBO fallback, or partial dirty rects) ---
    if (dirtyRects.empty()) {
        // Full frame upload
        glBindTexture(GL_TEXTURE_2D, yTexture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                        GL_RED, GL_UNSIGNED_BYTE, frame.plane(0));

        glBindTexture(GL_TEXTURE_2D, uTexture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width / 2, frame.height / 2,
                        GL_RED, GL_UNSIGNED_BYTE, frame.plane(1));

        glBindTexture(GL_TEXTURE_2D, vTexture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width / 2, frame.height / 2,
                        GL_RED, GL_UNSIGNED_BYTE, frame.plane(2));
    } else {
        // Partial upload: only dirty regions
        for (const auto& r : dirtyRects) {
            // Y plane
            glBindTexture(GL_TEXTURE_2D, yTexture_);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride);
            for (int y = r.y; y < r.bottom() && y < frame.height; ++y) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, y, r.width, 1,
                                GL_RED, GL_UNSIGNED_BYTE,
                                frame.plane(0) + y * frame.stride + r.x);
            }

            // U plane (half resolution)
            int ux = r.x / 2, uy = r.y / 2, uw = r.width / 2, uh = r.height / 2;
            glBindTexture(GL_TEXTURE_2D, uTexture_);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride / 2);
            for (int y = uy; y < uy + uh && y < frame.height / 2; ++y) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, ux, y, uw, 1,
                                GL_RED, GL_UNSIGNED_BYTE,
                                frame.plane(1) + y * (frame.stride / 2) + ux);
            }

            // V plane
            glBindTexture(GL_TEXTURE_2D, vTexture_);
            for (int y = uy; y < uy + uh && y < frame.height / 2; ++y) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, ux, y, uw, 1,
                                GL_RED, GL_UNSIGNED_BYTE,
                                frame.plane(2) + y * (frame.stride / 2) + ux);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    }
}

void GlRenderer::render(int viewportWidth, int viewportHeight) {
    if (!initialized_) return;

    // Only run the I420→RGB shader when a new frame was uploaded.
    // The FBO retains the previous RGB result, so ImGui::Image can
    // keep displaying it without re-rendering — saves massive GPU.
    if (dirty_) {
        dirty_ = false;

        // I420→RGB into FBO
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, frameWidth_, frameHeight_);
        glUseProgram(shader_);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, yTexture_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uTexture_);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vTexture_);

        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glViewport(0, 0, viewportWidth, viewportHeight);
}

void GlRenderer::resize(int width, int height) {
    if (width == frameWidth_ && height == frameHeight_) return;

    // Delete old textures
    glDeleteTextures(1, &yTexture_);
    glDeleteTextures(1, &uTexture_);
    glDeleteTextures(1, &vTexture_);
    glDeleteTextures(1, &rgbTexture_);
    glDeleteFramebuffers(1, &fbo_);

    // Recreate PBOs for new size
    if (pboSupported_ && pboIds_[0] != 0) {
        glDeleteBuffers(2, pboIds_);
        pboIds_[0] = pboIds_[1] = 0;
    }

    frameWidth_ = width;
    frameHeight_ = height;
    createTextures(width, height);

    if (pboSupported_) {
        pboSize_ = static_cast<size_t>(width) * height * 3 / 2;
        glGenBuffers(2, pboIds_);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds_[i]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize_, nullptr, GL_STREAM_DRAW);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        pboIndex_ = 0;
    }

    LOG_INFO("GL renderer resized: %dx%d", width, height);
}

void GlRenderer::destroy() {
    if (!initialized_) return;

    glDeleteTextures(1, &yTexture_);
    glDeleteTextures(1, &uTexture_);
    glDeleteTextures(1, &vTexture_);
    glDeleteTextures(1, &rgbTexture_);
    glDeleteFramebuffers(1, &fbo_);
    glDeleteProgram(shader_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);

    if (pboIds_[0] != 0) {
        glDeleteBuffers(2, pboIds_);
        pboIds_[0] = pboIds_[1] = 0;
    }
    pboSize_ = 0;

    initialized_ = false;
}

} // namespace omnidesk
