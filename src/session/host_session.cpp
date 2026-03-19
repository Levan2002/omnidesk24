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

            // Push frame + platform dirty rects to ring buffer
            CapturedFrameEntry entry;
            entry.frame = std::move(frame);
            entry.platformDirtyRects = std::move(result.dirtyRects);
            if (!captureBuffer_.push(std::move(entry))) {
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
        auto entryOpt = captureBuffer_.pop();
        if (!entryOpt) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        CapturedFrameEntry& entry = *entryOpt;
        Frame& capturedFrame = entry.frame;
        auto encStart = std::chrono::steady_clock::now();

        // Detect dirty regions *before* conversion to avoid converting
        // a frame that turns out to be identical to the previous one.
        // Use platform-provided damage rects when available (XDamage/DXGI)
        // to skip the expensive SIMD block-comparison diff entirely.
        dirtyRects_.clear();
        if (previousFrame_.data.empty()) {
            dirtyRects_.push_back({0, 0, capturedFrame.width, capturedFrame.height});
        } else if (!entry.platformDirtyRects.empty()) {
            // Platform already told us what changed — skip SIMD diff
            dirtyRects_ = std::move(entry.platformDirtyRects);
        } else {
            dirtyRects_ = diffDetector_->detect(previousFrame_, capturedFrame);
        }

        // Skip if nothing changed — avoid the I420 conversion entirely.
        if (dirtyRects_.empty()) {
            ++consecutiveStaticFrames_;
            previousFrame_ = std::move(capturedFrame);
            continue;
        }
        consecutiveStaticFrames_ = 0;

        // Merge small rects
        dirtyRects_ = RectMerger::merge(dirtyRects_, 8);

        // Feed temporal information to the classifier so it can
        // distinguish MOTION vs TEXT vs STATIC regions accurately.
        if (!previousFrame_.data.empty()) {
            classifier_->updateTemporalState(previousFrame_, capturedFrame);
        }

        // Classify content in each region (uses BGRA capturedFrame)
        regions_.clear();
        regions_.reserve(dirtyRects_.size());
        for (const auto& rect : dirtyRects_) {
            RegionInfo info;
            info.rect = rect;
            info.type = classifier_->classify(capturedFrame, rect);

            // Apply per-region QP adjustment from the quality tuner.
            QPAdjustment qpAdj = qualityTuner_->adjust(26, info.type);
            if (qpAdj.skip) {
                continue; // skip encoding STATIC regions entirely
            }
            info.rect = rect;
            regions_.push_back(info);
        }

        // Adaptive FPS: count motion vs non-motion regions
        int motionCount = 0;
        for (const auto& r : regions_) {
            if (r.type == ContentType::MOTION) ++motionCount;
        }
        float motionFrac = regions_.empty() ? 0.0f
                           : static_cast<float>(motionCount) / regions_.size();
        motionRatio_ = motionRatio_ * 0.7f + motionFrac * 0.3f;

        // Convert only dirty regions to I420 — unchanged areas keep their
        // previous I420 data, saving significant CPU on typical desktop
        // workloads where only a small portion of the screen changes.
        convertDirtyRegionsToI420(capturedFrame, reusableI420Frame_, dirtyRects_);
        Frame& i420Frame = reusableI420Frame_;

        // Adaptive resolution: resize I420 frame if encode resolution
        // differs from native capture resolution.
        Frame resizedFrame;
        Frame* encodeFramePtr;
        if (encodeWidth_ != i420Frame.width || encodeHeight_ != i420Frame.height) {
            resizeI420(i420Frame, resizedFrame, encodeWidth_, encodeHeight_);
            encodeFramePtr = &resizedFrame;
        } else {
            encodeFramePtr = &i420Frame;
        }

        // Encode
        EncodedPacket packet;
        if (encoder_->encode(*encodeFramePtr, regions_, packet)) {
            packet.dirtyRects = std::move(dirtyRects_);

            // Send via transport layer
            {
                std::lock_guard<std::mutex> lock(sendCbMutex_);
                if (sendCallback_) sendCallback_(packet);
            }

            auto encEnd = std::chrono::steady_clock::now();
            float encMs = std::chrono::duration<float, std::milli>(encEnd - encStart).count();
            encodeTimeMs_.store(encMs);

            float bits = static_cast<float>(packet.data.size()) * 8.0f;
            currentBitrate_.store(bits / 1000000.0f);

            ++encodedFrames;

            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - fpsStart).count();
            if (elapsed >= 1.0f) {
                currentFps_.store(static_cast<float>(encodedFrames) / elapsed);
                encodedFrames = 0;
                fpsStart = now;
            }

            // --- Adaptive quality ---
            float frameBudgetMs = 1000.0f / currentTargetFps_.load();
            uint32_t currentBitrateBps = static_cast<uint32_t>(
                currentBitrate_.load() * 1000000.0f);

            adaptiveQuality_->update(encMs, frameBudgetMs, currentBitrateBps, motionRatio_);
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

                encoderConfig_.width = newW;
                encoderConfig_.height = newH;
                encoder_->updateBitrate(adaptiveQuality_->targetBitrate());

                // Try lightweight reconfigure first, fall back to full reinit
                if (!encoder_->reconfigure(newW, newH)) {
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
