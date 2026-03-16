#include "session/viewer_session.h"
#include "codec/codec_factory.h"
#include "codec/decoder.h"
#include "render/gl_renderer.h"
#include "render/cursor_overlay.h"
#include "render/sharpening.h"
#include "input/cursor_predictor.h"
#include "core/clock.h"
#include "core/logger.h"

#include <chrono>

namespace omnidesk {

ViewerSession::ViewerSession() = default;

ViewerSession::~ViewerSession() {
    stop();
}

bool ViewerSession::start() {
    // Create decoder
    decoder_ = CodecFactory::createDecoder();
    if (!decoder_) {
        LOG_ERROR("Failed to create decoder");
        return false;
    }

    // Create renderer components
    renderer_ = std::make_unique<GlRenderer>();
    cursorOverlay_ = std::make_unique<CursorOverlay>();
    sharpening_ = std::make_unique<SharpeningFilter>();
    cursorPredictor_ = std::make_unique<CursorPredictor>();

    cursorOverlay_->init();
    sharpening_->init();

    running_ = true;
    LOG_INFO("Viewer session started");
    return true;
}

void ViewerSession::stop() {
    running_ = false;
    if (decodeThread_.joinable()) decodeThread_.join();

    renderer_.reset();
    decoder_.reset();
    cursorOverlay_.reset();
    sharpening_.reset();
    LOG_INFO("Viewer session stopped");
}

void ViewerSession::onVideoPacket(const EncodedPacket& packet) {
    if (!decoder_ || !running_) return;

    auto decStart = std::chrono::steady_clock::now();

    Frame decoded;
    if (!decoder_->decode(packet.data.data(), packet.data.size(), decoded)) {
        LOG_WARN("Decode failed for frame %lu", packet.frameId);
        return;
    }

    auto decEnd = std::chrono::steady_clock::now();
    float decMs = std::chrono::duration<float, std::milli>(decEnd - decStart).count();
    decodeTimeMs_.store(decMs);

    // Initialize renderer on first frame
    if (frameWidth_ == 0 || decoded.width != frameWidth_ || decoded.height != frameHeight_) {
        frameWidth_ = decoded.width;
        frameHeight_ = decoded.height;
        renderer_->init(decoded.width, decoded.height);
    }

    // Upload to GL texture (dirty rects for partial update)
    renderer_->uploadFrame(decoded, packet.dirtyRects);

    // Update stats
    float bits = static_cast<float>(packet.data.size()) * 8.0f;
    currentBitrate_.store(bits / 1000000.0f);

    // Calculate latency from embedded timestamp
    uint64_t now = Clock::nowUs();
    float latency = static_cast<float>(now - packet.timestampUs) / 1000.0f;
    latencyMs_.store(latency);
}

void ViewerSession::onCursorUpdate(const CursorInfo& cursor) {
    if (cursorPredictor_) {
        cursorPredictor_->onServerUpdate(cursor.x, cursor.y);
    }
    if (cursorOverlay_) {
        cursorOverlay_->updateShape(cursor);
        int predX, predY;
        cursorPredictor_->getPosition(predX, predY);
        cursorOverlay_->updatePosition(predX, predY);
    }
}

ViewerStats ViewerSession::getStats() const {
    ViewerStats stats;
    stats.fps = currentFps_.load();
    stats.bitrateMbps = currentBitrate_.load();
    stats.latencyMs = latencyMs_.load();
    stats.packetLossPercent = packetLoss_.load();
    stats.decodeTimeMs = decodeTimeMs_.load();
    stats.width = frameWidth_;
    stats.height = frameHeight_;
    return stats;
}

} // namespace omnidesk
