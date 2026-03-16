#pragma once

#include "core/types.h"
#include <cstdint>
#include <cstring>
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

} // namespace test_content
} // namespace omnidesk
