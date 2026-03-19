#pragma once

#include "core/types.h"
#include "webrtc/webrtc_session.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace omnidesk {

class SignalingClient;
class HostSession;
class ViewerSession;
class InputHandler;
class InputInjector;
// D2D objects are module-level statics in app.cpp

enum class AppState {
    DASHBOARD,
    CONNECTING,
    SESSION_HOST,
    SESSION_VIEWER,
};

struct AppConfig {
    std::string signalingHost = "signal.omnidesk24.com";
    uint16_t    signalingPort = 9800;
    std::vector<uint16_t> signalingFallbackPorts = {8443, 443};
    EncoderConfig encoder;
    CaptureConfig capture;
    std::string turnServer;
    std::string turnUser;
    std::string turnPassword;
    int windowWidth = 1280;
    int windowHeight = 800;
};

class App {
public:
    App();
    ~App();

    bool init(const AppConfig& config);
    void run();   // Win32 message loop
    void shutdown();

    AppState state() const { return state_; }

    // Win32 message handler (called from WndProc)
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    // Initialization
    bool createMainWindow();
    bool createGlChildWindow();
    void initD2D();
    void initSignaling();

    // Rendering
    void paint();           // called on WM_PAINT or when dirty
    void invalidate();      // marks window for repaint

    // State management
    void handleConnectionRequest(const UserID& fromUser);
    void connectToPeer(const std::string& peerId);
    void disconnectSession();
    void queueAction(std::function<void()> action);
    void drainPendingActions();
    void tryConnectSignaling();
    void startReconnectThread();

    // GL child window management
    void showGlWindow();
    void hideGlWindow();

    // Window handles
    HWND mainHwnd_ = nullptr;
    HWND glHwnd_ = nullptr;       // OpenGL child window
    HDC  glDC_ = nullptr;
    HGLRC glRC_ = nullptr;

    // D2D: objects are module-level statics in app.cpp

    // App state
    AppConfig config_;
    AppState state_ = AppState::DASHBOARD;
    AppState prevState_ = AppState::DASHBOARD;
    UserID myId_;

    // UI state
    std::string connectIdInput_;
    bool showSettings_ = false;
    bool showStats_ = false;
    bool showConnectionDialog_ = false;
    UserID pendingConnectionFrom_;
    std::string statusMessage_;
    float statusMessageTimer_ = 0.0f;
    float connectTimeoutSec_ = 0.0f;
    bool connectInputFocused_ = false;

    // Session management
    std::unique_ptr<SignalingClient> signaling_;
    std::unique_ptr<HostSession> hostSession_;
    std::unique_ptr<ViewerSession> viewerSession_;
    std::unique_ptr<WebRtcSession> webrtcSession_;
    std::unique_ptr<InputHandler> inputHandler_;
    std::unique_ptr<InputInjector> inputInjector_;

    // Thread-safe action queue
    std::vector<std::function<void()>> pendingActions_;
    std::mutex pendingActionsMutex_;
    HANDLE wakeEvent_ = nullptr;  // signals MsgWaitForMultipleObjects

    // Background reconnect
    std::thread reconnectThread_;
    std::atomic<bool> appRunning_{false};

    // Timing
    LARGE_INTEGER perfFreq_{};
    LARGE_INTEGER lastFrame_{};

    // Mouse state for hover effects
    float mouseX_ = 0, mouseY_ = 0;
    // Cached button rects for hit testing and hover
    struct FRect { float x, y, w, h; };
    FRect rCopyBtn_{}, rConnectBtn_{}, rSettingsBtn_{}, rConnectInput_{};
    // Custom title bar
    static constexpr float kTitleBarH = 32.0f;
    int hoveredTitleBtn_ = 0; // 0=none, 1=min, 2=max, 3=close
    bool isMaximized_ = false;
    RECT restoreRect_ = {100, 100, 1380, 900}; // saved window rect before maximize
};

} // namespace omnidesk
