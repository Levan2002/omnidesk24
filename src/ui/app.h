#pragma once

#include "core/types.h"
#include "transport/tcp_channel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    std::string signalingHost = "omnidesk24.com";
    uint16_t    signalingPort = 9800;
    // Fallback ports tried in order when signalingPort is unreachable.
    // 8443: nginx TCP-proxy (HTTPS-alternate). 443: port used by HTTPS,
    // almost never blocked even on corporate/school networks.
    std::vector<uint16_t> signalingFallbackPorts = {8443, 443};
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

    // Fixed port for P2P data channel
    static constexpr uint16_t DATA_PORT = 9801;

private:
    void initImGui();
    void renderFrame();
    void renderDashboard();
    void renderConnecting();
    void renderSession();
    void handleConnectionRequest(const UserID& fromUser);
    void connectToPeer(const std::string& peerId);
    void disconnectSession();
    void queueAction(std::function<void()> action);
    void tryConnectSignaling();       // attempt signaling connect + register
    void startReconnectThread();      // background thread for retrying

    // P2P data channel helpers
    static std::string getLocalIp();
    void sendVideoPacket(const EncodedPacket& packet);
    void viewerReceiveLoop();

    GLFWwindow* window_ = nullptr;
    AppConfig config_;
    AppState state_ = AppState::DASHBOARD;
    UserID myId_;
    std::string localIp_;

    // UI state
    char connectIdInput_[16] = {};
    bool showSettings_ = false;
    bool showStats_ = false;
    bool showConnectionDialog_ = false;
    UserID pendingConnectionFrom_;
    std::string statusMessage_;
    float statusMessageTimer_ = 0.0f;
    float connectTimeoutSec_  = 0.0f;   // seconds spent in CONNECTING state

    // Session management
    std::unique_ptr<SignalingClient> signaling_;
    std::unique_ptr<HostSession> hostSession_;
    std::unique_ptr<ViewerSession> viewerSession_;

    // P2P data channel (direct TCP between host and viewer)
    TcpChannel dataListener_;
    std::unique_ptr<TcpChannel> dataChannel_;
    std::mutex dataChannelMutex_;      // protects dataChannel_ across threads
    std::thread dataAcceptThread_;     // host: waits for viewer to connect
    std::thread dataRecvThread_;       // viewer: receives video packets
    std::atomic<bool> dataRunning_{false};

    // Relay mode: when P2P is impossible, send/receive data via signaling server
    std::atomic<bool> relayMode_{false};
    UserID relayPeerId_;               // the remote peer for relay

    // Thread-safe queue for actions posted from the signaling receive thread
    // and dispatched on the main/UI thread each frame.
    std::vector<std::function<void()>> pendingActions_;
    std::mutex pendingActionsMutex_;

    // Background reconnect thread: retries connecting when initial attempt
    // or any subsequent disconnect leaves us without a signaling connection.
    std::thread reconnectThread_;
    std::atomic<bool> appRunning_{false};
};

} // namespace omnidesk
