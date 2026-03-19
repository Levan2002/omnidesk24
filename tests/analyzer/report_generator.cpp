#include "analyzer/report_generator.h"
#include <iomanip>
#include <sstream>

namespace omnidesk {
namespace analyzer {

void ReportGenerator::printSingle(std::ostream& out, const AnalyzerResult& result,
                                   ReportFormat format) {
    if (format == ReportFormat::JSON) {
        printJSON(out, result);
    } else {
        printConsole(out, result);
    }
}

void ReportGenerator::printComparison(std::ostream& out,
                                       const std::vector<AnalyzerResult>& results,
                                       ReportFormat format) {
    if (format == ReportFormat::JSON) {
        printComparisonJSON(out, results);
    } else {
        printComparisonConsole(out, results);
    }
}

void ReportGenerator::printLatencyStats(std::ostream& out, const char* name,
                                         const LatencyStats& stats) {
    out << "  " << std::left << std::setw(16) << name
        << std::right << std::fixed << std::setprecision(2)
        << std::setw(8) << stats.avgMs
        << std::setw(8) << stats.p50Ms
        << std::setw(8) << stats.p95Ms
        << std::setw(8) << stats.p99Ms
        << std::setw(8) << stats.minMs
        << std::setw(8) << stats.maxMs
        << "\n";
}

void ReportGenerator::printConsole(std::ostream& out, const AnalyzerResult& result) {
    out << "\n";
    out << "================================================================\n";
    out << "  Codec Analyzer Report\n";
    out << "================================================================\n";

    if (!result.success) {
        out << "  ERROR: " << result.errorMessage << "\n";
        out << "================================================================\n\n";
        return;
    }

    // Configuration
    out << "\n  Configuration:\n";
    out << "    Backend:     " << result.backendName << "\n";
    out << "    Resolution:  " << result.config.width << "x" << result.config.height << "\n";
    out << "    Bitrate:     " << result.config.bitrateBps / 1000 << " kbps\n";
    out << "    FPS:         " << result.config.fps << "\n";
    out << "    Frames:      " << result.config.frameCount << "\n";
    out << "    Pattern:     " << result.config.pattern << "\n";

    // Quality
    out << "\n  Quality Metrics:\n";
    out << "    Avg PSNR:    " << std::fixed << std::setprecision(2)
        << result.qualityStats.avgPsnr << " dB\n";
    out << "    Min PSNR:    " << result.qualityStats.minPsnr << " dB\n";
    out << "    Max PSNR:    " << result.qualityStats.maxPsnr << " dB\n";
    out << "    Stddev PSNR: " << result.qualityStats.stddevPsnr << " dB\n";
    out << "    Avg SSIM:    " << std::setprecision(4) << result.qualityStats.avgSsim << "\n";
    out << "    Avg BPP:     " << std::setprecision(4) << result.qualityStats.avgBpp << "\n";
    out << "    Total size:  " << std::setprecision(1)
        << result.qualityStats.totalEncodedBytes / 1024.0 << " KB\n";
    out << "    Comp ratio:  " << std::setprecision(1)
        << result.qualityStats.avgCompressionRatio << ":1\n";

    // Latency
    out << "\n  Latency (ms):       avg     p50     p95     p99     min     max\n";
    out << "  " << std::string(72, '-') << "\n";
    printLatencyStats(out, "Color Convert", result.colorConvertLatency);
    printLatencyStats(out, "Encode", result.encodeLatency);
    printLatencyStats(out, "Decode", result.decodeLatency);
    printLatencyStats(out, "Total", result.totalLatency);
    out << "\n    Effective FPS: " << std::setprecision(1) << result.effectiveFps << "\n";

    // Resources
    out << "\n  Resource Usage:\n";
    out << "    Avg CPU:     " << std::setprecision(1)
        << result.resourceStats.avgCpuPercent << "%\n";
    out << "    Peak CPU:    " << result.resourceStats.peakCpuPercent << "%\n";
    out << "    Peak Memory: " << result.resourceStats.peakMemoryRssKb / 1024 << " MB\n";
    out << "    Avg Memory:  " << result.resourceStats.avgMemoryRssKb / 1024 << " MB\n";
    out << "    CPU User:    " << std::setprecision(1)
        << result.resourceStats.totalCpuUserMs << " ms\n";
    out << "    CPU System:  " << result.resourceStats.totalCpuSystemMs << " ms\n";

    out << "\n================================================================\n\n";
}

std::string ReportGenerator::escapeJSON(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

void ReportGenerator::printJSON(std::ostream& out, const AnalyzerResult& result) {
    out << std::fixed;
    out << "{\n";
    out << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    if (!result.success) {
        out << "  \"error\": \"" << escapeJSON(result.errorMessage) << "\"\n";
        out << "}\n";
        return;
    }

    out << "  \"config\": {\n";
    out << "    \"backend\": \"" << result.backendName << "\",\n";
    out << "    \"width\": " << result.config.width << ",\n";
    out << "    \"height\": " << result.config.height << ",\n";
    out << "    \"bitrateBps\": " << result.config.bitrateBps << ",\n";
    out << "    \"fps\": " << std::setprecision(1) << result.config.fps << ",\n";
    out << "    \"frames\": " << result.config.frameCount << ",\n";
    out << "    \"pattern\": \"" << escapeJSON(result.config.pattern) << "\"\n";
    out << "  },\n";

    out << "  \"quality\": {\n";
    out << "    \"avgPsnr\": " << std::setprecision(2) << result.qualityStats.avgPsnr << ",\n";
    out << "    \"minPsnr\": " << result.qualityStats.minPsnr << ",\n";
    out << "    \"maxPsnr\": " << result.qualityStats.maxPsnr << ",\n";
    out << "    \"stddevPsnr\": " << result.qualityStats.stddevPsnr << ",\n";
    out << "    \"avgSsim\": " << std::setprecision(4) << result.qualityStats.avgSsim << ",\n";
    out << "    \"avgBpp\": " << result.qualityStats.avgBpp << ",\n";
    out << "    \"totalEncodedBytes\": " << result.qualityStats.totalEncodedBytes << ",\n";
    out << "    \"avgCompressionRatio\": " << std::setprecision(1)
        << result.qualityStats.avgCompressionRatio << "\n";
    out << "  },\n";

    auto writeLatency = [&](const char* name, const LatencyStats& s) {
        out << "  \"" << name << "\": {\n";
        out << "    \"avgMs\": " << std::setprecision(3) << s.avgMs << ",\n";
        out << "    \"p50Ms\": " << s.p50Ms << ",\n";
        out << "    \"p95Ms\": " << s.p95Ms << ",\n";
        out << "    \"p99Ms\": " << s.p99Ms << ",\n";
        out << "    \"minMs\": " << s.minMs << ",\n";
        out << "    \"maxMs\": " << s.maxMs << "\n";
        out << "  }";
    };

    writeLatency("encodeLatency", result.encodeLatency);
    out << ",\n";
    writeLatency("decodeLatency", result.decodeLatency);
    out << ",\n";
    writeLatency("colorConvertLatency", result.colorConvertLatency);
    out << ",\n";
    writeLatency("totalLatency", result.totalLatency);
    out << ",\n";

    out << "  \"effectiveFps\": " << std::setprecision(1) << result.effectiveFps << ",\n";

    out << "  \"resources\": {\n";
    out << "    \"avgCpuPercent\": " << std::setprecision(1)
        << result.resourceStats.avgCpuPercent << ",\n";
    out << "    \"peakCpuPercent\": " << result.resourceStats.peakCpuPercent << ",\n";
    out << "    \"peakMemoryMb\": " << result.resourceStats.peakMemoryRssKb / 1024 << ",\n";
    out << "    \"avgMemoryMb\": " << result.resourceStats.avgMemoryRssKb / 1024 << ",\n";
    out << "    \"totalCpuUserMs\": " << result.resourceStats.totalCpuUserMs << ",\n";
    out << "    \"totalCpuSystemMs\": " << result.resourceStats.totalCpuSystemMs << "\n";
    out << "  },\n";

    // Per-frame data
    out << "  \"frames\": [\n";
    for (size_t i = 0; i < result.frameMetrics.size(); ++i) {
        const auto& f = result.frameMetrics[i];
        out << "    {";
        out << "\"id\": " << f.frameId << ", ";
        out << "\"psnr\": " << std::setprecision(2) << f.psnr << ", ";
        out << "\"ssim\": " << std::setprecision(4) << f.ssim << ", ";
        out << "\"bytes\": " << f.encodedBytes << ", ";
        out << "\"bpp\": " << f.bpp << ", ";
        out << "\"encodeMs\": " << std::setprecision(3) << f.encodeTimeMs << ", ";
        out << "\"decodeMs\": " << f.decodeTimeMs << ", ";
        out << "\"keyFrame\": " << (f.isKeyFrame ? "true" : "false");
        out << "}" << (i + 1 < result.frameMetrics.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void ReportGenerator::printComparisonConsole(std::ostream& out,
                                              const std::vector<AnalyzerResult>& results) {
    out << "\n";
    out << "================================================================================\n";
    out << "  Codec Analyzer Comparison (" << results.size() << " configurations)\n";
    out << "================================================================================\n\n";

    // Header
    out << std::left << std::setw(12) << "Backend"
        << std::setw(12) << "Resolution"
        << std::right << std::setw(8) << "Kbps"
        << std::setw(14) << "Pattern"
        << std::setw(8) << "PSNR"
        << std::setw(8) << "SSIM"
        << std::setw(8) << "Enc ms"
        << std::setw(8) << "Dec ms"
        << std::setw(8) << "FPS"
        << std::setw(8) << "CPU%"
        << std::setw(8) << "Mem MB"
        << "\n";
    out << std::string(104, '-') << "\n";

    for (const auto& r : results) {
        if (!r.success) {
            out << std::left << std::setw(12) << r.backendName
                << "  ERROR: " << r.errorMessage << "\n";
            continue;
        }

        std::string res = std::to_string(r.config.width) + "x" + std::to_string(r.config.height);
        out << std::left << std::setw(12) << r.backendName
            << std::setw(12) << res
            << std::right << std::setw(8) << r.config.bitrateBps / 1000
            << std::setw(14) << r.config.pattern
            << std::fixed
            << std::setw(8) << std::setprecision(1) << r.qualityStats.avgPsnr
            << std::setw(8) << std::setprecision(4) << r.qualityStats.avgSsim
            << std::setw(8) << std::setprecision(2) << r.encodeLatency.avgMs
            << std::setw(8) << std::setprecision(2) << r.decodeLatency.avgMs
            << std::setw(8) << std::setprecision(1) << r.effectiveFps
            << std::setw(8) << std::setprecision(1) << r.resourceStats.avgCpuPercent
            << std::setw(8) << r.resourceStats.peakMemoryRssKb / 1024
            << "\n";
    }

    out << std::string(104, '-') << "\n\n";
}

void ReportGenerator::printComparisonJSON(std::ostream& out,
                                           const std::vector<AnalyzerResult>& results) {
    out << "[\n";
    for (size_t i = 0; i < results.size(); ++i) {
        printJSON(out, results[i]);
        if (i + 1 < results.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
}

} // namespace analyzer
} // namespace omnidesk
