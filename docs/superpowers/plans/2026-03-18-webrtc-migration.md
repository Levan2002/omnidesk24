# WebRTC Migration (libdatachannel) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace custom transport with libdatachannel WebRTC. Keep entire codec pipeline. Send H.264 NALs over rtc::Track, control messages over rtc::DataChannel.

**Architecture:** Existing IEncoder produces H.264 → H264RtpPacketizer → rtc::Track → ICE/DTLS-SRTP. Reverse on viewer. DataChannel for input/cursor/clipboard. Signaling server adapted for SDP/ICE.

**Tech Stack:** C++17, libdatachannel v0.22, MinGW GCC 15, CMake, GLFW, ImGui

**Spec:** `docs/superpowers/specs/2026-03-18-webrtc-migration-design.md`

---

## File Structure

### New files

| File | Responsibility |
|------|---------------|
| `cmake/FetchLibDataChannel.cmake` | FetchContent for libdatachannel |
| `src/signaling/tcp_channel.h` | Moved from `src/transport/` |
| `src/signaling/tcp_channel.cpp` | Moved from `src/transport/` |
| `src/signaling/wire_format.h` | ControlHeader + serialization helpers from protocol.h |
| `src/webrtc/webrtc_session.h` | PeerConnection lifecycle, rtc::Track, rtc::DataChannel |
| `src/webrtc/webrtc_session.cpp` | Full implementation |

### Modified files

| File | Change |
|------|--------|
| `CMakeLists.txt` | Add libdatachannel, omnidesk_webrtc target, remove omnidesk_transport |
| `src/core/types.h` | Absorb PeerAddress, InputEvent, InputType from protocol.h. Remove EncodedPacket.data serialization deps on transport. |
| `src/signaling/signaling_client.h/cpp` | Add SDP/ICE methods, remove relay, change includes |
| `src/signaling/signaling_server.h/cpp` | Add SDP/ICE routing, remove relay handler, change includes |
| `src/signaling/database.h` | Remove vestigial transport/protocol.h include |
| `src/session/host_session.h/cpp` | Replace SendCallback serialization with WebRtcSession::sendVideo() |
| `src/session/viewer_session.h/cpp` | Receive NAL units from WebRTC callback instead of TcpChannel |
| `src/ui/app.h/cpp` | Replace TcpChannel/relay with WebRtcSession. Add TURN config. Wire SDP/ICE callbacks. |

### Deleted

| File(s) | Reason |
|---------|--------|
| `src/transport/` (entire dir, after moving tcp_channel) | Replaced by libdatachannel |

---

### Task 1: Add libdatachannel to CMake

**Files:**
- Create: `cmake/FetchLibDataChannel.cmake`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create cmake/FetchLibDataChannel.cmake**

```cmake
include(FetchContent)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/nickhutchinson/libdatachannel.git
    GIT_TAG        v0.22.7
    GIT_SHALLOW    TRUE
)

# Static library, enable media support (H264 RTP), disable WebSocket
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libdatachannel)
```

- [ ] **Step 2: Include in CMakeLists.txt**

Add `include(cmake/FetchLibDataChannel.cmake)` inside `if(NOT OMNIDESK_SERVER_ONLY)` after `include(cmake/FetchDeps.cmake)`.

- [ ] **Step 3: Verify**

Run: `cmake -B build-webrtc -G Ninja`
Expected: FetchContent downloads and configures libdatachannel. Target `datachannel-static` available.

- [ ] **Step 4: Commit**

```bash
git add cmake/FetchLibDataChannel.cmake CMakeLists.txt
git commit -m "feat: add libdatachannel dependency via FetchContent"
```

---

### Task 2: Relocate types and TcpChannel to signaling

**Files:**
- Create: `src/signaling/tcp_channel.h` (copy from transport)
- Create: `src/signaling/tcp_channel.cpp` (copy from transport)
- Create: `src/signaling/wire_format.h`
- Modify: `src/core/types.h`
- Modify: `src/signaling/signaling_client.h/cpp` (change includes)
- Modify: `src/signaling/signaling_server.h/cpp` (change includes)
- Modify: `src/signaling/database.h` (remove vestigial include)
- Modify: `CMakeLists.txt` (update signaling target)

