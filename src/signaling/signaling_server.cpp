#include "signaling/signaling_server.h"
#include "core/logger.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <sstream>

namespace omnidesk {

SignalingServer::SignalingServer() = default;

SignalingServer::~SignalingServer() {
    stop();
}

bool SignalingServer::start(uint16_t port) {
    if (running_) return false;

    if (!listener_.listen(port)) return false;

    // Retrieve the actual port (important when port=0 for OS-assigned ports)
    port_ = listener_.localPort();
    running_ = true;
    serverThread_ = std::thread(&SignalingServer::serverLoop, this);
    return true;
}

void SignalingServer::stop() {
    running_ = false;
    listener_.close();

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& client : clients_) {
            client->close();
        }
        clients_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(usersMutex_);
        users_.clear();
    }
}

bool SignalingServer::isRunning() const {
    return running_;
}

size_t SignalingServer::userCount() const {
    std::lock_guard<std::mutex> lock(usersMutex_);
    return users_.size();
}

bool SignalingServer::isUserRegistered(const UserID& userId) const {
    std::lock_guard<std::mutex> lock(usersMutex_);
    return users_.find(userId.id) != users_.end();
}

bool SignalingServer::getUserInfo(const UserID& userId, PeerAddress& publicAddr,
                                  PeerAddress& localAddr) const {
    std::lock_guard<std::mutex> lock(usersMutex_);
    auto it = users_.find(userId.id);
    if (it == users_.end()) return false;
    publicAddr = it->second.publicAddr;
    localAddr = it->second.localAddr;
    return true;
}

void SignalingServer::serverLoop() {
    auto lastPurge = std::chrono::steady_clock::now();

    while (running_) {
        // Accept new connections
        if (listener_.pollRead(50)) {
            auto client = listener_.accept();
            if (client) {
                auto shared = std::shared_ptr<TcpChannel>(client.release());
                std::lock_guard<std::mutex> lock(clientsMutex_);
                clients_.push_back(shared);
            }
        }

        // Process messages from all clients
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = clients_.begin();
            while (it != clients_.end()) {
                auto& client = *it;
                if (!client->isOpen()) {
                    // Client disconnected - remove from users
                    {
                        std::lock_guard<std::mutex> ulock(usersMutex_);
                        for (auto uit = users_.begin(); uit != users_.end(); ++uit) {
                            if (uit->second.channel.get() == client.get()) {
                                users_.erase(uit);
                                break;
                            }
                        }
                    }
                    it = clients_.erase(it);
                    continue;
                }

                // Try to receive a message
                MessageType msgType;
                std::vector<uint8_t> payload;
                auto result = client->recv(msgType, payload);

                if (result == SocketResult::OK) {
                    if (msgType == MessageType::RELAY_DATA) {
                        LOG_INFO("Server: got RELAY_DATA from client, payload=%zu bytes",
                                 payload.size());
                        handleRelayData(client, payload);
                    } else {
                        std::string jsonStr(payload.begin(), payload.end());
                        handleClientMessage(client, jsonStr);
                    }
                } else if (result == SocketResult::DISCONNECTED) {
                    {
                        std::lock_guard<std::mutex> ulock(usersMutex_);
                        for (auto uit = users_.begin(); uit != users_.end(); ++uit) {
                            if (uit->second.channel.get() == client.get()) {
                                users_.erase(uit);
                                break;
                            }
                        }
                    }
                    client->close();
                    it = clients_.erase(it);
                    continue;
                }

                ++it;
            }
        }

        // Periodic purge of stale users
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPurge).count() >= 30) {
            purgeStaleUsers();
            lastPurge = now;
        }
    }
}

void SignalingServer::handleClientMessage(std::shared_ptr<TcpChannel> client,
                                           const std::string& jsonStr) {
    std::string type = jsonGetString(jsonStr, "type");

    if (type == SignalingMsg::REGISTER) {
        handleRegister(client, jsonStr);
    } else if (type == SignalingMsg::CONNECT_REQUEST) {
        handleConnectRequest(client, jsonStr);
    } else if (type == SignalingMsg::CONNECT_ACCEPT) {
        handleConnectAccept(client, jsonStr);
    } else if (type == SignalingMsg::CONNECT_REJECT) {
        handleConnectReject(client, jsonStr);
    } else if (type == SignalingMsg::HEARTBEAT) {
        handleHeartbeat(client, jsonStr);
    }
}

