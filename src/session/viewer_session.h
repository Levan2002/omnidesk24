#pragma once

#include "core/types.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace omnidesk {

class IDecoder;
class GlRenderer;
class CursorOverlay;
class SharpeningFilter;
class CursorPredictor;

struct ViewerStats {
    float fps = 0;
    float bitrateMbps = 0;
    float latencyMs = 0;
    float packetLossPercent = 0;
    float decodeTimeMs = 0;
    int width = 0;
    int height = 0;
    std::string encoderName;   // kept for protocol compat
    std::string decoderName;   // e.g. "OpenH264 (software)"
};

class ViewerSession {
public:
    ViewerSession();
    ~ViewerSession();

    bool start();
    void stop();

    ViewerStats getStats() const;

    // Called by transport when video data arrives (any thread).
    // Decodes the packet and queues the frame for GL upload.
    void onVideoPacket(const EncodedPacket& packet);

    // Called by WebRTC video track callback with raw H.264 NAL unit data.
    // Simpler than onVideoPacket — no EncodedPacket header, just raw NAL bytes.
    void onNalUnit(const uint8_t* data, size_t size);

    // Called by transport when cursor update arrives
    void onCursorUpdate(const CursorInfo& cursor);

    // Must be called on the GL/main thread each frame to upload
    // decoded frames to GL textures.
    void processOnGlThread();

    // Get renderer for ImGui integration
    GlRenderer* renderer() { return renderer_.get(); }

private:
    void decodeLoop();

    std::unique_ptr<IDecoder> decoder_;
    std::unique_ptr<GlRenderer> renderer_;
    std::unique_ptr<CursorOverlay> cursorOverlay_;
    std::unique_ptr<SharpeningFilter> sharpening_;
    std::unique_ptr<CursorPredictor> cursorPredictor_;

    std::thread decodeThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> decoding_{false};  // true while decode is in progress
    bool decoderInitialized_ = false;

    // Thread-safe frame queue: decoded frames waiting for GL upload
    std::mutex frameMutex_;
    Frame pendingFrame_;
    std::vector<Rect> pendingDirtyRects_;
    bool hasNewFrame_ = false;

    // Stats
    std::atomic<float> currentFps_{0};
    std::atomic<float> currentBitrate_{0};
    std::atomic<float> latencyMs_{0};
    std::atomic<float> packetLoss_{0};
    std::atomic<float> decodeTimeMs_{0};
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    std::string decoderName_;   // human-readable decoder name

    // FPS tracking
    uint64_t framesDecoded_ = 0;
    std::chrono::steady_clock::time_point fpsStart_;
};

} // namespace omnidesk
