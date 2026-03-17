# WebRTC Migration Design (Revised — libdatachannel)

**Date:** 2026-03-18 (revised)
**Status:** Approved

## Summary

Replace OmniDesk24's custom transport layer (TCP/UDP channels, FEC, congestion control, NAT hole punching) with libdatachannel for WebRTC connectivity. **Keep the entire existing codec pipeline** (OpenH264, NVENC, VAAPI, MF, codec_factory) — libdatachannel handles transport only, not encoding/decoding. Send pre-encoded H.264 NAL units over WebRTC media tracks using libdatachannel's H264 RTP packetizer.

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Scope | Transport replacement | Replace custom transport with WebRTC. Keep all codecs. |
| WebRTC library | libdatachannel | CMake-native, builds with MinGW GCC, lightweight (~15K LOC), supports PeerConnection + DataChannel + H264 RTP tracks |
| Integration approach | Encode yourself, send via rtc::Track | Existing IEncoder produces H.264 NALs → H264RtpPacketizer → rtc::Track → network. Reverse on viewer side. |
| Signaling | Adapt existing server | Add SDP/ICE message types to existing JSON/TCP protocol. Keep user registration, heartbeats, reconnection. |
| TURN | Configurable URL | Support both self-hosted (coturn) and cloud providers. Replaces custom relay mode. |
| OpenH264 | **Kept** | libdatachannel has no built-in codecs. OpenH264 remains the software fallback. |
| Video delivery | WebRTC media track (rtc::Track) | H.264 NAL units sent via H264RtpPacketizer over SRTP |
| Control messages | WebRTC DataChannel | Input events, clipboard, cursor updates on a reliable/ordered DataChannel. |
| Platform | Windows first | Linux later. |
| Custom transport | Delete (except TcpChannel for signaling) | `src/transport/` removed. libdatachannel replaces UDP/TCP data, FEC, congestion, hole punching. |

## Architecture Overview

### What changes, what stays

| Layer | Current | After |
|-------|---------|-------|
| **Capture** (`src/capture/`) | DXGI, X11, PipeWire | Kept as-is |
| **Diff detection** (`src/diff/`) | Dirty rect detection | Kept as-is |
| **Codec** (`src/codec/`) | IEncoder/IDecoder + OpenH264 + NVENC/VAAPI/MF + codec_factory | **Kept as-is** — all encoders, decoders, factory, rate control, quality tuner |
| **Transport** (`src/transport/`) | TCP/UDP, FEC, congestion, hole punch | **Deleted** — libdatachannel replaces it. TcpChannel moved to signaling. |
| **Signaling** (`src/signaling/`) | Custom JSON/TCP protocol | **Adapted** — add SDP/ICE message types, remove relay |
| **Session** (`src/session/`) | HostSession + ViewerSession | **Modified** — keep encode/decode threads, replace send/receive with rtc::Track and rtc::DataChannel |
| **Render** (`src/render/`) | OpenGL I420 to RGB | Kept as-is |
| **UI** (`src/ui/`) | ImGui + GLFW | **Modified** — remove TcpChannel/relay, add TURN config, wire WebRtcSession |
| **Input** (`src/input/`) | Input handler, cursor predictor | Kept as-is |
| **Core** (`src/core/`) | Types, logging, threading | **Minor** — absorb PeerAddress/InputEvent/InputType from transport/protocol.h |

### Data flow (host)

```
DXGI Capture -> Frame -> IEncoder (NVENC/OpenH264) -> EncodedPacket (H.264 NALs)
    -> rtc::Track + H264RtpPacketizer -> ICE/DTLS-SRTP -> network
```

### Data flow (viewer)

```
network -> ICE/DTLS-SRTP -> rtc::Track + H264RtpDepacketizer -> H.264 NALs
    -> IDecoder (OpenH264) -> Frame -> GlRenderer -> ImGui
```

### Control flow (both directions)

```
InputEvent / Clipboard / CursorUpdate -> serialize -> rtc::DataChannel (reliable/ordered) -> deserialize
```

## New Module: `src/webrtc/`

### `webrtc_session.h/cpp` — PeerConnection lifecycle

- Creates `rtc::PeerConnection` with ICE/TURN config
- Creates `rtc::Track` with `rtc::H264RtpPacketizer` (host) or `rtc::H264RtpDepacketizer` (viewer)
- Creates `rtc::DataChannel` (reliable/ordered) for control messages
- Manages SDP offer/answer exchange via signaling client
- Trickles ICE candidates via signaling client
- Exposes: `sendVideo(const uint8_t* nal, size_t size)`, `sendData(...)`, callbacks for received video/data/connection state
- Implements `pc->onLocalDescription()`, `pc->onLocalCandidate()`, `pc->onStateChange()`, `pc->onTrack()`, `pc->onDataChannel()`

### libdatachannel API usage