void SignalingServer::handleRegister(std::shared_ptr<TcpChannel> client,
                                      const std::string& json) {
    std::string userId = jsonGetString(json, "user_id");
    std::string localHost = jsonGetString(json, "local_host");
    std::string localPortStr = jsonGetString(json, "local_port");

    if (userId.empty() || userId.size() != 8) {
        std::string resp = jsonMakeObject({
            {"type", SignalingMsg::REGISTER_FAIL},
            {"reason", "invalid_user_id"}
        });
        sendJson(*client, resp);
        return;
    }

    // Check for duplicate registration
    {
        std::lock_guard<std::mutex> lock(usersMutex_);
        auto it = users_.find(userId);
        if (it != users_.end()) {
            // Replace existing registration (reconnect scenario)
            it->second.channel->close();
        }

        RegisteredUser user;
        user.userId = UserID{userId};
        user.channel = client;
        user.publicAddr.host = client->remoteAddress();
        user.publicAddr.port = client->remotePort();
        user.localAddr.host = localHost;
        if (!localPortStr.empty()) {
            user.localAddr.port = static_cast<uint16_t>(std::stoi(localPortStr));
        }
        user.lastHeartbeat = std::chrono::steady_clock::now();

        users_[userId] = std::move(user);
    }

    std::string resp = jsonMakeObject({
        {"type", SignalingMsg::REGISTER_OK},
        {"user_id", userId}
    });
    sendJson(*client, resp);
}

void SignalingServer::handleConnectRequest(std::shared_ptr<TcpChannel> client,
                                            const std::string& json) {
    std::string fromId = jsonGetString(json, "from_id");
    std::string targetId = jsonGetString(json, "target_id");

    if (fromId.empty() || targetId.empty()) return;

    std::lock_guard<std::mutex> lock(usersMutex_);

    // Check if target is registered
    auto it = users_.find(targetId);
    if (it == users_.end() || !it->second.channel || !it->second.channel->isOpen()) {
        // Target offline
        std::string resp = jsonMakeObject({
            {"type", SignalingMsg::USER_OFFLINE},
            {"target_id", targetId}
        });
        sendJson(*client, resp);
        return;
    }

    // Get the requester's address info
    auto fromIt = users_.find(fromId);
    std::string fromPublicHost, fromPublicPort, fromLocalHost, fromLocalPort;
    if (fromIt != users_.end()) {
        fromPublicHost = fromIt->second.publicAddr.host;
        fromPublicPort = std::to_string(fromIt->second.publicAddr.port);
        fromLocalHost = fromIt->second.localAddr.host;
        fromLocalPort = std::to_string(fromIt->second.localAddr.port);
    }

    // Relay the connect request to the target
    std::string relayMsg = jsonMakeObject({
        {"type", SignalingMsg::CONNECT_REQUEST},
        {"from_id", fromId},
        {"from_public_host", fromPublicHost},
        {"from_public_port", fromPublicPort},
        {"from_local_host", fromLocalHost},
        {"from_local_port", fromLocalPort}
    });
    sendJson(*it->second.channel, relayMsg);
}

void SignalingServer::handleConnectAccept(std::shared_ptr<TcpChannel> client,
                                           const std::string& json) {
    std::string fromId = jsonGetString(json, "from_id");
    std::string targetId = jsonGetString(json, "target_id");

    if (fromId.empty() || targetId.empty()) return;

    std::lock_guard<std::mutex> lock(usersMutex_);

    // Find the original requester to relay the acceptance
    auto it = users_.find(targetId);
    if (it == users_.end() || !it->second.channel || !it->second.channel->isOpen()) return;

    // Get the accepter's address info
    auto fromIt = users_.find(fromId);
    std::string peerPublicHost, peerPublicPort, peerLocalHost, peerLocalPort;
    if (fromIt != users_.end()) {
        peerPublicHost = fromIt->second.publicAddr.host;
        peerPublicPort = std::to_string(fromIt->second.publicAddr.port);
        peerLocalHost = fromIt->second.localAddr.host;
        peerLocalPort = std::to_string(fromIt->second.localAddr.port);
    }

    std::string relayMsg = jsonMakeObject({
        {"type", SignalingMsg::CONNECT_ACCEPT},
        {"from_id", fromId},
        {"peer_public_host", peerPublicHost},
        {"peer_public_port", peerPublicPort},
        {"peer_local_host", peerLocalHost},
        {"peer_local_port", peerLocalPort}
    });
    sendJson(*it->second.channel, relayMsg);
}

