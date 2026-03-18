#pragma once

#include "core/types.h"
#include "signaling/tcp_channel.h"
#include "signaling/wire_format.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace omnidesk {

// Connection request information received from a remote peer.
struct ConnectionRequest {
    UserID fromId;
    PeerAddress publicAddr;   // Requester's public address
    PeerAddress localAddr;    // Requester's local address
};

// Connection acceptance information.
struct ConnectionAcceptance {
    UserID fromId;
    PeerAddress peerPublicAddr;
    PeerAddress peerLocalAddr;
};

// Connection rejection information.
struct ConnectionRejection {
    UserID fromId;
    std::string reason;
};

// SignalingClient: connects to a signaling server for user registration
// and connection brokering. Sends heartbeats every 30 seconds.
// Handles automatic reconnection on disconnect.
class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Heartbeat interval in seconds.
    static constexpr int HEARTBEAT_INTERVAL_SEC = 30;

    // Reconnection interval in seconds.
    static constexpr int RECONNECT_INTERVAL_SEC = 5;

    // Maximum reconnection attempts (0 = unlimited).
    static constexpr int MAX_RECONNECT_ATTEMPTS = 10;

    // Callbacks
    using ConnectionRequestCallback = std::function<void(const ConnectionRequest&)>;
    using ConnectionAcceptedCallback = std::function<void(const ConnectionAcceptance&)>;
    using ConnectionRejectedCallback = std::function<void(const ConnectionRejection&)>;
    using UserOfflineCallback = std::function<void(const UserID& targetId)>;
    using DisconnectedCallback = std::function<void()>;
    using RegisteredCallback = std::function<void(bool success)>;
    using RelayDataCallback = std::function<void(const UserID& fromId,
                                                  MessageType innerType,
                                                  const std::vector<uint8_t>& payload)>;

    // Connect to the signaling server.
    // Tries each port in fallbackPorts in order until one succeeds.
    // If fallbackPorts is empty, only port is tried.
    bool connect(const std::string& host, uint16_t port,
                 const std::vector<uint16_t>& fallbackPorts = {});

    // Disconnect from the server.
    void disconnect();

    // Register our user ID with the server.
    // localAddr is our local (LAN) address for hole punching.
    bool registerUser(const UserID& userId, const PeerAddress& localAddr = {});

    // Request a connection to a remote user.
    bool requestConnection(const UserID& targetId);

    // Accept an incoming connection request.
    bool acceptConnection(const UserID& fromId);

    // Reject an incoming connection request.
    bool rejectConnection(const UserID& fromId, const std::string& reason = "");

    // Send data to a peer through the signaling server relay.
    // Used when direct P2P connection is not possible.
    bool sendRelayData(const UserID& targetId, MessageType innerType,
                       const void* data, uint32_t length);

    // Check if connected to the server.
    bool isConnected() const;

    // Returns the port that was actually used to connect (0 if not connected).
    uint16_t connectedPort() const { return connectedPort_; }

    // Returns the local IP address used for the signaling TCP connection.
    // More reliable than UDP-based detection (avoids Docker/VPN adapters).
    std::string localAddress() const { return channel_.localAddress(); }

    // Check if registered.
    bool isRegistered() const { return registered_; }

    // Get our registered user ID.
    const UserID& userId() const { return userId_; }

    // Set callbacks.
    void onConnectionRequest(ConnectionRequestCallback cb);
    void onConnectionAccepted(ConnectionAcceptedCallback cb);
    void onConnectionRejected(ConnectionRejectedCallback cb);
    void onUserOffline(UserOfflineCallback cb);
    void onDisconnected(DisconnectedCallback cb);
    void onRegistered(RegisteredCallback cb);
    void onRelayData(RelayDataCallback cb);

    // Poll for pending events (processes callbacks on the calling thread).
    // Call from main/UI thread each frame to dispatch queued callbacks safely.
    void poll();

    // Enable/disable automatic reconnection.
    void setAutoReconnect(bool enabled) { autoReconnect_ = enabled; }

private:
    // Background thread for receiving messages and sending heartbeats.
    void receiveLoop();

    // Process a received JSON message.
    void handleMessage(const std::string& json);

    // Send a JSON message to the server.
    bool sendJson(const std::string& json);

    // Simple JSON helpers.
    static std::string jsonGetString(const std::string& json, const std::string& key);
    static std::string jsonMakeObject(
        const std::vector<std::pair<std::string, std::string>>& fields);

    // Attempt reconnection.
    bool tryReconnect();

    TcpChannel channel_;
    std::string serverHost_;
    uint16_t serverPort_ = 0;        // primary port from config
    uint16_t connectedPort_ = 0;     // actual port used (set on successful connect)
    std::vector<uint16_t> fallbackPorts_;  // ports to try if primary fails
    UserID userId_;
    PeerAddress localAddr_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> registered_{false};
    std::atomic<bool> running_{false};
    bool autoReconnect_ = true;

    std::thread receiveThread_;
    mutable std::mutex sendMutex_;

    // Callbacks
    ConnectionRequestCallback connectionRequestCb_;
    ConnectionAcceptedCallback connectionAcceptedCb_;
    ConnectionRejectedCallback connectionRejectedCb_;
    UserOfflineCallback userOfflineCb_;
    DisconnectedCallback disconnectedCb_;
    RegisteredCallback registeredCb_;
    RelayDataCallback relayDataCb_;
    std::mutex callbackMutex_;

    // Heartbeat tracking
    std::chrono::steady_clock::time_point lastHeartbeatSent_;
};

} // namespace omnidesk
