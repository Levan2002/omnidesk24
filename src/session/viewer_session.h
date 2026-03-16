#pragma once

#include "core/types.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

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
    std::string encoderName;
};

class ViewerSession {
public:
    ViewerSession();
    ~ViewerSession();

    bool start();
    void stop();

    ViewerStats getStats() const;

    // Called by transport when video data arrives
    void onVideoPacket(const EncodedPacket& packet);

    // Called by transport when cursor update arrives
    void onCursorUpdate(const CursorInfo& cursor);

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

    // Stats
    std::atomic<float> currentFps_{0};
    std::atomic<float> currentBitrate_{0};
    std::atomic<float> latencyMs_{0};
    std::atomic<float> packetLoss_{0};
    std::atomic<float> decodeTimeMs_{0};
    int frameWidth_ = 0;
    int frameHeight_ = 0;
};

} // namespace omnidesk