- [ ] **Step 1: Copy tcp_channel to signaling**

Copy `src/transport/tcp_channel.h` → `src/signaling/tcp_channel.h`. Update internal includes to reference `signaling/` instead of `transport/`. Same for `.cpp`.

- [ ] **Step 2: Create wire_format.h**

Extract from `transport/protocol.h`: `ControlHeader`, `PROTOCOL_MAGIC`, `PROTOCOL_VERSION`, serialization helpers (`writeU16` etc.), and the signaling-relevant message type constants. Place in `src/signaling/wire_format.h`.

- [ ] **Step 3: Add PeerAddress, InputEvent, InputType to core/types.h**

Move `PeerAddress` (protocol.h:189-198), `InputType` enum (protocol.h:146-153), and `InputEvent` struct (protocol.h:156-186) into `core/types.h`. Add the `#ifdef _WIN32 winsock2.h` / `arpa/inet.h` includes and the `writeU16`/`readU16`/`writeU32`/`readU32` inline helpers that InputEvent::serialize needs.

- [ ] **Step 4: Update signaling includes**

In `signaling_client.h`: replace `#include "transport/tcp_channel.h"` and `#include "transport/protocol.h"` with `#include "signaling/tcp_channel.h"` and `#include "signaling/wire_format.h"`. Remove `#include "transport/udp_channel.h"`. Same for signaling_server.h, signaling_client.cpp, signaling_server.cpp. In `database.h`: remove `#include "transport/protocol.h"`.

- [ ] **Step 5: Update CMakeLists.txt signaling target**

```cmake
add_library(omnidesk_signaling STATIC
    src/signaling/signaling_server.cpp
    src/signaling/signaling_client.cpp
    src/signaling/user_id.cpp
    src/signaling/tcp_channel.cpp
)
target_link_libraries(omnidesk_signaling PUBLIC omnidesk_core)
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_signaling PRIVATE ws2_32)
endif()
```

- [ ] **Step 6: Build signaling target**

Run: `cmake --build build-webrtc --target omnidesk_signaling`
Expected: Compiles without transport dependency.

- [ ] **Step 7: Commit**

```bash
git add src/signaling/tcp_channel.h src/signaling/tcp_channel.cpp src/signaling/wire_format.h
git add src/core/types.h src/signaling/signaling_client.h src/signaling/signaling_client.cpp
git add src/signaling/signaling_server.h src/signaling/signaling_server.cpp
git add src/signaling/database.h CMakeLists.txt
git commit -m "refactor: move TcpChannel and wire format to signaling, relocate types"
```

---

### Task 3: Delete transport directory

**Files:**
- Delete: `src/transport/` (entire directory)
- Modify: `CMakeLists.txt` (remove omnidesk_transport target and references)

- [ ] **Step 1: Remove transport from CMakeLists.txt**

Delete the entire `omnidesk_transport` target (lines 160-175). Remove `omnidesk_transport` from `omnidesk_session` link list (line 230). Remove from any other references.

- [ ] **Step 2: Delete transport directory**

```bash
rm -rf src/transport/
```

- [ ] **Step 3: Fix any remaining includes**

