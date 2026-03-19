#include "analyzer/resource_monitor.h"
#include <algorithm>
#include <cstdio>
#include <numeric>

#ifdef __linux__
#include <unistd.h>
#endif

namespace omnidesk {
namespace analyzer {

ResourceMonitor::ResourceMonitor()
    : startTime_(std::chrono::high_resolution_clock::now()) {}

void ResourceMonitor::sample() {
    ResourceSample s;
    auto now = std::chrono::high_resolution_clock::now();
    s.timestampMs = std::chrono::duration<double, std::milli>(now - startTime_).count();

    auto times = readProcTimes();
    s.cpuUserMs = times.userMs;
    s.cpuSystemMs = times.systemMs;
    s.memoryRssKb = currentRssKb();

    samples_.push_back(s);
}

void ResourceMonitor::reset() {
    samples_.clear();
    startTime_ = std::chrono::high_resolution_clock::now();
}

ResourceStats ResourceMonitor::computeStats() const {
    ResourceStats stats;
    if (samples_.size() < 2) return stats;

    size_t peakMem = 0;
    size_t totalMem = 0;
    double peakCpu = 0.0;
    double totalCpu = 0.0;
    int cpuSamples = 0;

    for (size_t i = 1; i < samples_.size(); ++i) {
        const auto& prev = samples_[i - 1];
        const auto& curr = samples_[i];

        double dtMs = curr.timestampMs - prev.timestampMs;
        if (dtMs > 0) {
            double cpuMs = (curr.cpuUserMs - prev.cpuUserMs) +
                           (curr.cpuSystemMs - prev.cpuSystemMs);
            double cpuPercent = (cpuMs / dtMs) * 100.0;
            peakCpu = std::max(peakCpu, cpuPercent);
            totalCpu += cpuPercent;
            cpuSamples++;
        }

        peakMem = std::max(peakMem, curr.memoryRssKb);
        totalMem += curr.memoryRssKb;
    }

    if (cpuSamples > 0) {
        stats.avgCpuPercent = totalCpu / cpuSamples;
    }
    stats.peakCpuPercent = peakCpu;
    stats.peakMemoryRssKb = peakMem;
    stats.avgMemoryRssKb = (samples_.size() > 1) ? totalMem / (samples_.size() - 1) : 0;

    if (!samples_.empty()) {
        stats.totalCpuUserMs = samples_.back().cpuUserMs - samples_.front().cpuUserMs;
        stats.totalCpuSystemMs = samples_.back().cpuSystemMs - samples_.front().cpuSystemMs;
    }

    return stats;
}

size_t ResourceMonitor::currentRssKb() {
#ifdef __linux__
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;

    size_t rss = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %zu kB", &rss) == 1) break;
    }
    fclose(f);
    return rss;
#else
    return 0;
#endif
}

ResourceMonitor::ProcTimes ResourceMonitor::readProcTimes() {
    ProcTimes times;
#ifdef __linux__
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return times;

    // Fields: pid comm state ppid ... utime stime (fields 14, 15, 1-indexed)
    long utime = 0, stime = 0;
    // Skip to fields 14 and 15
    int field = 0;
    char c;
    bool inParens = false;
    while ((c = fgetc(f)) != EOF) {
        if (c == '(') inParens = true;
        if (c == ')') inParens = false;
        if (!inParens && c == ' ') {
            field++;
            if (field == 13) { // utime is after 13 spaces (field 14)
                if (fscanf(f, "%ld %ld", &utime, &stime) != 2) {
                    utime = stime = 0;
                }
                break;
            }
        }
    }
    fclose(f);

    long ticksPerSec = sysconf(_SC_CLK_TCK);
    if (ticksPerSec > 0) {
        times.userMs = static_cast<double>(utime) * 1000.0 / ticksPerSec;
        times.systemMs = static_cast<double>(stime) * 1000.0 / ticksPerSec;
    }
#endif
    return times;
}

} // namespace analyzer
} // namespace omnidesk
