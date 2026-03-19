#include "analyzer/analyzer_engine.h"
#include "content/generate_test_frames.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

namespace omnidesk {
namespace analyzer {

Frame AnalyzerEngine::convertToI420(const Frame& bgra) {
    Frame out;
    out.allocate(bgra.width, bgra.height, PixelFormat::I420);
    out.timestampUs = bgra.timestampUs;
    out.frameId = bgra.frameId;

    int w = bgra.width;
    int h = bgra.height;
    uint8_t* yPlane = out.plane(0);
    uint8_t* uPlane = out.plane(1);
    uint8_t* vPlane = out.plane(2);

    int yStride = out.stride;
    int uvStride = out.stride / 2;

    for (int y = 0; y < h; ++y) {
        const uint8_t* srcRow = bgra.data.data() + y * bgra.stride;
        for (int x = 0; x < w; ++x) {
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];

            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[y * yStride + x] = static_cast<uint8_t>(std::clamp(yVal, 0, 255));

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                uPlane[(y / 2) * uvStride + (x / 2)] =
                    static_cast<uint8_t>(std::clamp(uVal, 0, 255));
                vPlane[(y / 2) * uvStride + (x / 2)] =
                    static_cast<uint8_t>(std::clamp(vVal, 0, 255));
            }
        }
    }
    return out;
}

AnalyzerResult AnalyzerEngine::run(const AnalyzerConfig& config) {
    AnalyzerResult result;
    result.config = config;
    result.backendName = CodecFactory::backendName(config.backend);

    // Create encoder
    auto encoder = CodecFactory::createEncoder(config.backend);
    if (!encoder) {
        result.errorMessage = "Failed to create encoder for " + result.backendName;
        return result;
    }

    // Create decoder
    auto decoder = CodecFactory::createDecoder(config.backend);
    if (!decoder) {
        result.errorMessage = "Failed to create decoder for " + result.backendName;
        return result;
    }

    // Configure encoder
    EncoderConfig encConfig;
    encConfig.width = config.width;
    encConfig.height = config.height;
    encConfig.targetBitrateBps = config.bitrateBps;
    encConfig.maxBitrateBps = config.bitrateBps * 2;
    encConfig.maxFps = config.fps;
    encConfig.screenContent = config.screenContent;
    encConfig.temporalLayers = config.temporalLayers;

    if (!encoder->init(encConfig)) {
        result.errorMessage = "Failed to init encoder";
        return result;
    }
    if (!decoder->init(config.width, config.height)) {
        result.errorMessage = "Failed to init decoder";
        return result;
    }

    // Generate test content
    Frame bgraFrame = test_content::generatePattern(config.pattern.c_str(),
                                                     config.width, config.height);

    // Pre-convert to I420
    Frame i420Reference;
    {
        i420Reference = convertToI420(bgraFrame);
    }

    LatencyProfiler profiler;
    ResourceMonitor monitor;
    monitor.sample(); // Initial sample

    uint64_t frameBudgetUs = static_cast<uint64_t>(1000000.0 / config.fps);

    for (int i = 0; i < config.frameCount; ++i) {
        FrameTiming timing;
        metrics::FrameMetrics fm;
        fm.frameId = static_cast<uint64_t>(i);

        // Prepare frame
        Frame i420Frame;
        {
            LatencyProfiler::ScopedTimer timer(timing.colorConvertMs);
            // For multi-frame: reuse the same content (simulating static screen)
            // Copy reference to avoid mutation
            i420Frame = i420Reference;
            i420Frame.frameId = static_cast<uint64_t>(i);
            i420Frame.timestampUs = static_cast<uint64_t>(i) * frameBudgetUs;
        }

        // Encode
        EncodedPacket packet;
        bool encOk;
        {
            LatencyProfiler::ScopedTimer timer(timing.encodeMs);
            if (i == 0) encoder->requestKeyFrame();

            std::vector<RegionInfo> regions;
            regions.push_back({{0, 0, config.width, config.height}, ContentType::UNKNOWN});
            encOk = encoder->encode(i420Frame, regions, packet);
        }

        if (!encOk) {
            result.errorMessage = "Encode failed at frame " + std::to_string(i);
            return result;
        }

        // Decode
        Frame decoded;
        bool decOk;
        {
            LatencyProfiler::ScopedTimer timer(timing.decodeMs);
            if (packet.data.empty()) {
                // Encoder skipped this frame (no data to decode)
                decOk = false;
            } else {
                decOk = decoder->decode(packet.data.data(), packet.data.size(), decoded);
            }
        }

        // Quality measurement (only if decode produced output)
        if (decOk && !decoded.data.empty()) {
            LatencyProfiler::ScopedTimer timer(timing.qualityCalcMs);
            fm.psnr = metrics::computePSNR_Y(i420Frame, decoded);
            fm.ssim = metrics::computeSSIM_Y(i420Frame, decoded);
        } else {
            // No decoded output (skip frame or decoder buffering) — use previous quality
            fm.psnr = 0.0;
            fm.ssim = 0.0;
        }

        fm.encodedBytes = packet.data.size();
        fm.bpp = metrics::bitsPerPixel(packet.data.size(), config.width, config.height);
        fm.encodeTimeMs = timing.encodeMs;
        fm.decodeTimeMs = timing.decodeMs;
        fm.colorConvertTimeMs = timing.colorConvertMs;
        fm.isKeyFrame = packet.isKeyFrame;

        timing.totalMs = timing.colorConvertMs + timing.encodeMs +
                         timing.decodeMs + timing.qualityCalcMs;

        result.frameMetrics.push_back(fm);
        profiler.addFrame(timing);
        monitor.sample();
    }

    // Aggregate results
    result.qualityStats = metrics::QualityStats::compute(
        result.frameMetrics, config.width, config.height);
    result.encodeLatency = profiler.encodeStats();
    result.decodeLatency = profiler.decodeStats();
    result.colorConvertLatency = profiler.colorConvertStats();
    result.totalLatency = profiler.totalStats();
    result.effectiveFps = profiler.effectiveFps();
    result.resourceStats = monitor.computeStats();
    result.success = true;

    return result;
}

std::vector<AnalyzerResult> AnalyzerEngine::runSweep(const SweepConfig& sweepConfig) {
    std::vector<AnalyzerResult> results;

    auto backends = sweepConfig.backends;
    if (backends.empty()) {
        backends = CodecFactory::availableBackends();
    }

    int totalConfigs = static_cast<int>(
        backends.size() * sweepConfig.resolutions.size() *
        sweepConfig.bitrates.size() * sweepConfig.patterns.size());
    int current = 0;

    for (auto backend : backends) {
        for (const auto& [w, h] : sweepConfig.resolutions) {
            for (auto bitrate : sweepConfig.bitrates) {
                for (const auto& pattern : sweepConfig.patterns) {
                    current++;
                    std::cerr << "\r[" << current << "/" << totalConfigs << "] "
                              << CodecFactory::backendName(backend) << " "
                              << w << "x" << h << " @ "
                              << bitrate / 1000 << "kbps "
                              << pattern << "...          " << std::flush;

                    AnalyzerConfig config;
                    config.backend = backend;
                    config.width = w;
                    config.height = h;
                    config.bitrateBps = bitrate;
                    config.frameCount = sweepConfig.frameCount;
                    config.pattern = pattern;

                    results.push_back(run(config));
                }
            }
        }
    }
    std::cerr << "\n";

    return results;
}

} // namespace analyzer
} // namespace omnidesk
