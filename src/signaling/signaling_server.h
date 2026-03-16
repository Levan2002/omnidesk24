#pragma once

#include "core/types.h"
#include "transport/tcp_channel.h"
#include "transport/protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace omnidesk {

// Signaling message types (sent as JSON over length-prefixed TCP).
// These are distinct from the wire protocol MessageType; signaling
// uses JSON payloads prefixed with a ControlHeader.
namespace SignalingMsg {
    constexpr const char* REGISTER           = "register";
    constexpr const char* REGISTER_OK        = "register_ok";
    constexpr const char* REGISTER_FAIL      = "register_fail";
    constexpr const char* CONNECT_REQUEST    = "connect_request";
    constexpr const char* CONNECT_ACCEPT     = "connect_accept";
    constexpr const char* CONNECT_REJECT     = "connect_reject";
    constexpr const char* HEARTBEAT          = "heartbeat";
    constexpr const char* HEARTBEAT_ACK      = "heartbeat_ack";
    constexpr const char* USER_OFFLINE       = "user_offline";
} // namespace SignalingMsg

// Registered user entry in the server's user registry.
struct RegisteredUser {
    UserID userId;
    std::shared_ptr<TcpChannel> channel;
    PeerAddress publicAddr;   // Public (NAT-mapped) address
    PeerAddress localAddr;    // Local (LAN) address
    std::chrono::steady_clock::time_point lastHeartbeat;
};

// SignalingServer: lightweight TCP server for user registration and
// connection brokering. Handles REGISTER, CONNECT_REQUEST,
// CONNECT_ACCEPT/REJECT, and HEARTBEAT messages.
//
// Messages are ControlHeader-prefixed with JSON payloads.
// Can run standalone (--server flag) or embedded in the application.
class SignalingServer {
public:
    SignalingServer();
    ~SignalingServer();

    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    // Start listening on the specified port. Returns true on success.
    bool start(uint16_t port);

    // Stop the server and disconnect all clients.
    void stop();

    // Check if the server is running.
    bool isRunning() const;

    // Get the port the server is listening on.
    uint16_t port() const { return port_; }

    // Get the number of registered users.
    size_t userCount() const;

    // Check if a user ID is registered.
    bool isUserRegistered(const UserID& userId) const;

    // Get a registered user's info (for testing/debugging).
    bool getUserInfo(const UserID& userId, PeerAddress& publicAddr,
                     PeerAddress& localAddr) const;

    // Heartbeat timeout in seconds. Users not sending heartbeats within
    // this period are considered offline and unregistered.
    static constexpr int HEARTBEAT_TIMEOUT_SEC = 90;

private:
    // Main server loop (runs in its own thread).
    void serverLoop();

    // Process a single client message.
    void handleClientMessage(std::shared_ptr<TcpChannel> client,
                             const std::string& jsonStr);

    // Handle specific message types.
    void handleRegister(std::shared_ptr<TcpChannel> client, const std::string& json);
    void handleConnectRequest(std::shared_ptr<TcpChannel> client, const std::string& json);
    void handleConnectAccept(std::shared_ptr<TcpChannel> client, const std::string& json);
    void handleConnectReject(std::shared_ptr<TcpChannel> client, const std::string& json);
    void handleHeartbeat(std::shared_ptr<TcpChannel> client, const std::string& json);

    // Send a JSON message to a client via ControlHeader-prefixed TCP.
    void sendJson(TcpChannel& client, const std::string& json);

    // Find a user by their TCP channel.
    UserID findUserByChannel(const TcpChannel* channel) const;

    // Find a registered user by their user ID.
    RegisteredUser* findUser(const UserID& userId);
    const RegisteredUser* findUser(const UserID& userId) const;

    // Remove stale users (heartbeat timeout).
    void purgeStaleUsers();

    // Simple JSON helpers (minimal, no external dependency).
    static std::string jsonGetString(const std::string& json, const std::string& key);
    static std::string jsonMakeObject(const std::vector<std::pair<std::string, std::string>>& fields);

    TcpChannel listener_;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread serverThread_;

    mutable std::mutex usersMutex_;
    std::map<std::string, RegisteredUser> users_;  // userId.id -> RegisteredUser

    // Connected clients (may not be registered yet)
    mutable std::mutex clientsMutex_;
    std::vector<std::shared_ptr<TcpChannel>> clients_;
};

} // namespace omnidesk
