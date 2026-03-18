#include "webrtc/webrtc_session.h"
#include "core/logger.h"
#include "signaling/signaling_client.h"

#include <rtc/rtc.hpp>

namespace omnidesk {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WebRtcSession::WebRtcSession(SignalingClient* signaling, const UserID& remoteId,
                             const WebRtcConfig& config)
    : signaling_(signaling), remoteId_(remoteId), config_(config) {}

WebRtcSession::~WebRtcSession() {
    close();
}

// ---------------------------------------------------------------------------
// PeerConnection setup
// ---------------------------------------------------------------------------

void WebRtcSession::setupPeerConnection() {
    rtc::Configuration rtcConfig;

    for (const auto& stun : config_.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }

    if (!config_.turnServer.empty()) {
        std::string url = config_.turnServer;
        std::string host;
        uint16_t port = 3478;
        auto colonPos = url.find(':');
        if (colonPos != std::string::npos) {
            std::string rest = url.substr(colonPos + 1);
            auto portSep = rest.rfind(':');
            if (portSep != std::string::npos) {
                host = rest.substr(0, portSep);
                port = static_cast<uint16_t>(std::stoi(rest.substr(portSep + 1)));
            } else {
                host = rest;
            }
        }
        if (!host.empty()) {
            rtcConfig.iceServers.emplace_back(
                host, port, config_.turnUser, config_.turnPassword);
        }
    }

    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

    // SDP
    pc_->onLocalDescription([this](rtc::Description desc) {
        std::string sdp(desc);
        std::string type = desc.typeString();
        LOG_DEBUG("WebRtcSession: local SDP generated (type=%s)", type.c_str());

        if (role_ == Role::Host) {
            signaling_->sendSdpOffer(remoteId_, sdp);
        } else {
            signaling_->sendSdpAnswer(remoteId_, sdp);
        }
    });

    // ICE candidates
    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        signaling_->sendIceCandidate(remoteId_, std::string(candidate),
                                      candidate.mid(), 0);
    });

    // Connection state
    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        LOG_INFO("WebRtcSession: PeerConnection state -> %d", static_cast<int>(state));

        if (state == rtc::PeerConnection::State::Connected) {
            connected_.store(true);
            std::lock_guard<std::mutex> lock(cbMutex_);
            if (onConnected_) onConnected_();
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            connected_.store(false);
            std::lock_guard<std::mutex> lock(cbMutex_);
            if (onDisconnected_) onDisconnected_();
        }
    });

    // Viewer: incoming DataChannels
    pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        LOG_INFO("WebRtcSession: received DataChannel (label=%s)", dc->label().c_str());
        if (dc->label() == "video") {
            setupVideoChannel(dc);
        } else {
            dataChannel_ = dc;
            setupDataChannel(dc);
        }
    });
}

// ---------------------------------------------------------------------------
// DataChannel helpers
// ---------------------------------------------------------------------------

void WebRtcSession::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
    dc->onOpen([]() { LOG_INFO("WebRtcSession: control DataChannel open"); });
    dc->onClosed([]() { LOG_INFO("WebRtcSession: control DataChannel closed"); });

    dc->onMessage([this](rtc::binary msg) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (onData_)
            onData_(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    }, [](std::string) {});
}

void WebRtcSession::setupVideoChannel(std::shared_ptr<rtc::DataChannel> dc) {
    videoChannel_ = dc;

    dc->onOpen([]() { LOG_INFO("WebRtcSession: video DataChannel open"); });
    dc->onClosed([]() { LOG_INFO("WebRtcSession: video DataChannel closed"); });

    dc->onMessage([this](rtc::binary msg) {
        static int rxCount = 0;
        if (rxCount++ < 5)
            LOG_INFO("WebRtcSession: video frame #%d (%zu bytes)", rxCount, msg.size());

        std::lock_guard<std::mutex> lock(cbMutex_);
        if (onVideo_)
            onVideo_(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    }, [](std::string) {});
}

// ---------------------------------------------------------------------------
// Host: start
// ---------------------------------------------------------------------------

bool WebRtcSession::startAsHost() {
    role_ = Role::Host;
    LOG_INFO("WebRtcSession: starting as HOST for remote %s", remoteId_.id.c_str());

    try {
        setupPeerConnection();

        // Reliable DataChannel for video (ensures all frames arrive;
        // we'll optimize to unreliable with flow control later)
        videoChannel_ = pc_->createDataChannel("video");
        setupVideoChannel(videoChannel_);

        // Reliable DataChannel for control messages
        dataChannel_ = pc_->createDataChannel("control");
        setupDataChannel(dataChannel_);

        // Generate SDP offer
        pc_->setLocalDescription(rtc::Description::Type::Offer);

        LOG_INFO("WebRtcSession: host setup complete, SDP offer will be sent");
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: startAsHost failed: %s", e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Viewer: start
// ---------------------------------------------------------------------------

bool WebRtcSession::startAsViewer() {
    role_ = Role::Viewer;
    LOG_INFO("WebRtcSession: starting as VIEWER for remote %s", remoteId_.id.c_str());

    try {
        setupPeerConnection();
        // Viewer waits for remote offer. DataChannels arrive via onDataChannel.
        LOG_INFO("WebRtcSession: viewer setup complete, waiting for remote offer");
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: startAsViewer failed: %s", e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Signaling: feed remote SDP / ICE
// ---------------------------------------------------------------------------

void WebRtcSession::onRemoteDescription(const std::string& sdp, const std::string& type) {
    if (!pc_) return;
    try {
        pc_->setRemoteDescription(rtc::Description(sdp, type));
    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: setRemoteDescription failed: %s", e.what());
    }
}

void WebRtcSession::onRemoteCandidate(const std::string& candidate,
                                       const std::string& sdpMid) {
    if (!pc_) return;
    try {
        pc_->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: addRemoteCandidate failed: %s", e.what());
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool WebRtcSession::sendVideo(const uint8_t* nalData, size_t size) {
    if (!videoChannel_ || !videoChannel_->isOpen()) return false;
    try {
        videoChannel_->send(reinterpret_cast<const std::byte*>(nalData), size);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: sendVideo failed: %s", e.what());
        return false;
    }
}

bool WebRtcSession::sendData(const uint8_t* data, size_t size) {
    if (!dataChannel_ || !dataChannel_->isOpen()) return false;
    try {
        dataChannel_->send(reinterpret_cast<const std::byte*>(data), size);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: sendData failed: %s", e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Callback setters
// ---------------------------------------------------------------------------

void WebRtcSession::setOnConnected(ConnectedCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    onConnected_ = std::move(cb);
}

void WebRtcSession::setOnDisconnected(DisconnectedCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    onDisconnected_ = std::move(cb);
}

void WebRtcSession::setOnVideo(VideoCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    onVideo_ = std::move(cb);
}

void WebRtcSession::setOnData(DataCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    onData_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void WebRtcSession::close() {
    connected_.store(false);

    if (videoChannel_) {
        try { videoChannel_->close(); } catch (...) {}
        videoChannel_.reset();
    }
    if (dataChannel_) {
        try { dataChannel_->close(); } catch (...) {}
        dataChannel_.reset();
    }
    if (pc_) {
        try { pc_->close(); } catch (...) {}
        pc_.reset();
    }

    LOG_INFO("WebRtcSession: closed");
}

} // namespace omnidesk
