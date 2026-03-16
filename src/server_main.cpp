// OmniDesk24 Standalone Signaling + Relay Server
// Runs without GUI dependencies (no GLFW, ImGui, OpenGL).
// Usage: omnidesk24-server [--port PORT] [--db CONNSTR] [--debug]

#include "signaling/signaling_server.h"
#include "signaling/database.h"
#include "core/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

static void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  --port PORT       Listen port (default: 9800)\n"
              << "  --db CONNSTR      PostgreSQL connection string\n"
              << "                    (default: dbname=omnidesk24 user=omnidesk)\n"
              << "  --debug           Enable debug logging\n"
              << "  --help            Show this help\n";
}

int main(int argc, char* argv[]) {
    uint16_t port = 9800;
    std::string dbConnStr = "dbname=omnidesk24 user=omnidesk";
    bool debug = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            dbConnStr = argv[++i];
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (debug) {
        omnidesk::Logger::instance().setLevel(omnidesk::LogLevel::DBG);
    }

    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    LOG_INFO("OmniDesk24 Signaling Server v0.1.0");
    LOG_INFO("Port: %d", port);

    // Connect to PostgreSQL
    omnidesk::Database db;
    bool dbOk = db.connect(dbConnStr);
    if (dbOk) {
        if (!db.initSchema()) {
            LOG_WARN("DB: schema init failed, continuing without persistence");
            db.disconnect();
            dbOk = false;
        } else {
            LOG_INFO("DB: PostgreSQL connected and schema ready");
        }
    } else {
        LOG_WARN("DB: PostgreSQL not available, running in memory-only mode");
    }

    // Start the signaling server
    omnidesk::SignalingServer server;
    if (!server.start(port)) {
        LOG_ERROR("Failed to start signaling server on port %d", port);
        return 1;
    }

    LOG_INFO("Signaling server running on port %d", server.port());
    LOG_INFO("Press Ctrl+C to stop");

    // Main loop: print stats periodically, persist user data
    auto lastStats = std::chrono::steady_clock::now();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastStats).count();

        // Print stats every 60 seconds
        if (elapsed >= 60) {
            size_t users = server.userCount();
            LOG_INFO("Stats: %zu online users", users);

            if (dbOk) {
                LOG_INFO("DB Stats: %lld total sessions, %lld active, %lld registered users",
                         static_cast<long long>(db.totalSessions()),
                         static_cast<long long>(db.activeSessions()),
                         static_cast<long long>(db.totalRegisteredUsers()));
            }
            lastStats = now;
        }
    }

    LOG_INFO("Shutting down...");
    server.stop();

    if (dbOk) {
        db.logEvent("server_stop", "", "graceful shutdown");
        db.disconnect();
    }

    LOG_INFO("Server stopped");
    return 0;
}
