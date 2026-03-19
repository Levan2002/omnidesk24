#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace omnidesk {
namespace analyzer {

struct LatencyStats {
    double minMs = 0.0;
    double maxMs = 0.0;
    double avgMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double stddevMs = 0.0;
    int sampleCount = 0;
};

// Per-frame timing breakdown for the encode-decode pipeline.
struct FrameTiming {
    double colorConvertMs = 0.0;
    double encodeMs = 0.0;
    double decodeMs = 0.0;
    double qualityCalcMs = 0.0;
    double totalMs = 0.0;  // end-to-end for this frame
};

// Collects per-frame timing data and computes statistical summaries.
class LatencyProfiler {
public:
    LatencyProfiler() = default;
    ~LatencyProfiler() = default;

    void addFrame(const FrameTiming& timing) { timings_.push_back(timing); }
    void reset() { timings_.clear(); }

    const std::vector<FrameTiming>& timings() const { return timings_; }

    LatencyStats encodeStats() const { return computeStats(extractField(&FrameTiming::encodeMs)); }
    LatencyStats decodeStats() const { return computeStats(extractField(&FrameTiming::decodeMs)); }
    LatencyStats colorConvertStats() const { return computeStats(extractField(&FrameTiming::colorConvertMs)); }
    LatencyStats totalStats() const { return computeStats(extractField(&FrameTiming::totalMs)); }

    // Effective FPS based on total processing time
    double effectiveFps() const {
        if (timings_.empty()) return 0.0;
        double totalMs = 0.0;
        for (const auto& t : timings_) totalMs += t.totalMs;
        if (totalMs < 1e-6) return 0.0;
        return timings_.size() * 1000.0 / totalMs;
    }

    // Helper for timing sections
    class ScopedTimer {
    public:
        ScopedTimer(double& target)
            : target_(target)
            , start_(std::chrono::high_resolution_clock::now()) {}

        ~ScopedTimer() {
            auto end = std::chrono::high_resolution_clock::now();
            target_ = std::chrono::duration<double, std::milli>(end - start_).count();
        }

    private:
        double& target_;
        std::chrono::high_resolution_clock::time_point start_;
    };

private:
    std::vector<double> extractField(double FrameTiming::*field) const {
        std::vector<double> values;
        values.reserve(timings_.size());
        for (const auto& t : timings_) values.push_back(t.*field);
        return values;
    }

    static LatencyStats computeStats(std::vector<double> values) {
        LatencyStats s;
        if (values.empty()) return s;

        s.sampleCount = static_cast<int>(values.size());
        std::sort(values.begin(), values.end());

        s.minMs = values.front();
        s.maxMs = values.back();

        double sum = 0;
        for (double v : values) sum += v;
        s.avgMs = sum / values.size();

        s.p50Ms = percentile(values, 0.50);
        s.p95Ms = percentile(values, 0.95);
        s.p99Ms = percentile(values, 0.99);

        double variance = 0;
        for (double v : values) {
            double d = v - s.avgMs;
            variance += d * d;
        }
        s.stddevMs = std::sqrt(variance / values.size());

        return s;
    }

    static double percentile(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0.0;
        double idx = p * (sorted.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - lo;
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

    std::vector<FrameTiming> timings_;
};

} // namespace analyzer
} // namespace omnidesk
