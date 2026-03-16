#pragma once

#include "core/types.h"
#include "core/ring_buffer.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace omnidesk {

class ICaptureSource;
class IDirtyRegionDetector;
class ContentClassifier;
class IEncoder;
class QualityTuner;
class AdaptiveBitrateController;

struct HostStats {
    float fps = 0;
    float bitrateMbps = 0;
    float encodeTimeMs = 0;
    float captureTimeMs = 0;
    int width = 0;
    int height = 0;
    std::string encoderName;
};

class HostSession {
public:
    HostSession();
    ~HostSession();

    bool start(const EncoderConfig& encConfig, const CaptureConfig& capConfig);
    void stop();

    HostStats getStats() const;

    // Called by transport when viewer sends quality report
    void onQualityReport(const QualityReport& report);

private:
    void captureLoop();
    void encodeLoop();

    std::unique_ptr<ICaptureSource> capture_;
    std::unique_ptr<IEncoder> encoder_;
    std::unique_ptr<IDirtyRegionDetector> diffDetector_;
    std::unique_ptr<ContentClassifier> classifier_;
    std::unique_ptr<QualityTuner> qualityTuner_;
    std::unique_ptr<AdaptiveBitrateController> rateController_;

    RingBuffer<Frame, 4> captureBuffer_;
    Frame previousFrame_;

    std::thread captureThread_;
    std::thread encodeThread_;
    std::atomic<bool> running_{false};

    // Stats (atomic for thread-safe reads)
    std::atomic<float> currentFps_{0};
    std::atomic<float> currentBitrate_{0};
    std::atomic<float> encodeTimeMs_{0};
    std::atomic<float> captureTimeMs_{0};
    int frameWidth_ = 0;
    int frameHeight_ = 0;

    EncoderConfig encoderConfig_;
    uint64_t frameCounter_ = 0;
};

} // namespace omnidesk
