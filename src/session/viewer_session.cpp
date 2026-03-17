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
    fpsStart_ = std::chrono::steady_clock::now();
    framesDecoded_ = 0;
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
        LOG_WARN("Decode failed for frame %llu", (unsigned long long)packet.frameId);
        return;
    }

    auto decEnd = std::chrono::steady_clock::now();
    float decMs = std::chrono::duration<float, std::milli>(decEnd - decStart).count();
    decodeTimeMs_.store(decMs);

    // Update stats
    float bits = static_cast<float>(packet.data.size()) * 8.0f;
    currentBitrate_.store(bits / 1000000.0f);

    uint64_t now = Clock::nowUs();
    float latency = static_cast<float>(now - packet.timestampUs) / 1000.0f;
    latencyMs_.store(latency);

    // Queue decoded frame for GL upload on the main thread
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        pendingFrame_ = std::move(decoded);
        pendingDirtyRects_ = packet.dirtyRects;
        hasNewFrame_ = true;
    }

    // FPS tracking
    ++framesDecoded_;
    float elapsed = std::chrono::duration<float>(decEnd - fpsStart_).count();
    if (elapsed >= 1.0f) {
        currentFps_.store(static_cast<float>(framesDecoded_) / elapsed);
        framesDecoded_ = 0;
        fpsStart_ = decEnd;
    }
}

void ViewerSession::processOnGlThread() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!hasNewFrame_ || !renderer_) return;

    // Initialize or resize renderer on resolution change
    if (frameWidth_ == 0 || pendingFrame_.width != frameWidth_ ||
        pendingFrame_.height != frameHeight_) {
        frameWidth_ = pendingFrame_.width;
        frameHeight_ = pendingFrame_.height;
        renderer_->init(pendingFrame_.width, pendingFrame_.height);
        LOG_INFO("Viewer renderer initialized: %dx%d", frameWidth_, frameHeight_);
    }

    // Upload to GL texture
    renderer_->uploadFrame(pendingFrame_, pendingDirtyRects_);
    hasNewFrame_ = false;
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
