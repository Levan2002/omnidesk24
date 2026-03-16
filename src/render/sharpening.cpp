#include "render/sharpening.h"
#include "core/logger.h"

#include "render/gl_proc.h"
#include <GLFW/glfw3.h>

namespace omnidesk {

// AMD FidelityFX Contrast Adaptive Sharpening (CAS) - simplified version
static const char* kCasFS = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D inputTex;
uniform float sharpness;
uniform vec2 texelSize;

void main() {
    // Sample cross pattern
    vec3 c = texture(inputTex, vTexCoord).rgb;
    vec3 t = texture(inputTex, vTexCoord + vec2(0, -texelSize.y)).rgb;
    vec3 b = texture(inputTex, vTexCoord + vec2(0,  texelSize.y)).rgb;
    vec3 l = texture(inputTex, vTexCoord + vec2(-texelSize.x, 0)).rgb;
    vec3 r = texture(inputTex, vTexCoord + vec2( texelSize.x, 0)).rgb;

    // Min/max of neighborhood
    vec3 mnRGB = min(min(min(t, b), min(l, r)), c);
    vec3 mxRGB = max(max(max(t, b), max(l, r)), c);

    // Sharpening amount based on local contrast
    vec3 ampRGB = clamp(min(mnRGB, 1.0 - mxRGB) / mxRGB, 0.0, 1.0);
    ampRGB = sqrt(ampRGB);

    float peak = -1.0 / mix(8.0, 5.0, sharpness);
    vec3 w = ampRGB * peak;

    vec3 result = (c + (t + b + l + r) * w) / (1.0 + 4.0 * w);
    fragColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
)glsl";

static const char* kPassthroughVS = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

bool SharpeningFilter::init() {
    auto compile = [](uint32_t type, const char* src) -> uint32_t {
        uint32_t s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        int ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) return 0;
        return s;
    };

    uint32_t vs = compile(GL_VERTEX_SHADER, kPassthroughVS);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, kCasFS);
    if (!vs || !fs) return false;

    shader_ = glCreateProgram();
    glAttachShader(shader_, vs);
    glAttachShader(shader_, fs);
    glLinkProgram(shader_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    // Create FBO for rendering sharpened output
    glGenFramebuffers(1, &fbo_);

    // Fullscreen quad VAO
    float verts[] = {-1,-1, 0,0,  1,-1, 1,0,  -1,1, 0,1,  1,1, 1,1};
    glGenVertexArrays(1, &vao_);
    uint32_t vbo;
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          reinterpret_cast<void*>(2*sizeof(float)));
    glEnableVertexAttribArray(1);

    initialized_ = true;
    return true;
}

void SharpeningFilter::apply(uint32_t inputTexture, uint32_t outputTexture,
                             int width, int height) {
    if (!initialized_ || !enabled_) return;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);
    glViewport(0, 0, width, height);

    glUseProgram(shader_);
    glUniform1f(glGetUniformLocation(shader_, "sharpness"), strength_);
    glUniform2f(glGetUniformLocation(shader_, "texelSize"),
                1.0f / width, 1.0f / height);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTexture);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SharpeningFilter::destroy() {
    if (!initialized_) return;
    glDeleteProgram(shader_);
    glDeleteFramebuffers(1, &fbo_);
    glDeleteVertexArrays(1, &vao_);
    initialized_ = false;
}

} // namespace omnidesk
