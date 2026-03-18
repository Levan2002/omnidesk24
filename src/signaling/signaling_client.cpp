#include "signaling/signaling_client.h"
#include "core/logger.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <sstream>

namespace omnidesk {

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    disconnect();
}

bool SignalingClient::connect(const std::string& host, uint16_t port,
                              const std::vector<uint16_t>& fallbackPorts) {
    if (connected_) disconnect();

    serverHost_    = host;
    serverPort_    = port;
    fallbackPorts_ = fallbackPorts;
    connectedPort_ = 0;

    // Build ordered list: primary port first, then fallbacks (skip duplicates).
    std::vector<uint16_t> portsToTry;
    portsToTry.push_back(port);
    for (uint16_t p : fallbackPorts) {
        if (p != port) portsToTry.push_back(p);
    }

    // Short per-port timeout so the UI doesn't freeze for too long.
    constexpr int kPortTimeoutMs = 2500;

    bool ok = false;
    for (uint16_t p : portsToTry) {
        if (channel_.connect(host, p, kPortTimeoutMs)) {
            connectedPort_ = p;
            ok = true;
            break;
        }
    }
    if (!ok) return false;

    connected_ = true;
    running_ = true;
    lastHeartbeatSent_ = std::chrono::steady_clock::now();

    receiveThread_ = std::thread(&SignalingClient::receiveLoop, this);
    return true;
}

void SignalingClient::disconnect() {
    running_ = false;
    connected_ = false;
    registered_ = false;

    channel_.close();

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
}

bool SignalingClient::registerUser(const UserID& userId, const PeerAddress& localAddr) {
    if (!connected_) return false;

    userId_ = userId;
    localAddr_ = localAddr;

    std::string msg = jsonMakeObject({
        {"type", "register"},
        {"user_id", userId.id},
        {"local_host", localAddr.host},
        {"local_port", std::to_string(localAddr.port)}
    });

    return sendJson(msg);
}

bool SignalingClient::requestConnection(const UserID& targetId) {
    if (!connected_ || !registered_) return false;

    std::string msg = jsonMakeObject({
        {"type", "connect_request"},
        {"from_id", userId_.id},
        {"target_id", targetId.id}
    });

    return sendJson(msg);
}

bool SignalingClient::acceptConnection(const UserID& fromId) {
    if (!connected_ || !registered_) return false;

    std::string msg = jsonMakeObject({
        {"type", "connect_accept"},
        {"from_id", userId_.id},
        {"target_id", fromId.id}
    });

    return sendJson(msg);
}

bool SignalingClient::rejectConnection(const UserID& fromId, const std::string& reason) {
    if (!connected_ || !registered_) return false;

    std::string msg = jsonMakeObject({
        {"type", "connect_reject"},
        {"from_id", userId_.id},
        {"target_id", fromId.id},
        {"reason", reason}
    });

    return sendJson(msg);
}

bool SignalingClient::isConnected() const {
    return connected_ && channel_.isOpen();
}

void SignalingClient::onConnectionRequest(ConnectionRequestCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionRequestCb_ = std::move(cb);
}

void SignalingClient::onConnectionAccepted(ConnectionAcceptedCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionAcceptedCb_ = std::move(cb);
}

void SignalingClient::onConnectionRejected(ConnectionRejectedCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionRejectedCb_ = std::move(cb);
}

void SignalingClient::onUserOffline(UserOfflineCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    userOfflineCb_ = std::move(cb);
}

void SignalingClient::onDisconnected(DisconnectedCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    disconnectedCb_ = std::move(cb);
}

void SignalingClient::onRegistered(RegisteredCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    registeredCb_ = std::move(cb);
}

void SignalingClient::onSdpOffer(SdpOfferCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    sdpOfferCb_ = std::move(cb);
}

void SignalingClient::onSdpAnswer(SdpAnswerCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    sdpAnswerCb_ = std::move(cb);
}

void SignalingClient::onIceCandidate(IceCandidateCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    iceCandidateCb_ = std::move(cb);
}

