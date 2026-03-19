#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace omnidesk {
namespace analyzer {

struct ResourceSample {
    double cpuUserMs = 0.0;
    double cpuSystemMs = 0.0;
    size_t memoryRssKb = 0;
    double timestampMs = 0.0;
};

struct ResourceStats {
    double avgCpuPercent = 0.0;
    double peakCpuPercent = 0.0;
    size_t peakMemoryRssKb = 0;
    size_t avgMemoryRssKb = 0;
    double totalCpuUserMs = 0.0;
    double totalCpuSystemMs = 0.0;
};

// Monitors CPU and memory usage of the current process via /proc/self.
class ResourceMonitor {
public:
    ResourceMonitor();
    ~ResourceMonitor() = default;

    // Take a snapshot of current resource usage.
    void sample();

    // Reset all collected samples.
    void reset();

    // Get all collected samples.
    const std::vector<ResourceSample>& samples() const { return samples_; }

    // Compute aggregate statistics from collected samples.
    ResourceStats computeStats() const;

    // Read current RSS memory in KB.
    static size_t currentRssKb();

private:
    struct ProcTimes {
        double userMs = 0.0;
        double systemMs = 0.0;
    };

    static ProcTimes readProcTimes();

    std::vector<ResourceSample> samples_;
    std::chrono::high_resolution_clock::time_point startTime_;
};

} // namespace analyzer
} // namespace omnidesk