Search for `#include "transport/` in all remaining source files. The main hits will be in `src/ui/app.h` and `src/ui/app.cpp` — these will be properly fixed in Task 7, but for now comment them out or add `#if 0` to keep cmake configuration working.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: delete custom transport layer (~2000 LOC)"
```

---

### Task 4: Add SDP/ICE to signaling

**Files:**
- Modify: `src/signaling/signaling_client.h`
- Modify: `src/signaling/signaling_client.cpp`
- Modify: `src/signaling/signaling_server.h`
- Modify: `src/signaling/signaling_server.cpp`

- [ ] **Step 1: Add SDP/ICE types and methods to SignalingClient**

Add structs: `SdpMessage { UserID fromId; std::string sdp; }`, `IceCandidate { UserID fromId; std::string candidate; std::string sdpMid; int sdpMLineIndex; }`.

Add methods: `sendSdpOffer(targetId, sdp)`, `sendSdpAnswer(targetId, sdp)`, `sendIceCandidate(targetId, candidate, sdpMid, sdpMLineIndex)`.

Add callbacks: `onSdpOffer`, `onSdpAnswer`, `onIceCandidate`.

Remove: `sendRelayData()`, `RelayDataCallback`, `onRelayData()`, `relayDataCb_`.

- [ ] **Step 2: Implement in signaling_client.cpp**

Send methods serialize to JSON: `{"type":"sdp_offer","to":"<id>","sdp":"<sdp>"}`.
In `handleMessage()`, route `"sdp_offer"`, `"sdp_answer"`, `"ice_candidate"` to callbacks.
Remove relay data handling.

- [ ] **Step 3: Add SDP/ICE routing to SignalingServer**

Add handler methods that forward `sdp_offer`, `sdp_answer`, `ice_candidate` to the target user (same pattern as `handleConnectRequest`). Remove `handleRelayData()`.

- [ ] **Step 4: Build and verify**

Run: `cmake --build build-webrtc --target omnidesk_signaling`

- [ ] **Step 5: Commit**

```bash
git add src/signaling/signaling_client.h src/signaling/signaling_client.cpp
git add src/signaling/signaling_server.h src/signaling/signaling_server.cpp
git commit -m "feat: add SDP/ICE signaling, remove relay support"
```

---

### Task 5: Create WebRtcSession

**Files:**
- Create: `src/webrtc/webrtc_session.h`
- Create: `src/webrtc/webrtc_session.cpp`
- Modify: `CMakeLists.txt` (add omnidesk_webrtc target)

This is the core new component. Wraps libdatachannel's `rtc::PeerConnection`.

- [ ] **Step 1: Write webrtc_session.h**

```cpp
#pragma once
#include "core/types.h"
#include <rtc/rtc.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    using ConnectedCallback = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;
    using VideoCallback = std::function<void(const uint8_t* data, size_t size)>;
    using DataCallback = std::function<void(const uint8_t* data, size_t size)>;

    WebRtcSession(SignalingClient* signaling, const UserID& remoteId,
                  const WebRtcConfig& config);
    ~WebRtcSession();

    // Host: create offer, add video track (send-only), add DataChannel
    bool startAsHost();

    // Viewer: wait for offer, will create answer on receiving it
    bool startAsViewer();

    // Called when SDP/ICE messages arrive from signaling
    void onRemoteDescription(const std::string& sdp, const std::string& type);
    void onRemoteCandidate(const std::string& candidate, const std::string& sdpMid);

    // Send H.264 NAL units over the video track
    bool sendVideo(const uint8_t* nalData, size_t size);

    // Send control message over DataChannel
    bool sendData(const uint8_t* data, size_t size);

    // Set callbacks
    void setOnConnected(ConnectedCallback cb);
    void setOnDisconnected(DisconnectedCallback cb);
    void setOnVideo(VideoCallback cb);
    void setOnData(DataCallback cb);

    void close();

