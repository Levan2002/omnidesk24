#include "ui/app.h"
#include "signaling/signaling_server.h"
#include "core/logger.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#ifdef _WIN32
// Returns true if a Windows Firewall rule named "OmniDesk24" already exists
// for the current executable path. No elevation required to query.
static bool firewallRuleExists(const char* exePath) {
    char args[MAX_PATH + 128];
    snprintf(args, sizeof(args),
        "/C netsh advfirewall firewall show rule name=\"OmniDesk24\" dir=in >nul 2>&1");
    SHELLEXECUTEINFOA sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = "open";   // no elevation needed to query
    sei.lpFile       = "cmd.exe";
    sei.lpParameters = args;
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExA(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, 3000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0; // netsh exits 0 when the rule is found
}

// Register inbound + outbound allow rules in ONE elevated UAC prompt.
// Only called when the rules don't already exist (first run / new install).
static void registerWindowsFirewallRule() {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return;

    // Skip if rules already registered for this executable
    if (firewallRuleExists(exePath)) return;

    // Build a single cmd /C "..." that adds both rules in one elevation prompt
    char args[MAX_PATH * 2 + 512];
    snprintf(args, sizeof(args),
        "/C netsh advfirewall firewall add rule name=\"OmniDesk24\" dir=in "
        "action=allow program=\"%s\" enable=yes profile=any "
        "& netsh advfirewall firewall add rule name=\"OmniDesk24\" dir=out "
        "action=allow program=\"%s\" enable=yes profile=any",
        exePath, exePath);

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = "runas";  // request elevation — ONE prompt for both rules
    sei.lpFile       = "cmd.exe";
    sei.lpParameters = args;
    sei.nShow        = SW_HIDE;
    if (ShellExecuteExA(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000);
        CloseHandle(sei.hProcess);
    }
}
#endif

static void printUsage(const char* argv0) {
    fprintf(stderr, "Usage: %s [options]\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --server              Run as signaling server only\n");
    fprintf(stderr, "  --signal-host HOST    Relay server address (default: omnidesk24.com)\n");
    fprintf(stderr, "  --signal-port PORT    Relay server primary port (default: 9800)\n");
    fprintf(stderr, "  --signal-fallback P   Additional port to try if primary fails (repeatable)\n");
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
        } else if (strcmp(argv[i], "--signal-fallback") == 0 && i + 1 < argc) {
            config.signalingFallbackPorts.push_back(
                static_cast<uint16_t>(std::stoi(argv[++i])));
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            config.encoder.targetBitrateBps = static_cast<uint32_t>(std::stoi(argv[++i])) * 1000;
        } else if (strcmp(argv[i], "--max-fps") == 0 && i + 1 < argc) {
            config.encoder.maxFps = static_cast<float>(std::stoi(argv[++i]));
        } else if (strcmp(argv[i], "--monitor") == 0 && i + 1 < argc) {
            config.capture.monitorId = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--debug") == 0) {
            omnidesk::Logger::instance().setLevel(omnidesk::LogLevel::DBG);
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
        // Server mode: keep the console so log output is visible in the terminal.
        LOG_INFO("Running in signaling server mode on port %d", config.signalingPort);
        LOG_INFO("Use the dedicated omnidesk24-server binary for production deployment.");
        omnidesk::SignalingServer server;
        if (!server.start(config.signalingPort)) {
            LOG_ERROR("Failed to start signaling server on port %d", config.signalingPort);
            return 1;
        }
        LOG_INFO("Signaling server running. Press Ctrl+C to stop.");
        // Block until the server stops
        while (server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }

    // Normal GUI mode: detach from any console the process may have inherited
    // (e.g. when launched from cmd.exe or PowerShell). Logs go to the log file
    // at %APPDATA%\omnidesk24\omnidesk24.log instead.
#ifdef _WIN32
    // Register firewall rules so Windows allows our outbound connections.
    // This shows the standard UAC prompt on the very first run.
    registerWindowsFirewallRule();
    FreeConsole();
#endif

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
