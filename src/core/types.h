#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace omnidesk {

// Pixel formats
enum class PixelFormat : uint8_t {
    BGRA,   // 32-bit, common on Windows/X11 capture
    RGBA,   // 32-bit
    NV12,   // Hardware encoder preferred (Y plane + interleaved UV)
    I420,   // OpenH264 preferred (Y + U + V separate planes)
};

// Rectangle (pixel coordinates)
struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    int32_t right() const { return x + width; }
    int32_t bottom() const { return y + height; }
    int32_t area() const { return width * height; }
    bool empty() const { return width <= 0 || height <= 0; }

    bool intersects(const Rect& other) const {
        return x < other.right() && right() > other.x &&
               y < other.bottom() && bottom() > other.y;
    }

    Rect united(const Rect& other) const {
        int32_t nx = std::min(x, other.x);
        int32_t ny = std::min(y, other.y);
        return {nx, ny,
                std::max(right(), other.right()) - nx,
                std::max(bottom(), other.bottom()) - ny};
    }

    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
};

// Content type classification for encoding quality decisions
enum class ContentType : uint8_t {
    UNKNOWN,
    TEXT,       // Sharp edges, few colors → encode at higher quality
    MOTION,     // Video/animation → standard encoding
    STATIC,     // Unchanged region → skip encoding
};

struct RegionInfo {
    Rect rect;
    ContentType type = ContentType::UNKNOWN;
};

// Raw frame buffer
struct Frame {
    std::vector<uint8_t> data;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;          // Bytes per row (may include padding)
    PixelFormat format = PixelFormat::BGRA;
    uint64_t timestampUs = 0;    // Capture timestamp in microseconds
    uint64_t frameId = 0;

    size_t planeSize(int plane = 0) const {
        if (format == PixelFormat::I420) {
            if (plane == 0) return static_cast<size_t>(stride) * height;          // Y
            return static_cast<size_t>(stride / 2) * (height / 2);               // U or V
        }
        if (format == PixelFormat::NV12) {
            if (plane == 0) return static_cast<size_t>(stride) * height;          // Y
            return static_cast<size_t>(stride) * (height / 2);                    // UV
        }
        return static_cast<size_t>(stride) * height; // BGRA/RGBA
    }

    uint8_t* plane(int p) {
        if (p == 0) return data.data();
        if (format == PixelFormat::I420) {
            if (p == 1) return data.data() + planeSize(0);
            if (p == 2) return data.data() + planeSize(0) + planeSize(1);
        }
        if (format == PixelFormat::NV12 && p == 1) {
            return data.data() + planeSize(0);
        }
        return nullptr;
    }

    const uint8_t* plane(int p) const {
        return const_cast<Frame*>(this)->plane(p);
    }

    void allocate(int32_t w, int32_t h, PixelFormat fmt) {
        width = w;
        height = h;
        format = fmt;
        switch (fmt) {
            case PixelFormat::BGRA:
            case PixelFormat::RGBA:
                stride = w * 4;
                data.resize(static_cast<size_t>(stride) * h);
                break;
            case PixelFormat::I420:
                stride = w;
                data.resize(static_cast<size_t>(w) * h * 3 / 2);
                break;
            case PixelFormat::NV12:
                stride = w;
                data.resize(static_cast<size_t>(w) * h * 3 / 2);
                break;
        }
    }
};

// Encoded H.264 packet
struct EncodedPacket {
    std::vector<uint8_t> data;
    uint64_t frameId = 0;
    uint64_t timestampUs = 0;
    bool isKeyFrame = false;
    uint8_t temporalLayer = 0;
    std::vector<Rect> dirtyRects;  // Which regions this packet covers
};

// Cursor information
struct CursorInfo {
    int32_t x = 0;
    int32_t y = 0;
    int32_t hotspotX = 0;
    int32_t hotspotY = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> imageData;  // BGRA cursor image
    uint64_t shapeHash = 0;          // Hash for cursor shape caching
    bool visible = true;
    bool shapeChanged = false;
};

// Monitor information
struct MonitorInfo {
    int32_t id = 0;
    std::string name;
    Rect bounds;
    bool primary = false;
};

// User identity
struct UserID {
    std::string id;  // 8-char alphanumeric, persistent per installation

    bool valid() const { return id.length() == 8; }
    bool operator==(const UserID& o) const { return id == o.id; }
    bool operator!=(const UserID& o) const { return id != o.id; }
    bool operator<(const UserID& o) const { return id < o.id; }
};

// Encoder capabilities
struct EncoderInfo {
    std::string name;
    bool isHardware = false;
    bool supportsROI = false;         // Region-of-interest QP control
    bool supportsSVC = false;         // Temporal scalability
    int32_t maxWidth = 0;
    int32_t maxHeight = 0;
};

// Encoder configuration
struct EncoderConfig {
    int32_t width = 1920;
    int32_t height = 1080;
    uint32_t targetBitrateBps = 2000000;  // 2 Mbps default
    uint32_t maxBitrateBps = 5000000;     // 5 Mbps cap
    float maxFps = 60.0f;
    bool screenContent = true;             // Optimize for screen content
    uint8_t temporalLayers = 2;            // SVC temporal layers
    bool adaptiveQuantization = true;
};

// Capture configuration
struct CaptureConfig {
    int32_t monitorId = -1;   // -1 = primary monitor
    float maxFps = 60.0f;
    bool captureCursor = true;
};

// Network quality report from viewer to host
struct QualityReport {
    float rttMs = 0;
    float packetLossPercent = 0;
    float decodeTimeMs = 0;
    float displayTimeMs = 0;
    float jitterMs = 0;
};

} // namespace omnidesk
