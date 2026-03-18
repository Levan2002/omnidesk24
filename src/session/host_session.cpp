#include "session/host_session.h"
#include "capture/capture.h"
#include "diff/region_detector.h"
#include "diff/content_classifier.h"
#include "diff/rect_merger.h"
#include "codec/codec_factory.h"
#include "codec/encoder.h"
#include "codec/quality_tuner.h"
#include "codec/rate_control.h"
#include "codec/adaptive_quality.h"
#include "core/clock.h"
#include "core/logger.h"
#include "core/simd_utils.h"

#include <chrono>

namespace omnidesk {

HostSession::HostSession() = default;

HostSession::~HostSession() {
    stop();
}

bool HostSession::start(const EncoderConfig& encConfig, const CaptureConfig& capConfig) {
    encoderConfig_ = encConfig;

    // Create capture source
    capture_ = createCaptureSource();
    if (!capture_ || !capture_->init(capConfig)) {
        LOG_ERROR("Failed to initialize screen capture");
        return false;
    }

    // Create encoder — try each backend until one initialises successfully.
    for (auto backend : CodecFactory::availableBackends()) {
        auto enc = CodecFactory::createEncoder(backend);
        if (enc && enc->init(encConfig)) {
            encoder_ = std::move(enc);
            break;
        }
        LOG_INFO("Encoder backend %s: skipped (init failed)",
                 CodecFactory::backendName(backend));
    }
    if (!encoder_) {
        LOG_ERROR("Failed to initialize encoder (no working backend)");
        return false;
    }

    auto info = encoder_->getInfo();
    LOG_INFO("Using encoder: %s (%s)", info.name.c_str(),
             info.isHardware ? "hardware" : "software");

    // Create diff detector, classifier, quality tuner, rate controller
    diffDetector_ = createDirtyRegionDetector();
    classifier_ = std::make_unique<ContentClassifier>();
    qualityTuner_ = std::make_unique<QualityTuner>();
    AdaptiveBitrateController::Config rcConfig;
    rcConfig.initialBitrateBps = encConfig.targetBitrateBps;
    rcConfig.maxBitrateBps = encConfig.maxBitrateBps;
    rateController_ = std::make_unique<AdaptiveBitrateController>(rcConfig);

    // Initialize adaptive quality controller
    AdaptiveQuality::Config aqConfig;
    aqConfig.nativeWidth = encConfig.width;
    aqConfig.nativeHeight = encConfig.height;
    adaptiveQuality_ = std::make_unique<AdaptiveQuality>(aqConfig);

    nativeWidth_ = encConfig.width;
    nativeHeight_ = encConfig.height;
    encodeWidth_ = encConfig.width;
    encodeHeight_ = encConfig.height;
    frameWidth_ = encConfig.width;
    frameHeight_ = encConfig.height;

    running_ = true;
    captureThread_ = std::thread(&HostSession::captureLoop, this);
    encodeThread_ = std::thread(&HostSession::encodeLoop, this);

    LOG_INFO("Host session started: %dx%d @ %.0f fps",
             encConfig.width, encConfig.height, encConfig.maxFps);
    return true;
}

void HostSession::setSendCallback(SendCallback cb) {
    std::lock_guard<std::mutex> lock(sendCbMutex_);
    sendCallback_ = std::move(cb);
}

void HostSession::stop() {
    running_ = false;
    if (captureThread_.joinable()) captureThread_.join();
    if (encodeThread_.joinable()) encodeThread_.join();

    encoder_.reset();
    capture_.reset();
    LOG_INFO("Host session stopped");
}

void HostSession::captureLoop() {
    currentTargetFps_.store(encoderConfig_.idleFps);  // start at idle FPS

    while (running_) {
        auto frameStart = std::chrono::steady_clock::now();

        // Adaptive interval: use current target FPS (set by encode loop)
        float fps = currentTargetFps_.load();
        auto targetInterval = std::chrono::microseconds(
            static_cast<int64_t>(1000000.0 / fps));

        Frame frame;
        auto result = capture_->captureFrame(frame);

        auto captureEnd = std::chrono::steady_clock::now();
        float captureMs = std::chrono::duration<float, std::milli>(captureEnd - frameStart).count();
        captureTimeMs_.store(captureMs);

        if (result.status == CaptureResult::OK) {
            frame.frameId = ++frameCounter_;
            frame.timestampUs = Clock::nowUs();

            // Push to ring buffer (drops frame if encoder is behind)
            if (!captureBuffer_.push(std::move(frame))) {
                // Encoder is behind — skip this frame
            }
        }

        // Frame rate limiting
        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < targetInterval) {
            std::this_thread::sleep_for(targetInterval - elapsed);
        }
    }
}

