#include "session/host_session.h"
#include "capture/capture.h"
#include "diff/region_detector.h"
#include "diff/content_classifier.h"
#include "diff/rect_merger.h"
#include "codec/codec_factory.h"
#include "codec/encoder.h"
#include "codec/quality_tuner.h"
#include "codec/rate_control.h"
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

    // Create encoder (auto-selects best available)
    encoder_ = CodecFactory::createEncoder();
    if (!encoder_ || !encoder_->init(encConfig)) {
        LOG_ERROR("Failed to initialize encoder");
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
    auto targetInterval = std::chrono::microseconds(
        static_cast<int64_t>(1000000.0 / encoderConfig_.maxFps));

    while (running_) {
        auto frameStart = std::chrono::steady_clock::now();

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

        // Convert to I420 for encoding
        Frame i420Frame;
        convertFrameToI420(capturedFrame, i420Frame);

        // Detect dirty regions
        std::vector<Rect> dirtyRects;
        if (previousFrame_.data.empty()) {
            // First frame: full frame is dirty
            dirtyRects.push_back({0, 0, i420Frame.width, i420Frame.height});
        } else {
            dirtyRects = diffDetector_->detect(previousFrame_, capturedFrame);
        }

        // Skip if nothing changed
        if (dirtyRects.empty()) {
            previousFrame_ = std::move(capturedFrame);
            continue;
        }

        // Merge small rects
        dirtyRects = RectMerger::merge(dirtyRects, 8);

        // Classify content in each region
        std::vector<RegionInfo> regions;
        regions.reserve(dirtyRects.size());
        for (const auto& rect : dirtyRects) {
            RegionInfo info;
            info.rect = rect;
            info.type = classifier_->classify(capturedFrame, rect);
            regions.push_back(info);
        }

        // Encode
        EncodedPacket packet;
        if (encoder_->encode(i420Frame, regions, packet)) {
            packet.dirtyRects = dirtyRects;

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
