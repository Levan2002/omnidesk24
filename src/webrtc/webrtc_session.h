#pragma once

#include "core/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtc {
class PeerConnection;
class DataChannel;
} // namespace rtc

namespace omnidesk {

class SignalingClient;

struct WebRtcConfig {
    std::string turnServer;
    std::string turnUser;
    std::string turnPassword;
    std::vector<std::string> stunServers = {"stun:stun.l.google.com:19302"};
};

class WebRtcSession {
public:
    using ConnectedCallback    = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;
    using VideoCallback        = std::function<void(const uint8_t* data, size_t size)>;
    using DataCallback         = std::function<void(const uint8_t* data, size_t size)>;

    WebRtcSession(SignalingClient* signaling, const UserID& remoteId,
                  const WebRtcConfig& config);
    ~WebRtcSession();

    WebRtcSession(const WebRtcSession&) = delete;
    WebRtcSession& operator=(const WebRtcSession&) = delete;

    bool startAsHost();
    bool startAsViewer();

    void onRemoteDescription(const std::string& sdp, const std::string& type);
    void onRemoteCandidate(const std::string& candidate, const std::string& sdpMid);

    bool sendVideo(const uint8_t* nalData, size_t size);
    bool sendData(const uint8_t* data, size_t size);

    void setOnConnected(ConnectedCallback cb);
    void setOnDisconnected(DisconnectedCallback cb);
    void setOnVideo(VideoCallback cb);
    void setOnData(DataCallback cb);

    void close();
    bool isConnected() const { return connected_.load(); }

private:
    void setupPeerConnection();
    void setupDataChannel(std::shared_ptr<rtc::DataChannel> dc);
    void setupVideoChannel(std::shared_ptr<rtc::DataChannel> dc);

    enum class Role { Host, Viewer };
    Role role_ = Role::Viewer;

    SignalingClient* signaling_ = nullptr;
    UserID remoteId_;
    WebRtcConfig config_;

    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel> dataChannel_;   // reliable, ordered — control
    std::shared_ptr<rtc::DataChannel> videoChannel_;   // unreliable — video frames

    std::atomic<bool> connected_{false};

    mutable std::mutex cbMutex_;
    ConnectedCallback    onConnected_;
    DisconnectedCallback onDisconnected_;
    VideoCallback        onVideo_;
    DataCallback         onData_;
};

} // namespace omnidesk
