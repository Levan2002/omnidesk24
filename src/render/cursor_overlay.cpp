#include "render/cursor_overlay.h"
#include "core/logger.h"

#include "render/gl_proc.h"

namespace omnidesk {

static const char* kCursorVS = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform vec4 uRect; // x, y, w, h in NDC
void main() {
    vec2 pos = uRect.xy + aPos * uRect.zw;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

static const char* kCursorFS = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D cursorTex;
void main() {
    fragColor = texture(cursorTex, vTexCoord);
}
)glsl";

CursorOverlay::CursorOverlay() = default;
CursorOverlay::~CursorOverlay() { destroy(); }

bool CursorOverlay::init() {
    // Compile shader
    auto compile = [](uint32_t type, const char* src) -> uint32_t {
        uint32_t s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        int ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) return 0;
        return s;
    };

    uint32_t vs = compile(GL_VERTEX_SHADER, kCursorVS);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, kCursorFS);
    if (!vs || !fs) return false;

    shader_ = glCreateProgram();
    glAttachShader(shader_, vs);
    glAttachShader(shader_, fs);
    glLinkProgram(shader_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    // Unit quad
    float verts[] = {0,0, 0,1,  1,0, 1,1,  0,1, 0,0,  1,1, 1,0};
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          reinterpret_cast<void*>(2*sizeof(float)));
    glEnableVertexAttribArray(1);

    initialized_ = true;
    return true;
}

void CursorOverlay::updatePosition(int x, int y) {
    cursorX_ = x;
    cursorY_ = y;
}

void CursorOverlay::updateShape(const CursorInfo& cursor) {
    visible_ = cursor.visible;
    hotspotX_ = cursor.hotspotX;
    hotspotY_ = cursor.hotspotY;
    cursorW_ = cursor.width;
    cursorH_ = cursor.height;

    if (!cursor.shapeChanged) return;

    // Check cache
    auto it = shapeCache_.find(cursor.shapeHash);
    if (it != shapeCache_.end()) {
        currentShapeHash_ = cursor.shapeHash;
        cursorTexture_ = it->second;
        return;
    }

    // Create new texture
    uint32_t tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cursor.width, cursor.height,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, cursor.imageData.data());

    shapeCache_[cursor.shapeHash] = tex;
    currentShapeHash_ = cursor.shapeHash;
    cursorTexture_ = tex;
}

void CursorOverlay::render(int viewportWidth, int viewportHeight,
                           int frameWidth, int frameHeight) {
    if (!initialized_ || !visible_ || !cursorTexture_) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(shader_);

    // Convert cursor position to NDC
    float x = static_cast<float>(cursorX_ - hotspotX_) / frameWidth;
    float y = static_cast<float>(cursorY_ - hotspotY_) / frameHeight;
    float w = static_cast<float>(cursorW_) / frameWidth;
    float h = static_cast<float>(cursorH_) / frameHeight;

    // Scale to viewport
    float scaleX = static_cast<float>(viewportWidth) / frameWidth;
    float scaleY = static_cast<float>(viewportHeight) / frameHeight;
    float scale = std::min(scaleX, scaleY);
    (void)scale; // Used if we implement aspect-ratio-correct scaling

    glUniform4f(glGetUniformLocation(shader_, "uRect"), x, y, w, h);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cursorTexture_);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
}

void CursorOverlay::destroy() {
    if (!initialized_) return;
    for (auto& [hash, tex] : shapeCache_) {
        glDeleteTextures(1, &tex);
    }
    shapeCache_.clear();
    glDeleteProgram(shader_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);
    initialized_ = false;
}

} // namespace omnidesk
