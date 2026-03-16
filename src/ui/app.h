#pragma once

#include "core/types.h"
#include <functional>
#include <memory>
#include <string>

struct GLFWwindow;

namespace omnidesk {

// Forward declarations
class SignalingClient;
class HostSession;
class ViewerSession;

enum class AppState {
    DASHBOARD,      // Main screen: show ID, connect field
    CONNECTING,     // Waiting for host to accept
    SESSION_HOST,   // Hosting a remote desktop session
    SESSION_VIEWER, // Viewing a remote desktop
};

struct AppConfig {
    std::string signalingHost = "127.0.0.1";
    uint16_t signalingPort = 9800;
    EncoderConfig encoder;
    CaptureConfig capture;
    int windowWidth = 1280;
    int windowHeight = 800;
};

class App {
public:
    App();
    ~App();

    bool init(const AppConfig& config);
    void run();  // Main loop
    void shutdown();

    AppState state() const { return state_; }

private:
    void initImGui();
    void renderFrame();
    void renderDashboard();
    void renderConnecting();
    void renderSession();
    void handleConnectionRequest(const UserID& fromUser);
    void connectToPeer(const std::string& peerId);
    void disconnectSession();

    GLFWwindow* window_ = nullptr;
    AppConfig config_;
    AppState state_ = AppState::DASHBOARD;
    UserID myId_;

    // UI state
    char connectIdInput_[16] = {};
    bool showSettings_ = false;
    bool showStats_ = false;
    bool showConnectionDialog_ = false;
    UserID pendingConnectionFrom_;
    std::string statusMessage_;
    float statusMessageTimer_ = 0;

    // Session management
    std::unique_ptr<SignalingClient> signaling_;
    std::unique_ptr<HostSession> hostSession_;
    std::unique_ptr<ViewerSession> viewerSession_;
};

} // namespace omnidesk