void SignalingServer::handleConnectReject(std::shared_ptr<TcpChannel> client,
                                           const std::string& json) {
    std::string fromId = jsonGetString(json, "from_id");
    std::string targetId = jsonGetString(json, "target_id");
    std::string reason = jsonGetString(json, "reason");

    if (fromId.empty() || targetId.empty()) return;

    std::lock_guard<std::mutex> lock(usersMutex_);

    auto it = users_.find(targetId);
    if (it == users_.end() || !it->second.channel || !it->second.channel->isOpen()) return;

    std::string relayMsg = jsonMakeObject({
        {"type", SignalingMsg::CONNECT_REJECT},
        {"from_id", fromId},
        {"reason", reason}
    });
    sendJson(*it->second.channel, relayMsg);
}

void SignalingServer::handleHeartbeat(std::shared_ptr<TcpChannel> client,
                                       const std::string& json) {
    std::string userId = jsonGetString(json, "user_id");

    if (!userId.empty()) {
        std::lock_guard<std::mutex> lock(usersMutex_);
        auto it = users_.find(userId);
        if (it != users_.end()) {
            it->second.lastHeartbeat = std::chrono::steady_clock::now();
        }
    }

    std::string resp = jsonMakeObject({
        {"type", SignalingMsg::HEARTBEAT_ACK}
    });
    sendJson(*client, resp);
}

void SignalingServer::handleRelayData(std::shared_ptr<TcpChannel> client,
                                       const std::vector<uint8_t>& payload) {
    // Wire format: [8 bytes target ID][2 bytes inner type][N bytes data]
    if (payload.size() < 10) return;

    std::string targetId(payload.begin(), payload.begin() + 8);
    while (!targetId.empty() && targetId.back() == '\0') targetId.pop_back();

    // Find the sender's user ID
    std::string senderId;
    {
        std::lock_guard<std::mutex> lock(usersMutex_);
        for (const auto& [id, user] : users_) {
            if (user.channel.get() == client.get()) {
                senderId = id;
                break;
            }
        }
    }
    if (senderId.empty()) {
        LOG_WARN("Server: relay sender not found in users");
        return;
    }

    LOG_INFO("Server: relaying %zu bytes from %s to %s",
             payload.size(), senderId.c_str(), targetId.c_str());

    // Build forwarded payload: replace target ID with sender ID
    std::vector<uint8_t> fwd(payload.size());
    std::memset(fwd.data(), 0, 8);
    std::memcpy(fwd.data(), senderId.data(),
                std::min<size_t>(senderId.size(), 8));
    std::memcpy(fwd.data() + 8, payload.data() + 8, payload.size() - 8);

    // Forward to target
    std::lock_guard<std::mutex> lock(usersMutex_);
    auto it = users_.find(targetId);
    if (it != users_.end() && it->second.channel && it->second.channel->isOpen()) {
        it->second.channel->send(MessageType::RELAY_DATA,
                                  fwd.data(),
                                  static_cast<uint32_t>(fwd.size()));
    }
}

void SignalingServer::sendJson(TcpChannel& client, const std::string& json) {
    client.send(MessageType::HELLO,
                reinterpret_cast<const void*>(json.data()),
                static_cast<uint32_t>(json.size()));
}

UserID SignalingServer::findUserByChannel(const TcpChannel* channel) const {
    std::lock_guard<std::mutex> lock(usersMutex_);
    for (const auto& [id, user] : users_) {
        if (user.channel.get() == channel) {
            return user.userId;
        }
    }
    return UserID{};
}

RegisteredUser* SignalingServer::findUser(const UserID& userId) {
    auto it = users_.find(userId.id);
    if (it != users_.end()) return &it->second;
    return nullptr;
}

const RegisteredUser* SignalingServer::findUser(const UserID& userId) const {
    auto it = users_.find(userId.id);
    if (it != users_.end()) return &it->second;
    return nullptr;
}

void SignalingServer::purgeStaleUsers() {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(usersMutex_);
    auto it = users_.begin();
    while (it != users_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.lastHeartbeat).count();
        if (elapsed >= HEARTBEAT_TIMEOUT_SEC) {
            if (it->second.channel) {
                it->second.channel->close();
            }
            it = users_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- Simple JSON helpers ----
// Minimal JSON parsing without external dependencies. Handles simple flat
// objects with string values. Not a general-purpose parser.

std::string SignalingServer::jsonGetString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    // Find the colon after the key
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";

    if (json[pos] != '"') return "";

    // Find closing quote
    auto endPos = json.find('"', pos + 1);
    if (endPos == std::string::npos) return "";

    return json.substr(pos + 1, endPos - pos - 1);
}

std::string SignalingServer::jsonMakeObject(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    std::ostringstream ss;
    ss << "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "\"" << fields[i].first << "\":\"" << fields[i].second << "\"";
    }
    ss << "}";
    return ss.str();
}

} // namespace omnidesk
