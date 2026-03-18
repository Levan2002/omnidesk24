#include "webrtc/webrtc_session.h"
#include "core/logger.h"
#include "signaling/signaling_client.h"

#include <rtc/rtc.hpp>

#include <cstring>
#include <random>

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
// PeerConnection setup (shared between host and viewer)
// ---------------------------------------------------------------------------

void WebRtcSession::setupPeerConnection() {
    rtc::Configuration rtcConfig;

    // STUN servers
    for (const auto& stun : config_.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }

    // Optional TURN server — parse "turn:host:port" into hostname + port
    // and construct an IceServer with credentials.
    if (!config_.turnServer.empty()) {
        // Extract hostname and port from the TURN URL.
        // Expected format: "turn:hostname:port" or "turns:hostname:port"
        std::string url = config_.turnServer;
        std::string host;
        uint16_t port = 3478; // default TURN port
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

    // ---- onLocalDescription: send SDP to remote peer via signaling ----
    pc_->onLocalDescription([this](rtc::Description desc) {
        std::string sdp(desc);
        std::string type = desc.typeString();

        LOG_DEBUG("WebRtcSession: local description generated (type=%s, len=%zu)",
                  type.c_str(), sdp.size());

        if (role_ == Role::Host) {
            signaling_->sendSdpOffer(remoteId_, sdp);
        } else {
            signaling_->sendSdpAnswer(remoteId_, sdp);
        }
    });

    // ---- onLocalCandidate: send ICE candidate to remote peer ----
    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        std::string cand = std::string(candidate);
        std::string mid  = candidate.mid();

        LOG_DEBUG("WebRtcSession: local ICE candidate (mid=%s)", mid.c_str());

        signaling_->sendIceCandidate(remoteId_, cand, mid, 0);
    });

    // ---- onStateChange: track connected / disconnected ----
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

    // ---- Viewer: listen for incoming track ----
    pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
        LOG_INFO("WebRtcSession: received remote track (mid=%s)", track->mid().c_str());
        setupIncomingTrack(track);
    });

    // ---- Viewer: listen for incoming DataChannel ----
    pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        LOG_INFO("WebRtcSession: received remote DataChannel (label=%s)",
                 dc->label().c_str());
        dataChannel_ = dc;
        setupDataChannel(dc);
    });
}

// ---------------------------------------------------------------------------
// DataChannel helpers
// ---------------------------------------------------------------------------

void WebRtcSession::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
    dc->onOpen([this]() {
        LOG_INFO("WebRtcSession: DataChannel open");
    });

    dc->onClosed([this]() {
        LOG_INFO("WebRtcSession: DataChannel closed");
    });

    dc->onMessage([this](rtc::message_variant msg) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (!onData_) return;

        if (std::holds_alternative<rtc::binary>(msg)) {
            const auto& bin = std::get<rtc::binary>(msg);
            onData_(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
        } else {
            // String messages: forward raw bytes.
            const auto& str = std::get<std::string>(msg);
            onData_(reinterpret_cast<const uint8_t*>(str.data()), str.size());
        }
    });
}

// ---------------------------------------------------------------------------
// Incoming track (viewer side)
// ---------------------------------------------------------------------------

void WebRtcSession::setupIncomingTrack(std::shared_ptr<rtc::Track> track) {
    videoTrack_ = track;

    // Install an H.264 RTP depacketizer so we receive reassembled NAL units.
    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::LongStartSequence);
    track->setMediaHandler(depacketizer);

    track->onMessage([this](rtc::message_variant msg) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (!onVideo_) return;

        if (std::holds_alternative<rtc::binary>(msg)) {
            const auto& bin = std::get<rtc::binary>(msg);
            onVideo_(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
        }
    });
}

// ---------------------------------------------------------------------------
// Host: start
// ---------------------------------------------------------------------------

bool WebRtcSession::startAsHost() {
    role_ = Role::Host;

    LOG_INFO("WebRtcSession: starting as HOST for remote %s", remoteId_.id.c_str());

    try {
        setupPeerConnection();

        // ---- Add H.264 video track (send-only) ----
        rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
        media.addH264Codec(96);  // payload type 96

        // Generate a random SSRC
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFE);
        uint32_t ssrc = dist(gen);
        media.addSSRC(ssrc, "video-stream");

        videoTrack_ = pc_->addTrack(media);

        // Set up RTP packetizer for H.264
        rtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "video-stream", 96, rtc::H264RtpPacketizer::ClockRate);

        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::LongStartSequence, rtpConfig_);

        videoTrack_->setMediaHandler(packetizer);

        // ---- Create DataChannel for control messages ----
        dataChannel_ = pc_->createDataChannel("control");
        setupDataChannel(dataChannel_);

        // ---- Generate SDP offer ----
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

        // The viewer does NOT call setLocalDescription. It waits for the
        // remote offer (delivered via onRemoteDescription), and
        // libdatachannel will automatically generate an answer.

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
    if (!pc_) {
        LOG_ERROR("WebRtcSession: onRemoteDescription called but PeerConnection is null");
        return;
    }

    LOG_DEBUG("WebRtcSession: setting remote description (type=%s, len=%zu)",
              type.c_str(), sdp.size());

    try {
        pc_->setRemoteDescription(rtc::Description(sdp, type));
    } catch (const std::exception& e) {
        LOG_ERROR("WebRtcSession: setRemoteDescription failed: %s", e.what());
    }
}

void WebRtcSession::onRemoteCandidate(const std::string& candidate,
                                       const std::string& sdpMid) {
    if (!pc_) {
        LOG_ERROR("WebRtcSession: onRemoteCandidate called but PeerConnection is null");
        return;
    }

    LOG_DEBUG("WebRtcSession: adding remote ICE candidate (mid=%s)", sdpMid.c_str());

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
    if (!videoTrack_ || !videoTrack_->isOpen()) return false;

    try {
        videoTrack_->send(reinterpret_cast<const std::byte*>(nalData), size);
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

    if (dataChannel_) {
        try { dataChannel_->close(); } catch (...) {}
        dataChannel_.reset();
    }

    if (videoTrack_) {
        try { videoTrack_->close(); } catch (...) {}
        videoTrack_.reset();
    }

    if (pc_) {
        try { pc_->close(); } catch (...) {}
        pc_.reset();
    }

    rtpConfig_.reset();

    LOG_INFO("WebRtcSession: closed");
}

} // namespace omnidesk