private:
    void setupCallbacks();

    SignalingClient* signaling_;
    UserID remoteId_;
    WebRtcConfig config_;

    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::DataChannel> dataChannel_;

    ConnectedCallback onConnected_;
    DisconnectedCallback onDisconnected_;
    VideoCallback onVideo_;
    DataCallback onData_;
    std::mutex cbMutex_;
};

} // namespace omnidesk
```

- [ ] **Step 2: Write webrtc_session.cpp**

Key implementation sections:

**Constructor — configure ICE:**
```cpp
WebRtcSession::WebRtcSession(SignalingClient* signaling, const UserID& remoteId,
                              const WebRtcConfig& config)
    : signaling_(signaling), remoteId_(remoteId), config_(config) {
    rtc::Configuration rtcConfig;
    for (auto& stun : config.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }
    if (!config.turnServer.empty()) {
        rtcConfig.iceServers.emplace_back(
            config.turnServer, config.turnUser, config.turnPassword);
    }
    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);
    setupCallbacks();
}
```

**setupCallbacks — SDP/ICE trickle:**
```cpp
void WebRtcSession::setupCallbacks() {
    pc_->onLocalDescription([this](rtc::Description desc) {
        signaling_->sendSdpOffer(remoteId_, std::string(desc));  // or sendSdpAnswer
    });
    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        signaling_->sendIceCandidate(remoteId_, std::string(candidate),
                                      candidate.mid(), 0);
    });
    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Connected) {
            std::lock_guard<std::mutex> lock(cbMutex_);
            if (onConnected_) onConnected_();
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            std::lock_guard<std::mutex> lock(cbMutex_);
            if (onDisconnected_) onDisconnected_();
        }
    });
}
```

**startAsHost — add video track + DataChannel:**
```cpp
bool WebRtcSession::startAsHost() {
    // Video track with H264 RTP packetizer
    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(96);
    media.addSSRC(1, "video-stream");
    videoTrack_ = pc_->addTrack(media);

    auto session = std::make_shared<rtc::RtpPacketizationConfig>(
        1, "video-stream", 96, rtc::H264RtpPacketizer::defaultClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence, session);
    videoTrack_->setMediaHandler(packetizer);

    // DataChannel
    dataChannel_ = pc_->createDataChannel("control");
    dataChannel_->onMessage([this](auto data) {
        if (auto* bin = std::get_if<rtc::binary>(&data)) {
            std::lock_guard<std::mutex> lock(cbMutex_);
            if (onData_) onData_(reinterpret_cast<const uint8_t*>(bin->data()), bin->size());
        }
    });

    pc_->setLocalDescription(rtc::Description::Type::Offer);
    return true;
}
```

**startAsViewer — wait for track, set up depacketizer:**
```cpp
bool WebRtcSession::startAsViewer() {
    pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
        auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
        track->setMediaHandler(depacketizer);
        track->onMessage([this](auto data) {
            if (auto* bin = std::get_if<rtc::binary>(&data)) {
                std::lock_guard<std::mutex> lock(cbMutex_);
                if (onVideo_) onVideo_(
                    reinterpret_cast<const uint8_t*>(bin->data()), bin->size());
            }
        }, nullptr);
        videoTrack_ = track;
    });
    pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        dataChannel_ = dc;
        dc->onMessage([this](auto data) {
            if (auto* bin = std::get_if<rtc::binary>(&data)) {
                std::lock_guard<std::mutex> lock(cbMutex_);
                if (onData_) onData_(
                    reinterpret_cast<const uint8_t*>(bin->data()), bin->size());
            }
        });
    });
    return true;
}
```

**sendVideo / sendData:**
```cpp
bool WebRtcSession::sendVideo(const uint8_t* nalData, size_t size) {
    if (!videoTrack_ || !videoTrack_->isOpen()) return false;
    videoTrack_->send(reinterpret_cast<const std::byte*>(nalData), size);
    return true;
}

bool WebRtcSession::sendData(const uint8_t* data, size_t size) {
    if (!dataChannel_ || !dataChannel_->isOpen()) return false;
    dataChannel_->send(reinterpret_cast<const std::byte*>(data), size);
    return true;
}
```

**onRemoteDescription / onRemoteCandidate:**
```cpp
void WebRtcSession::onRemoteDescription(const std::string& sdp, const std::string& type) {
    pc_->setRemoteDescription(rtc::Description(sdp, type));
}

