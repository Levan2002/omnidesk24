#include "analyzer/analyzer_engine.h"
#include "analyzer/report_generator.h"
#include "codec/codec_factory.h"
#include "content/generate_test_frames.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace omnidesk;
using namespace omnidesk::analyzer;

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --backend <name>       Encoder backend (openh264, nvenc, vaapi, mf)\n"
              << "  --resolution <WxH>     Frame resolution (default: 1920x1080)\n"
              << "  --bitrate <bps>        Target bitrate in bps (default: 4000000)\n"
              << "  --fps <rate>           Target FPS (default: 30)\n"
              << "  --frames <count>       Number of frames (default: 60)\n"
              << "  --pattern <name>       Test pattern (default: text_document)\n"
              << "  --report <format>      Output format: console, json (default: console)\n"
              << "  --sweep                Run all backend/resolution/bitrate/pattern combos\n"
              << "  --compare              Compare all available backends at current settings\n"
              << "  --list-backends        List available codec backends\n"
              << "  --list-patterns        List available test patterns\n"
              << "  --help                 Show this help\n\n"
              << "Examples:\n"
              << "  " << prog << " --backend openh264 --resolution 1920x1080 --bitrate 4000000\n"
              << "  " << prog << " --sweep --report json > report.json\n"
              << "  " << prog << " --compare --pattern code_editor\n";
}

static CodecBackend parseBackend(const std::string& name) {
    if (name == "openh264" || name == "OpenH264") return CodecBackend::OpenH264;
    if (name == "nvenc" || name == "NVENC") return CodecBackend::NVENC;
    if (name == "vaapi" || name == "VA-API" || name == "va-api") return CodecBackend::VAAPI;
    if (name == "mf" || name == "MF" || name == "media_foundation") return CodecBackend::MF;
    std::cerr << "Unknown backend: " << name << "\n";
    std::exit(1);
    return CodecBackend::OpenH264;
}

static bool parseResolution(const std::string& s, int& w, int& h) {
    auto pos = s.find('x');
    if (pos == std::string::npos) pos = s.find('X');
    if (pos == std::string::npos) return false;
    w = std::atoi(s.substr(0, pos).c_str());
    h = std::atoi(s.substr(pos + 1).c_str());
    return w > 0 && h > 0 && w % 2 == 0 && h % 2 == 0;
}

int main(int argc, char* argv[]) {
    AnalyzerConfig config;
    ReportFormat reportFormat = ReportFormat::Console;
    bool sweep = false;
    bool compare = false;
    bool backendSet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--list-backends") {
            auto backends = CodecFactory::availableBackends();
            std::cout << "Available backends:\n";
            for (auto b : backends) {
                std::cout << "  " << CodecFactory::backendName(b) << "\n";
            }
            return 0;
        } else if (arg == "--list-patterns") {
            std::cout << "Available test patterns:\n";
            for (int j = 0; j < test_content::kPatternCount; ++j) {
                std::cout << "  " << test_content::kPatternNames[j] << "\n";
            }
            return 0;
        } else if (arg == "--sweep") {
            sweep = true;
        } else if (arg == "--compare") {
            compare = true;
        } else if (arg == "--backend" && i + 1 < argc) {
            config.backend = parseBackend(argv[++i]);
            backendSet = true;
        } else if (arg == "--resolution" && i + 1 < argc) {
            if (!parseResolution(argv[++i], config.width, config.height)) {
                std::cerr << "Invalid resolution: " << argv[i]
                          << " (must be WxH with even dimensions)\n";
                return 1;
            }
        } else if (arg == "--bitrate" && i + 1 < argc) {
            config.bitrateBps = static_cast<uint32_t>(std::atol(argv[++i]));
        } else if (arg == "--fps" && i + 1 < argc) {
            config.fps = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--frames" && i + 1 < argc) {
            config.frameCount = std::atoi(argv[++i]);
        } else if (arg == "--pattern" && i + 1 < argc) {
            config.pattern = argv[++i];
        } else if (arg == "--report" && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "json") reportFormat = ReportFormat::JSON;
            else if (fmt == "console") reportFormat = ReportFormat::Console;
            else {
                std::cerr << "Unknown format: " << fmt << "\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    AnalyzerEngine engine;

    if (sweep) {
        AnalyzerEngine::SweepConfig sweepConfig;
        auto results = engine.runSweep(sweepConfig);
        ReportGenerator::printComparison(std::cout, results, reportFormat);
    } else if (compare) {
        auto backends = CodecFactory::availableBackends();
        std::vector<AnalyzerResult> results;
        for (auto backend : backends) {
            config.backend = backend;
            std::cerr << "Testing " << CodecFactory::backendName(backend) << "...\n";
            results.push_back(engine.run(config));
        }
        ReportGenerator::printComparison(std::cout, results, reportFormat);
    } else {
        if (!backendSet) {
            // Auto-select best backend
            auto backends = CodecFactory::availableBackends();
            if (!backends.empty()) {
                config.backend = backends.front();
            }
        }
        auto result = engine.run(config);
        ReportGenerator::printSingle(std::cout, result, reportFormat);
    }

    return 0;
}
