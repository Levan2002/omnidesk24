#pragma once

#include "core/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare libdatachannel types to keep this header lightweight.
namespace rtc {
class PeerConnection;
class DataChannel;
class Track;
struct RtpPacketizationConfig;
} // namespace rtc

namespace omnidesk {

class SignalingClient;

/// Configuration for ICE/STUN/TURN servers used by the PeerConnection.
struct WebRtcConfig {
    std::string turnServer;     // e.g. "turn:relay.example.com:3478"
    std::string turnUser;
    std::string turnPassword;
    std::vector<std::string> stunServers = {"stun:stun.l.google.com:19302"};
};

/// WebRtcSession wraps an rtc::PeerConnection and manages:
///   - H.264 video track (host sends, viewer receives)
///   - DataChannel for control messages (input events, quality reports)
///   - SDP/ICE exchange via SignalingClient
///
/// Thread-safety: all public methods are safe to call from any thread.
/// libdatachannel callbacks run on internal threads; the session protects
/// its own state and callback invocations with a mutex.
class WebRtcSession {
public:
    using ConnectedCallback    = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;
    using VideoCallback        = std::function<void(const uint8_t* data, size_t size)>;
    using DataCallback         = std::function<void(const uint8_t* data, size_t size)>;

    /// Create a session that will communicate with \p remoteId via \p signaling.
    WebRtcSession(SignalingClient* signaling, const UserID& remoteId,
                  const WebRtcConfig& config);
    ~WebRtcSession();

    // Non-copyable, non-movable (prevent double-free of PeerConnection).
    WebRtcSession(const WebRtcSession&) = delete;
    WebRtcSession& operator=(const WebRtcSession&) = delete;

    /// Start as the hosting (sending) side.
    /// Creates the video track + DataChannel and generates an SDP offer.
    bool startAsHost();

    /// Start as the viewing (receiving) side.
    /// Waits for the remote offer (call onRemoteDescription when it arrives).
    bool startAsViewer();

    /// Feed a remote SDP description received via signaling.
    void onRemoteDescription(const std::string& sdp, const std::string& type);

    /// Feed a remote ICE candidate received via signaling.
    void onRemoteCandidate(const std::string& candidate, const std::string& sdpMid);

    /// Send an encoded H.264 NAL unit over the video track (host only).
    /// Returns false if the track is not open.
    bool sendVideo(const uint8_t* nalData, size_t size);

    /// Send an arbitrary binary message over the DataChannel.
    /// Returns false if the DataChannel is not open.
    bool sendData(const uint8_t* data, size_t size);

    // ---- Callback setters (thread-safe) ----
    void setOnConnected(ConnectedCallback cb);
    void setOnDisconnected(DisconnectedCallback cb);
    void setOnVideo(VideoCallback cb);
    void setOnData(DataCallback cb);

    /// Tear down the PeerConnection and DataChannel.
    void close();

    /// True once the PeerConnection reaches the Connected state.
    bool isConnected() const { return connected_.load(); }

private:
    /// Shared PeerConnection setup (ICE servers, callbacks).
    void setupPeerConnection();

    /// Install callbacks on a DataChannel (used for both host-created and
    /// viewer-received channels).
    void setupDataChannel(std::shared_ptr<rtc::DataChannel> dc);

    /// Install callbacks on a Track received by the viewer.
    void setupIncomingTrack(std::shared_ptr<rtc::Track> track);

    // ---- Role ----
    enum class Role { Host, Viewer };
    Role role_ = Role::Viewer;

    // ---- External references (non-owning) ----
    SignalingClient* signaling_ = nullptr;
    UserID remoteId_;
    WebRtcConfig config_;

    // ---- libdatachannel objects ----
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::DataChannel> dataChannel_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig_;

    // ---- State ----
    std::atomic<bool> connected_{false};

    // ---- Callbacks (protected by cbMutex_) ----
    mutable std::mutex cbMutex_;
    ConnectedCallback    onConnected_;
    DisconnectedCallback onDisconnected_;
    VideoCallback        onVideo_;
    DataCallback         onData_;
};

} // namespace omnidesk