void WebRtcSession::onRemoteCandidate(const std::string& candidate, const std::string& sdpMid) {
    pc_->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
}
```

- [ ] **Step 3: Add CMake target**

```cmake
# WebRTC integration (libdatachannel)
add_library(omnidesk_webrtc STATIC
    src/webrtc/webrtc_session.cpp
)
target_link_libraries(omnidesk_webrtc PUBLIC
    omnidesk_core omnidesk_signaling datachannel-static
)
```

- [ ] **Step 4: Build**

Run: `cmake --build build-webrtc --target omnidesk_webrtc`

- [ ] **Step 5: Commit**

```bash
git add src/webrtc/webrtc_session.h src/webrtc/webrtc_session.cpp CMakeLists.txt
git commit -m "feat: add WebRtcSession — PeerConnection with H264 track + DataChannel"
```

---

### Task 6: Modify HostSession and ViewerSession

**Files:**
- Modify: `src/session/host_session.h`
- Modify: `src/session/host_session.cpp`
- Modify: `src/session/viewer_session.h`
- Modify: `src/session/viewer_session.cpp`

Minimal changes — keep encode/decode threads, just change the send/receive endpoints.

- [ ] **Step 1: Update HostSession SendCallback**

In `host_session.h`, the `SendCallback` type stays: `using SendCallback = std::function<void(const EncodedPacket&)>;`

No structural changes needed in HostSession itself. The `App` class will set the SendCallback to call `webrtcSession->sendVideo(packet.data.data(), packet.data.size())`. The existing serialization in `sendVideoPacket()` moves to the App's callback.

Actually, the HostSession interface can stay as-is. The change is in how App wires the callback.

- [ ] **Step 2: Update ViewerSession**

ViewerSession's `onVideoPacket(const EncodedPacket&)` interface stays. The change is that App feeds it from WebRTC's video callback instead of TcpChannel. However, the WebRTC callback delivers raw H.264 NAL units, not `EncodedPacket`. Add a simpler method:

```cpp
// In viewer_session.h, add:
void onNalUnit(const uint8_t* data, size_t size);
```

```cpp
// In viewer_session.cpp:
void ViewerSession::onNalUnit(const uint8_t* data, size_t size) {
    Frame decoded;
    if (decoder_->decode(data, size, decoded)) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        pendingFrame_ = std::move(decoded);
        hasNewFrame_ = true;
        framesDecoded_++;
    }
}
```

- [ ] **Step 3: Remove transport includes from session files**

Remove any `#include "transport/..."` from host_session and viewer_session files. They should only need `core/types.h` and codec includes.

- [ ] **Step 4: Build**

Run: `cmake --build build-webrtc --target omnidesk_session`

- [ ] **Step 5: Commit**

```bash
git add src/session/host_session.h src/session/host_session.cpp
git add src/session/viewer_session.h src/session/viewer_session.cpp
git commit -m "refactor: update sessions for WebRTC — add onNalUnit, remove transport deps"
```

---

### Task 7: Rewrite App class for WebRTC

**Files:**
- Modify: `src/ui/app.h`
- Modify: `src/ui/app.cpp`

Replace TcpChannel/relay logic with WebRtcSession.

- [ ] **Step 1: Update app.h**

Remove:
- `#include "transport/tcp_channel.h"`, `#include "transport/protocol.h"`
- `TcpChannel dataListener_`, `std::unique_ptr<TcpChannel> dataChannel_`, `dataChannelMutex_`
- `dataAcceptThread_`, `dataRecvThread_`, `dataRunning_`
- `relayMode_`, `relayPeerId_`
- `sendVideoPacket()`, `viewerReceiveLoop()`, `getLocalIp()`

Add:
```cpp
#include "webrtc/webrtc_session.h"

// In AppConfig:
std::string turnServer;
std::string turnUser;
std::string turnPassword;

// In App:
std::unique_ptr<WebRtcSession> webrtcSession_;
```

- [ ] **Step 2: Rewrite connection flow in app.cpp**

**Host accept:**
```cpp
void App::handleConnectionAccept(const UserID& viewerId) {
    hostSession_ = std::make_unique<HostSession>();
    hostSession_->setSendCallback([this](const EncodedPacket& pkt) {
        if (webrtcSession_) {
            webrtcSession_->sendVideo(pkt.data.data(), pkt.data.size());
        }
    });
    hostSession_->start(config_.encoder, config_.capture);

    WebRtcConfig wrtcCfg;
    wrtcCfg.turnServer = config_.turnServer;
    wrtcCfg.turnUser = config_.turnUser;
    wrtcCfg.turnPassword = config_.turnPassword;

    webrtcSession_ = std::make_unique<WebRtcSession>(signaling_.get(), viewerId, wrtcCfg);
    webrtcSession_->setOnConnected([this]() {
        queueAction([this]() {
            state_ = AppState::SESSION_HOST;
            hostSession_->requestKeyFrame();
        });
    });
    webrtcSession_->setOnDisconnected([this]() {
        queueAction([this]() { disconnectSession(); });
    });
    webrtcSession_->setOnData([this](const uint8_t* data, size_t size) {
        // Handle input events from viewer
    });
    webrtcSession_->startAsHost();
}
```

