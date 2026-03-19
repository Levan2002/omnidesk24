#pragma once

#include "core/types.h"
#include "codec/codec_factory.h"
#include "analyzer/quality_metrics.h"
#include "analyzer/latency_profiler.h"
#include "analyzer/resource_monitor.h"

#include <memory>
#include <string>
#include <vector>

namespace omnidesk {
namespace analyzer {

struct AnalyzerConfig {
    CodecBackend backend = CodecBackend::OpenH264;
    int width = 1920;
    int height = 1080;
    uint32_t bitrateBps = 4000000;
    float fps = 30.0f;
    int frameCount = 60;
    std::string pattern = "text_document";
    bool screenContent = true;
    uint8_t temporalLayers = 2;
};

struct AnalyzerResult {
    AnalyzerConfig config;
    std::string backendName;

    // Per-frame data
    std::vector<metrics::FrameMetrics> frameMetrics;

    // Aggregate quality
    metrics::QualityStats qualityStats;

    // Latency
    LatencyStats encodeLatency;
    LatencyStats decodeLatency;
    LatencyStats colorConvertLatency;
    LatencyStats totalLatency;
    double effectiveFps = 0.0;

    // Resource usage
    ResourceStats resourceStats;

    // Success/failure
    bool success = false;
    std::string errorMessage;
};

// Core simulation engine: encode N frames → decode → measure quality/latency/resources.
class AnalyzerEngine {
public:
    AnalyzerEngine() = default;
    ~AnalyzerEngine() = default;

    // Run a single configuration analysis.
    AnalyzerResult run(const AnalyzerConfig& config);

    // Run a sweep across multiple configurations (all combos).
    struct SweepConfig {
        std::vector<CodecBackend> backends;  // empty = all available
        std::vector<std::pair<int, int>> resolutions = {{1920, 1080}, {1280, 720}, {960, 540}};
        std::vector<uint32_t> bitrates = {1000000, 2000000, 4000000, 8000000};
        std::vector<std::string> patterns = {"text_document", "code_editor", "gradient",
                                              "random_noise", "mixed_content", "static_desktop"};
        int frameCount = 60;
    };

    std::vector<AnalyzerResult> runSweep(const SweepConfig& sweepConfig);

private:
    // BGRA → I420 color conversion (BT.601)
    static Frame convertToI420(const Frame& bgra);
};

} // namespace analyzer
} // namespace omnidesk
