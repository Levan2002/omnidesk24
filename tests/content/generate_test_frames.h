#pragma once

#include "core/types.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Generates reference test frames for visual quality and benchmark tests.
// All frames are BGRA format.

namespace omnidesk {
namespace test_content {

// Solid color frame (for baseline/static tests)
inline Frame solidColor(int w, int h, uint8_t b, uint8_t g, uint8_t r) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < f.data.size(); i += 4) {
        f.data[i] = b;
        f.data[i + 1] = g;
        f.data[i + 2] = r;
        f.data[i + 3] = 255;
    }
    return f;
}

// Horizontal gradient (smooth transitions — good for banding detection)
inline Frame gradient(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            uint8_t val = static_cast<uint8_t>(x * 255 / (w - 1));
            f.data[idx] = val;
            f.data[idx + 1] = val;
            f.data[idx + 2] = val;
            f.data[idx + 3] = 255;
        }
    }
    return f;
}

// Checkerboard pattern (sharp edges — good for text-like content testing)
inline Frame checkerboard(int w, int h, int blockSize = 8) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            uint8_t val = ((x / blockSize + y / blockSize) % 2 == 0) ? 0 : 255;
            f.data[idx] = val;
            f.data[idx + 1] = val;
            f.data[idx + 2] = val;
            f.data[idx + 3] = 255;
        }
    }
    return f;
}

// Simulated text document (alternating horizontal bars like text lines)
inline Frame textDocument(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    // White background
    std::memset(f.data.data(), 255, f.data.size());
    // Draw "text lines" as dark horizontal bars
    for (int y = 0; y < h; y++) {
        int lineInBlock = y % 20;  // 20px per "line"
        bool isText = lineInBlock >= 4 && lineInBlock < 16;
        if (isText) {
            // Simulate text with varying horizontal patterns
            for (int x = 40; x < w - 40; x++) {
                size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                // Create pseudo-random "character" blocks
                bool isChar = ((x / 7 + y / 3) * 31337) % 5 != 0;
                if (isChar) {
                    f.data[idx] = 30;      // Dark text
                    f.data[idx + 1] = 30;
                    f.data[idx + 2] = 30;
                }
            }
        }
    }
    return f;
}

// Color bars (standard test pattern)
inline Frame colorBars(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    // SMPTE-like color bars: white, yellow, cyan, green, magenta, red, blue, black
    const uint8_t bars[][3] = {
        {255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
        {255, 0, 255},   {0, 0, 255},   {255, 0, 0},   {0, 0, 0}
    };
    int barWidth = w / 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int bar = std::min(x / barWidth, 7);
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            f.data[idx] = bars[bar][0];     // B
            f.data[idx + 1] = bars[bar][1]; // G
            f.data[idx + 2] = bars[bar][2]; // R
            f.data[idx + 3] = 255;
        }
    }
    return f;
}

// High-motion noise frame (random pixels — worst case for encoder)
inline Frame randomNoise(int w, int h, uint32_t seed = 42) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    // Simple LCG for reproducible results
    uint32_t state = seed;
    for (size_t i = 0; i < f.data.size(); i += 4) {
        state = state * 1664525u + 1013904223u;
        f.data[i] = static_cast<uint8_t>(state);
        f.data[i + 1] = static_cast<uint8_t>(state >> 8);
        f.data[i + 2] = static_cast<uint8_t>(state >> 16);
        f.data[i + 3] = 255;
    }
    return f;
}

// Dark background with colored syntax-highlighted "code" lines.
inline Frame codeEditor(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    // Dark background (#1e1e1e)
    for (size_t i = 0; i < f.data.size(); i += 4) {
        f.data[i + 0] = 30;   // B
        f.data[i + 1] = 30;   // G
        f.data[i + 2] = 30;   // R
        f.data[i + 3] = 255;  // A
    }

    struct SyntaxColor { uint8_t r, g, b; };
    const SyntaxColor colors[] = {
        {86, 156, 214},   // blue (keywords)
        {78, 201, 176},   // teal (types)
        {206, 145, 120},  // orange (strings)
        {181, 206, 168},  // green (comments)
        {220, 220, 220},  // white (identifiers)
        {197, 134, 192},  // purple (operators)
    };

    int lineHeight = 20;
    int margin = 60;
    int charWidth = 9;
    for (int lineIdx = 0; lineIdx * lineHeight + 10 < h - 10; ++lineIdx) {
        int y0 = lineIdx * lineHeight + 10;
        int x = margin;
        int tokenIdx = 0;
        while (x < w - 20) {
            int tokenLen = 3 + ((lineIdx * 7 + tokenIdx * 13) % 12);
            const auto& color = colors[(lineIdx + tokenIdx) % 6];
            for (int dy = 3; dy < lineHeight - 3; ++dy) {
                for (int dx = 0; dx < tokenLen * charWidth && x + dx < w - 20; ++dx) {
                    bool isChar = ((dx / charWidth * 3 + dy) % 4 != 0);
                    if (isChar) {
                        uint8_t* row = f.data.data() + (y0 + dy) * f.stride;
                        row[(x + dx) * 4 + 0] = color.b;
                        row[(x + dx) * 4 + 1] = color.g;
                        row[(x + dx) * 4 + 2] = color.r;
                    }
                }
            }
            x += tokenLen * charWidth + charWidth;
            tokenIdx++;
            if (tokenIdx > 8) break;
        }
    }

    // Line numbers (gray, left gutter)
    for (int lineIdx = 0; lineIdx * lineHeight + 10 < h - 10; ++lineIdx) {
        int y0 = lineIdx * lineHeight + 10;
        for (int dy = 4; dy < lineHeight - 4; ++dy) {
            for (int dx = 20; dx < 50; ++dx) {
                if (((dx + dy) * 17 + lineIdx) % 3 == 0) {
                    uint8_t* row = f.data.data() + (y0 + dy) * f.stride;
                    row[dx * 4 + 0] = 100;
                    row[dx * 4 + 1] = 100;
                    row[dx * 4 + 2] = 100;
                }
            }
        }
    }
    return f;
}