**Viewer connect:**
```cpp
void App::connectToPeer(const std::string& peerId) {
    viewerSession_ = std::make_unique<ViewerSession>();
    viewerSession_->start();

    WebRtcConfig wrtcCfg;
    wrtcCfg.turnServer = config_.turnServer;
    wrtcCfg.turnUser = config_.turnUser;
    wrtcCfg.turnPassword = config_.turnPassword;

    UserID targetId;
    targetId.id = peerId;

    webrtcSession_ = std::make_unique<WebRtcSession>(signaling_.get(), targetId, wrtcCfg);
    webrtcSession_->setOnConnected([this]() {
        queueAction([this]() { state_ = AppState::SESSION_VIEWER; });
    });
    webrtcSession_->setOnDisconnected([this]() {
        queueAction([this]() { disconnectSession(); });
    });
    webrtcSession_->setOnVideo([this](const uint8_t* data, size_t size) {
        if (viewerSession_) viewerSession_->onNalUnit(data, size);
    });
    webrtcSession_->setOnData([this](const uint8_t* data, size_t size) {
        // Handle cursor updates from host
    });
    webrtcSession_->startAsViewer();
}
```

**Wire SDP/ICE signaling callbacks in init():**
```cpp
signaling_->onSdpOffer([this](const SdpMessage& msg) {
    queueAction([this, msg]() {
        if (webrtcSession_) webrtcSession_->onRemoteDescription(msg.sdp, "offer");
    });
});
signaling_->onSdpAnswer([this](const SdpMessage& msg) {
    queueAction([this, msg]() {
        if (webrtcSession_) webrtcSession_->onRemoteDescription(msg.sdp, "answer");
    });
});
signaling_->onIceCandidate([this](const IceCandidate& ice) {
    queueAction([this, ice]() {
        if (webrtcSession_) webrtcSession_->onRemoteCandidate(ice.candidate, ice.sdpMid);
    });
});
```

**disconnectSession:**
```cpp
void App::disconnectSession() {
    if (webrtcSession_) webrtcSession_->close();
    webrtcSession_.reset();
    hostSession_.reset();
    viewerSession_.reset();
    state_ = AppState::DASHBOARD;
}
```

Remove: `sendVideoPacket()`, `viewerReceiveLoop()`, `getLocalIp()`, all `dataChannel_`/`dataListener_`/relay code.

- [ ] **Step 3: Update CMakeLists.txt session and UI deps**

In `omnidesk_session` link list, replace `omnidesk_transport` with `omnidesk_webrtc`:
```cmake
target_link_libraries(omnidesk_session PUBLIC
    omnidesk_core omnidesk_capture omnidesk_diff
    omnidesk_codec omnidesk_webrtc omnidesk_signaling
    omnidesk_render omnidesk_input
)
```

- [ ] **Step 4: Build everything**

Run: `cmake --build build-webrtc`
Expected: Full build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/ui/app.h src/ui/app.cpp CMakeLists.txt
git commit -m "feat: rewrite App for WebRTC — replace P2P/relay with libdatachannel"
```

---

### Task 8: Smoke test and cleanup

- [ ] **Step 1: Verify no old transport references**

```bash
grep -r "transport/" src/ --include="*.h" --include="*.cpp"
```
Expected: No matches.

- [ ] **Step 2: Build clean**

```bash
rm -rf build-webrtc && cmake -B build-webrtc -G Ninja && cmake --build build-webrtc
```

- [ ] **Step 3: Run executable**

Launch `./build-webrtc/omnidesk24` — verify it starts, connects to signaling, shows dashboard.

- [ ] **Step 4: Connection test**

If signaling server available: connect two instances, verify SDP exchange and ICE connection.

- [ ] **Step 5: Commit any fixes**

```bash
git add -A && git commit -m "fix: cleanup from smoke test"
```