void HostSession::encodeLoop() {
    uint64_t encodedFrames = 0;
    auto fpsStart = std::chrono::steady_clock::now();

    while (running_) {
        auto frameOpt = captureBuffer_.pop();
        if (!frameOpt) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        Frame& capturedFrame = *frameOpt;
        auto encStart = std::chrono::steady_clock::now();

        // Detect dirty regions *before* conversion to avoid converting
        // a frame that turns out to be identical to the previous one.
        std::vector<Rect> dirtyRects;
        if (previousFrame_.data.empty()) {
            // First frame: full frame is dirty
            dirtyRects.push_back({0, 0, capturedFrame.width, capturedFrame.height});
        } else {
            dirtyRects = diffDetector_->detect(previousFrame_, capturedFrame);
        }

        // Skip if nothing changed — avoid the I420 conversion entirely.
        if (dirtyRects.empty()) {
            previousFrame_ = std::move(capturedFrame);
            continue;
        }

        // Merge small rects
        dirtyRects = RectMerger::merge(dirtyRects, 8);

        // Feed temporal information to the classifier so it can
        // distinguish MOTION vs TEXT vs STATIC regions accurately.
        if (!previousFrame_.data.empty()) {
            classifier_->updateTemporalState(previousFrame_, capturedFrame);
        }

        // Classify content in each region (uses BGRA capturedFrame)
        std::vector<RegionInfo> regions;
        regions.reserve(dirtyRects.size());
        for (const auto& rect : dirtyRects) {
            RegionInfo info;
            info.rect = rect;
            info.type = classifier_->classify(capturedFrame, rect);

            // Apply per-region QP adjustment from the quality tuner.
            // The encoder uses regionQP if set (> 0), otherwise its default.
            QPAdjustment qpAdj = qualityTuner_->adjust(26, info.type);
            if (qpAdj.skip) {
                continue; // skip encoding STATIC regions entirely
            }
            info.rect = rect;
            regions.push_back(info);
        }

        // Adaptive FPS: count motion vs non-motion regions
        int motionCount = 0;
        for (const auto& r : regions) {
            if (r.type == ContentType::MOTION) ++motionCount;
        }
        float motionFrac = regions.empty() ? 0.0f
                           : static_cast<float>(motionCount) / regions.size();
        // Smooth the ratio — used by adaptive quality controller for FPS decisions
        motionRatio_ = motionRatio_ * 0.7f + motionFrac * 0.3f;

        // Convert to I420 for encoding (only for frames that have changes)
        Frame i420Frame;
        convertFrameToI420(capturedFrame, i420Frame);

        // Adaptive resolution: resize I420 frame if encode resolution
        // differs from native capture resolution.
        Frame encodeFrame;
        if (encodeWidth_ != i420Frame.width || encodeHeight_ != i420Frame.height) {
            resizeI420(i420Frame, encodeFrame, encodeWidth_, encodeHeight_);
        } else {
            encodeFrame = std::move(i420Frame);
        }

        // Encode
        EncodedPacket packet;
        if (encoder_->encode(encodeFrame, regions, packet)) {
            packet.dirtyRects = std::move(dirtyRects);

            // Send via transport layer
            {
                std::lock_guard<std::mutex> lock(sendCbMutex_);
                if (sendCallback_) sendCallback_(packet);
            }

            auto encEnd = std::chrono::steady_clock::now();
            float encMs = std::chrono::duration<float, std::milli>(encEnd - encStart).count();
            encodeTimeMs_.store(encMs);

            // Update bitrate stats
            float bits = static_cast<float>(packet.data.size()) * 8.0f;
            currentBitrate_.store(bits / 1000000.0f);

            ++encodedFrames;

            // FPS calculation
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - fpsStart).count();
            if (elapsed >= 1.0f) {
                currentFps_.store(static_cast<float>(encodedFrames) / elapsed);
                encodedFrames = 0;
                fpsStart = now;
            }

            // --- Adaptive quality: update after encoding ---
            float frameBudgetMs = 1000.0f / currentTargetFps_.load();
            uint32_t currentBitrateBps = static_cast<uint32_t>(
                currentBitrate_.load() * 1000000.0f);

            adaptiveQuality_->update(encMs, frameBudgetMs, currentBitrateBps, motionRatio_);

            // Drive capture/encode rate from adaptive quality controller
            currentTargetFps_.store(adaptiveQuality_->targetFps());

            if (adaptiveQuality_->resolutionChanged()) {
                int newW = adaptiveQuality_->targetWidth();
                int newH = adaptiveQuality_->targetHeight();

                LOG_INFO("Adaptive quality: resolution %dx%d -> %dx%d (level %d)",
                         encodeWidth_, encodeHeight_, newW, newH,
                         adaptiveQuality_->currentLevel());

                encodeWidth_ = newW;
                encodeHeight_ = newH;
                frameWidth_ = newW;
                frameHeight_ = newH;

                // Reconfigure encoder with new resolution
                encoderConfig_.width = newW;
                encoderConfig_.height = newH;
                encoder_->updateBitrate(adaptiveQuality_->targetBitrate());

                // Reinitialize encoder for new resolution
                if (!encoder_->init(encoderConfig_)) {
                    LOG_ERROR("Failed to reinit encoder at %dx%d, reverting", newW, newH);
                    encoderConfig_.width = nativeWidth_;
                    encoderConfig_.height = nativeHeight_;
                    encodeWidth_ = nativeWidth_;
                    encodeHeight_ = nativeHeight_;
                    frameWidth_ = nativeWidth_;
                    frameHeight_ = nativeHeight_;
                    encoder_->init(encoderConfig_);
                    adaptiveQuality_->reset();
                }
            }
        }

        previousFrame_ = std::move(capturedFrame);
    }
}

HostStats HostSession::getStats() const {
    HostStats stats;
    stats.fps = currentFps_.load();
    stats.bitrateMbps = currentBitrate_.load();
    stats.encodeTimeMs = encodeTimeMs_.load();
    stats.captureTimeMs = captureTimeMs_.load();
    stats.width = frameWidth_;
    stats.height = frameHeight_;
    if (encoder_) stats.encoderName = encoder_->getInfo().name;
    return stats;
}

void HostSession::requestKeyFrame() {
    if (encoder_) encoder_->requestKeyFrame();
}

void HostSession::onQualityReport(const QualityReport& report) {
    if (rateController_) {
        uint32_t newBitrate = rateController_->onQualityReport(report);
        if (encoder_) {
            encoder_->updateBitrate(newBitrate);
        }
    }
}

} // namespace omnidesk