// Simulated desktop with icons, taskbar, and wallpaper gradient.
inline Frame staticDesktop(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);

    // Blue gradient wallpaper
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        uint8_t blue = static_cast<uint8_t>(180 + 50 * y / std::max(h - 1, 1));
        uint8_t green = static_cast<uint8_t>(80 + 30 * y / std::max(h - 1, 1));
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = blue;
            row[x * 4 + 1] = green;
            row[x * 4 + 2] = 40;
            row[x * 4 + 3] = 255;
        }
    }

    // Taskbar (dark gray strip at bottom)
    for (int y = h - 48; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = 40;
            row[x * 4 + 1] = 40;
            row[x * 4 + 2] = 40;
        }
    }

    // Desktop "icons" (small colored squares)
    for (int iconRow = 0; iconRow < 5; ++iconRow) {
        for (int iconCol = 0; iconCol < 2; ++iconCol) {
            int ix = 40 + iconCol * 90;
            int iy = 40 + iconRow * 100;
            uint8_t cr = static_cast<uint8_t>(60 + iconRow * 40);
            uint8_t cg = static_cast<uint8_t>(100 + iconCol * 80);
            uint8_t cb = 180;
            for (int dy = 0; dy < 48 && iy + dy < h - 48; ++dy) {
                uint8_t* row = f.data.data() + (iy + dy) * f.stride;
                for (int dx = 0; dx < 48 && ix + dx < w; ++dx) {
                    row[(ix + dx) * 4 + 0] = cb;
                    row[(ix + dx) * 4 + 1] = cg;
                    row[(ix + dx) * 4 + 2] = cr;
                }
            }
        }
    }
    return f;
}

// Top half text on white, bottom half colorful pattern (browser with embedded video).
inline Frame mixedContent(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);

    int halfH = h / 2;
    std::memset(f.data.data(), 255, static_cast<size_t>(f.stride) * halfH);
    for (int y = 20; y < halfH - 10; y += 14) {
        for (int dy = 0; dy < 2 && y + dy < halfH; ++dy) {
            uint8_t* row = f.data.data() + (y + dy) * f.stride;
            for (int x = 30; x < w - 30; ++x) {
                if (((x / 6 + y / 3) * 31337) % 5 != 0) {
                    row[x * 4 + 0] = 20;
                    row[x * 4 + 1] = 20;
                    row[x * 4 + 2] = 20;
                }
            }
        }
    }

    for (int y = halfH; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            int t = (y - halfH) * 3 + x;
            row[x * 4 + 0] = static_cast<uint8_t>((t * 7) & 0xFF);
            row[x * 4 + 1] = static_cast<uint8_t>((t * 13 + 50) & 0xFF);
            row[x * 4 + 2] = static_cast<uint8_t>((t * 3 + 100) & 0xFF);
            row[x * 4 + 3] = 255;
        }
    }
    return f;
}

// All available pattern names for enumeration
inline const char* const kPatternNames[] = {
    "text_document", "gradient", "checkerboard", "solid",
    "code_editor", "random_noise", "mixed_content", "color_bars",
    "static_desktop"
};
inline constexpr int kPatternCount = 9;

// Generate a named pattern
inline Frame generatePattern(const char* name, int w, int h) {
    std::string s(name);
    if (s == "text_document") return textDocument(w, h);
    if (s == "gradient") return gradient(w, h);
    if (s == "checkerboard") return checkerboard(w, h);
    if (s == "solid") return solidColor(w, h, 128, 128, 128);
    if (s == "code_editor") return codeEditor(w, h);
    if (s == "random_noise") return randomNoise(w, h);
    if (s == "mixed_content") return mixedContent(w, h);
    if (s == "color_bars") return colorBars(w, h);
    if (s == "static_desktop") return staticDesktop(w, h);
    // Default to gradient
    return gradient(w, h);
}

} // namespace test_content
} // namespace omnidesk