bool SignalingClient::sendSdpOffer(const UserID& targetId, const std::string& sdp) {
    if (!connected_ || !registered_) return false;

    std::string msg = jsonMakeObject({
        {"type", "sdp_offer"},
        {"to", targetId.id},
        {"sdp", jsonEscape(sdp)}
    });
    return sendJson(msg);
}

bool SignalingClient::sendSdpAnswer(const UserID& targetId, const std::string& sdp) {
    if (!connected_ || !registered_) return false;

    std::string msg = jsonMakeObject({
        {"type", "sdp_answer"},
        {"to", targetId.id},
        {"sdp", jsonEscape(sdp)}
    });
    return sendJson(msg);
}

bool SignalingClient::sendIceCandidate(const UserID& targetId, const std::string& candidate,
                                        const std::string& sdpMid, int sdpMLineIndex) {
    if (!connected_ || !registered_) return false;

    std::string msg = jsonMakeObject({
        {"type", "ice_candidate"},
        {"to", targetId.id},
        {"candidate", jsonEscape(candidate)},
        {"sdpMid", sdpMid},
        {"sdpMLineIndex", std::to_string(sdpMLineIndex)}
    });
    return sendJson(msg);
}

void SignalingClient::poll() {
    // Callbacks are dispatched directly from the receive thread.
    // Callers that need thread-safe dispatch should queue actions themselves.
}

void SignalingClient::receiveLoop() {
    while (running_) {
        // Send heartbeat if needed
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - lastHeartbeatSent_).count();

        if (elapsed >= HEARTBEAT_INTERVAL_SEC && registered_) {
            std::string hb = jsonMakeObject({
                {"type", "heartbeat"},
                {"user_id", userId_.id}
            });
            sendJson(hb);
            lastHeartbeatSent_ = now;
        }

        // Poll for incoming messages
        if (!channel_.isOpen()) {
            connected_ = false;
            registered_ = false;

            // Notify disconnect
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (disconnectedCb_) disconnectedCb_();
            }

            // Try reconnect
            if (autoReconnect_ && running_) {
                if (tryReconnect()) {
                    continue;
                }
            }
            break;
        }

        if (!channel_.pollRead(100)) continue;

        MessageType msgType;
        std::vector<uint8_t> payload;
        auto result = channel_.recv(msgType, payload);

        if (result == SocketResult::OK) {
            std::string json(payload.begin(), payload.end());
            handleMessage(json);
        } else if (result == SocketResult::DISCONNECTED) {
            connected_ = false;
            registered_ = false;

            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (disconnectedCb_) disconnectedCb_();
            }

            if (autoReconnect_ && running_) {
                if (tryReconnect()) continue;
            }
            break;
        }
    }
}

