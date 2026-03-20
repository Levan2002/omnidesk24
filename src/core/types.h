#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif

namespace omnidesk {

// ---- Serialization helpers (network byte order) ----

inline void writeU16(uint8_t* buf, uint16_t val) {
    uint16_t net = htons(val);
    std::memcpy(buf, &net, 2);
}

inline void writeU32(uint8_t* buf, uint32_t val) {
    uint32_t net = htonl(val);
    std::memcpy(buf, &net, 4);
}

inline uint16_t readU16(const uint8_t* buf) {
    uint16_t net;
    std::memcpy(&net, buf, 2);
    return ntohs(net);
}

inline uint32_t readU32(const uint8_t* buf) {
    uint32_t net;
    std::memcpy(&net, buf, 4);
    return ntohl(net);
}

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
    PixelFormat preferredInputFormat = PixelFormat::I420;  // OmniCodec prefers BGRA
};

// Encoder configuration
struct EncoderConfig {
    int32_t width = 1920;
    int32_t height = 1080;
    uint32_t targetBitrateBps = 4000000;  // 4 Mbps default (1080p screen content needs more)
    uint32_t maxBitrateBps = 8000000;     // 8 Mbps cap
    float maxFps = 60.0f;                  // Max FPS (adaptive: drops to 30 when idle)
    float idleFps = 30.0f;                 // FPS for low-motion (scroll, text, static)
    bool screenContent = true;             // Optimize for screen content
    uint8_t temporalLayers = 2;            // SVC temporal layers
    bool adaptiveQuantization = true;
};

// Capture configuration
struct CaptureConfig {
    int32_t monitorId = -1;   // -1 = primary monitor
    float maxFps = 60.0f;                  // Capture at max, encode loop adapts
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

// Input event types
enum class InputType : uint8_t {
    MOUSE_MOVE    = 0,
    MOUSE_DOWN    = 1,
    MOUSE_UP      = 2,
    MOUSE_SCROLL  = 3,
    KEY_DOWN      = 4,
    KEY_UP        = 5,
};

// Input event struct for mouse/keyboard events
struct InputEvent {
    InputType type     = InputType::MOUSE_MOVE;
    int32_t   x        = 0;
    int32_t   y        = 0;
    uint8_t   button   = 0;   // mouse button (0=left, 1=right, 2=middle)
    uint32_t  scancode = 0;   // keyboard scan code
    bool      pressed  = false;

    static constexpr size_t SIZE = 16;

    void serialize(uint8_t* buf) const {
        buf[0] = static_cast<uint8_t>(type);
        buf[1] = pressed ? 1 : 0;
        buf[2] = button;
        buf[3] = 0; // padding
        writeU32(buf + 4, static_cast<uint32_t>(x));
        writeU32(buf + 8, static_cast<uint32_t>(y));
        writeU32(buf + 12, scancode);
    }

    static InputEvent deserialize(const uint8_t* buf) {
        InputEvent ev;
        ev.type     = static_cast<InputType>(buf[0]);
        ev.pressed  = buf[1] != 0;
        ev.button   = buf[2];
        ev.x        = static_cast<int32_t>(readU32(buf + 4));
        ev.y        = static_cast<int32_t>(readU32(buf + 8));
        ev.scancode = readU32(buf + 12);
        return ev;
    }
};

// Peer address for signaling and communication
struct PeerAddress {
    std::string host;
    uint16_t port = 0;

    bool valid() const { return !host.empty() && port != 0; }
    bool operator==(const PeerAddress& o) const {
        return host == o.host && port == o.port;
    }
    std::string toString() const { return host + ":" + std::to_string(port); }
};

} // namespace omnidesk
