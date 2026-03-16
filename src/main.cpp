#include "ui/app.h"
#include "core/logger.h"

#include <cstring>
#include <string>

static void printUsage(const char* argv0) {
    fprintf(stderr, "Usage: %s [options]\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --server              Run as signaling server only\n");
    fprintf(stderr, "  --signal-host HOST    Signaling server address (default: 127.0.0.1)\n");
    fprintf(stderr, "  --signal-port PORT    Signaling server port (default: 9800)\n");
    fprintf(stderr, "  --bitrate KBPS        Target bitrate in kbps (default: 2000)\n");
    fprintf(stderr, "  --max-fps FPS         Maximum frame rate (default: 60)\n");
    fprintf(stderr, "  --monitor ID          Monitor to capture (-1 = primary, default: -1)\n");
    fprintf(stderr, "  --debug               Enable debug logging\n");
    fprintf(stderr, "  --help                Show this help\n");
}

int main(int argc, char* argv[]) {
    omnidesk::AppConfig config;
    bool serverMode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0) {
            serverMode = true;
        } else if (strcmp(argv[i], "--signal-host") == 0 && i + 1 < argc) {
            config.signalingHost = argv[++i];
        } else if (strcmp(argv[i], "--signal-port") == 0 && i + 1 < argc) {
            config.signalingPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            config.encoder.targetBitrateBps = static_cast<uint32_t>(std::stoi(argv[++i])) * 1000;
        } else if (strcmp(argv[i], "--max-fps") == 0 && i + 1 < argc) {
            config.encoder.maxFps = static_cast<float>(std::stoi(argv[++i]));
        } else if (strcmp(argv[i], "--monitor") == 0 && i + 1 < argc) {
            config.capture.monitorId = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--debug") == 0) {
            omnidesk::Logger::instance().setLevel(omnidesk::LogLevel::DEBUG);
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    LOG_INFO("OmniDesk24 v%s starting...", "0.1.0");

    if (serverMode) {
        LOG_INFO("Running in signaling server mode on port %d", config.signalingPort);
        // TODO: Run signaling server standalone
        // SignalingServer server;
        // server.start(config.signalingPort);
        // server.run(); // blocks
        LOG_ERROR("Standalone signaling server not yet implemented");
        return 1;
    }

    // Normal mode: launch GUI app
    omnidesk::App app;
    if (!app.init(config)) {
        LOG_ERROR("Failed to initialize application");
        return 1;
    }

    app.run();
    app.shutdown();

    LOG_INFO("OmniDesk24 exiting");
    return 0;
}