void SignalingClient::handleMessage(const std::string& json) {
    std::string type = jsonGetString(json, "type");

    if (type == "register_ok") {
        registered_ = true;
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (registeredCb_) registeredCb_(true);
    }
    else if (type == "register_fail") {
        registered_ = false;
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (registeredCb_) registeredCb_(false);
    }
    else if (type == "connect_request") {
        ConnectionRequest req;
        req.fromId = UserID{jsonGetString(json, "from_id")};
        req.publicAddr.host = jsonGetString(json, "from_public_host");
        std::string portStr = jsonGetString(json, "from_public_port");
        if (!portStr.empty()) req.publicAddr.port = static_cast<uint16_t>(std::stoi(portStr));
        req.localAddr.host = jsonGetString(json, "from_local_host");
        portStr = jsonGetString(json, "from_local_port");
        if (!portStr.empty()) req.localAddr.port = static_cast<uint16_t>(std::stoi(portStr));

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (connectionRequestCb_) connectionRequestCb_(req);
    }
    else if (type == "connect_accept") {
        ConnectionAcceptance acc;
        acc.fromId = UserID{jsonGetString(json, "from_id")};
        acc.peerPublicAddr.host = jsonGetString(json, "peer_public_host");
        std::string portStr = jsonGetString(json, "peer_public_port");
        if (!portStr.empty()) acc.peerPublicAddr.port = static_cast<uint16_t>(std::stoi(portStr));
        acc.peerLocalAddr.host = jsonGetString(json, "peer_local_host");
        portStr = jsonGetString(json, "peer_local_port");
        if (!portStr.empty()) acc.peerLocalAddr.port = static_cast<uint16_t>(std::stoi(portStr));

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (connectionAcceptedCb_) connectionAcceptedCb_(acc);
    }
    else if (type == "connect_reject") {
        ConnectionRejection rej;
        rej.fromId = UserID{jsonGetString(json, "from_id")};
        rej.reason = jsonGetString(json, "reason");

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (connectionRejectedCb_) connectionRejectedCb_(rej);
    }
    else if (type == "user_offline") {
        UserID targetId{jsonGetString(json, "target_id")};

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (userOfflineCb_) userOfflineCb_(targetId);
    }
    else if (type == "sdp_offer") {
        SdpMessage msg;
        msg.fromId = UserID{jsonGetString(json, "from")};
        msg.sdp = jsonGetString(json, "sdp");
        msg.type = "offer";

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (sdpOfferCb_) sdpOfferCb_(msg);
    }
    else if (type == "sdp_answer") {
        SdpMessage msg;
        msg.fromId = UserID{jsonGetString(json, "from")};
        msg.sdp = jsonGetString(json, "sdp");
        msg.type = "answer";

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (sdpAnswerCb_) sdpAnswerCb_(msg);
    }
    else if (type == "ice_candidate") {
        IceCandidateMsg msg;
        msg.fromId = UserID{jsonGetString(json, "from")};
        msg.candidate = jsonGetString(json, "candidate");
        msg.sdpMid = jsonGetString(json, "sdpMid");
        std::string idxStr = jsonGetString(json, "sdpMLineIndex");
        if (!idxStr.empty()) msg.sdpMLineIndex = std::stoi(idxStr);

        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (iceCandidateCb_) iceCandidateCb_(msg);
    }
    else if (type == "heartbeat_ack") {
        // Heartbeat acknowledged, nothing to do
    }
}

bool SignalingClient::sendJson(const std::string& json) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    auto result = channel_.send(MessageType::HELLO,
                                reinterpret_cast<const void*>(json.data()),
                                static_cast<uint32_t>(json.size()));
    return result == SocketResult::OK;
}

bool SignalingClient::tryReconnect() {
    // Build the port list: try the last-known working port first.
    std::vector<uint16_t> portsToTry;
    if (connectedPort_ != 0) portsToTry.push_back(connectedPort_);
    if (serverPort_ != connectedPort_) portsToTry.push_back(serverPort_);
    for (uint16_t p : fallbackPorts_) {
        bool dup = false;
        for (uint16_t x : portsToTry) if (x == p) { dup = true; break; }
        if (!dup) portsToTry.push_back(p);
    }

    for (int attempt = 0; attempt < MAX_RECONNECT_ATTEMPTS && running_; ++attempt) {
        // Wait before attempting reconnect
        for (int i = 0; i < RECONNECT_INTERVAL_SEC * 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) return false;

        channel_.close();

        constexpr int kPortTimeoutMs = 2500;
        bool ok = false;
        for (uint16_t p : portsToTry) {
            if (channel_.connect(serverHost_, p, kPortTimeoutMs)) {
                connectedPort_ = p;
                ok = true;
                break;
            }
        }

        if (ok) {
            connected_ = true;

            // Re-register if we were registered before
            if (userId_.valid()) {
                std::string msg = jsonMakeObject({
                    {"type", "register"},
                    {"user_id", userId_.id},
                    {"local_host", localAddr_.host},
                    {"local_port", std::to_string(localAddr_.port)}
                });
                sendJson(msg);
            }

            lastHeartbeatSent_ = std::chrono::steady_clock::now();
            return true;
        }
    }
    return false;
}

// ---- Simple JSON helpers ----

std::string SignalingClient::jsonGetString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return "";

    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";

    if (json[pos] != '"') return "";

    auto endPos = json.find('"', pos + 1);
    if (endPos == std::string::npos) return "";

    return json.substr(pos + 1, endPos - pos - 1);
}

std::string SignalingClient::jsonMakeObject(
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

std::string SignalingClient::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

} // namespace omnidesk