```cpp
#include <rtc/rtc.hpp>

// Host: create track with H264 packetizer
rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
media.addH264Codec(96);
media.addSSRC(ssrc, "video-stream");
auto track = pc->addTrack(media);

auto session = std::make_shared<rtc::RtpPacketizationConfig>(
    ssrc, "video-stream", 96, rtc::H264RtpPacketizer::defaultClockRate);
auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
    rtc::NalUnit::Separator::LongStartSequence, session);
track->setMediaHandler(packetizer);

// Send encoded H.264 NAL units directly
track->send(reinterpret_cast<const std::byte*>(nal_data), nal_size);

// Viewer: receive on track callback
pc->onTrack([](std::shared_ptr<rtc::Track> track) {
    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
    track->setMediaHandler(depacketizer);
    track->onMessage([](rtc::message_variant data) {
        // H.264 NAL unit ready for decoding
    }, nullptr);
});
```

## Signaling Adaptation

Identical to original spec — add `sdp_offer`, `sdp_answer`, `ice_candidate` JSON messages. Remove `sendRelayData()` and relay handler.

### Connection flow

```
1. Viewer -> connect_request -> Host
2. Host -> connect_accept -> Viewer
3. Host creates PeerConnection, generates offer (automatic with libdatachannel)
4. Host -> sdp_offer -> Viewer (via onLocalDescription callback)
5. Viewer creates PeerConnection, sets remote description, generates answer
6. Viewer -> sdp_answer -> Host
7. Both trickle ICE candidates via ice_candidate messages
8. ICE connects (direct P2P or via TURN)
9. Video track + DataChannel open
```

## Session Layer Changes

### HostSession

- **Keeps everything:** capture thread, encode thread, IEncoder, RingBuffer, QualityTuner, AdaptiveBitrateController, dirty rect detection
- **Changes SendCallback:** instead of serializing to TcpChannel, calls `webrtcSession->sendVideo(encodedPacket.data)` to send H.264 NALs via rtc::Track

### ViewerSession

- **Keeps everything:** decode thread, IDecoder, GlRenderer, CursorOverlay, SharpeningFilter, CursorPredictor
- **Changes onVideoPacket():** receives H.264 NAL units from WebRTC track callback instead of TcpChannel

### App class

- **Remove:** `TcpChannel dataListener_/dataChannel_`, `dataAcceptThread_`, `dataRecvThread_`, `relayMode_`, `relayPeerId_`, `sendVideoPacket()`, `viewerReceiveLoop()`
- **Add:** `std::unique_ptr<WebRtcSession> webrtcSession_`
- **Add to AppConfig:** `turnServer`, `turnUser`, `turnPassword`
- After `connect_accept`, create WebRtcSession and start SDP exchange
- WebRtcSession video callback feeds ViewerSession::onVideoPacket()
- WebRtcSession data callback dispatches input/cursor/clipboard messages

## Build System

### libdatachannel integration

```cmake
include(FetchContent)
FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/nickhutchinson/libdatachannel.git
    GIT_TAG        v0.22.7
    GIT_SHALLOW    TRUE
)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)   # Enable media track support
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE) # We don't need WebSocket
FetchContent_MakeAvailable(libdatachannel)
```

This builds as a static library with MinGW GCC — no prebuilt binaries needed.

### CMake target changes

| Target | Change |
|--------|--------|
| `omnidesk_codec` | **No change** — keep all sources |
| `omnidesk_transport` | **Delete entirely** |
| `omnidesk_webrtc` | **New** — `webrtc_session.cpp`. Links `datachannel-static`, `omnidesk_signaling`. |
| `omnidesk_signaling` | Remove dependency on `omnidesk_transport`. Absorb `TcpChannel`. |
| `omnidesk_session` | Replace `omnidesk_transport` with `omnidesk_webrtc` in link list |
| `omnidesk24` | No change to direct deps (inherits via omnidesk_session) |
| `omnidesk24-server` | Remove `omnidesk_transport` dependency |

## Deletion Inventory

### Deleted

- `src/transport/` entire directory **except** `tcp_channel.h/cpp` (moved to signaling)
- That's it. No codec files deleted.

### Relocated

- `tcp_channel.h/cpp` → `src/signaling/`
- `ControlHeader`, serialization helpers → `src/signaling/wire_format.h`
- `PeerAddress`, `InputEvent`, `InputType` → `src/core/types.h`

### Total removed

~2000 LOC (transport only, minus tcp_channel which is relocated).

## Key Advantage Over libwebrtc Plan

- **No codec changes** — entire codec pipeline untouched
- **No CaptureTrackSource, VideoEncoderFactory, VideoDecoderFactory, VideoSink** — none needed
- **Session architecture preserved** — capture/encode/decode threads stay
- **Builds in seconds** — FetchContent, no prebuilt binaries
- **MinGW compatible** — no compiler switch needed
