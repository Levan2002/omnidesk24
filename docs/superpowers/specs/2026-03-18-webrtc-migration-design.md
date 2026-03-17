# OpenH264 to WebRTC Migration Design

**Date:** 2026-03-18
**Status:** Approved

## Summary

Replace OmniDesk24's custom transport layer (TCP/UDP channels, FEC, congestion control, NAT hole punching) and OpenH264 software codec with Google's libwebrtc. Keep the existing screen capture, hardware encoding, hardware decoding, and OpenGL rendering pipelines. Use WebRTC as the transport and media negotiation layer, feeding pre-encoded frames via custom `webrtc::VideoEncoder`/`VideoDecoder` wrappers.

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Scope | Full replacement | Replace transport, NAT traversal, codec negotiation, and software codec |
| WebRTC library | libwebrtc (Google) | Reference implementation, most complete, battle-tested |
| Integration approach | Custom VideoEncoder/Decoder wrapper | Hardware encoders wrap into `webrtc::VideoEncoder`. WebRTC controls pacing, FEC, bandwidth estimation natively. Falls back to libwebrtc built-in software codecs. |
| Signaling | Adapt existing server | Add SDP/ICE message types to existing JSON/TCP protocol. Keep user registration, heartbeats, reconnection. |
| TURN | Configurable URL | Support both self-hosted (coturn) and cloud providers. Replaces custom relay mode. |
| OpenH264 | Dropped | libwebrtc's built-in software codecs serve as fallback when no hardware encoder is available. |
| Video delivery | WebRTC media track | Video on media track with WebRTC congestion control. |
| Control messages | WebRTC DataChannel | Input events, clipboard, cursor updates on a reliable/ordered DataChannel. |
| Platform | Windows first | Linux support deferred to reduce initial build complexity. |
| Custom transport | Delete entirely | `src/transport/` removed. WebRTC ICE/DTLS-SRTP/SCTP replaces all of it. |

## Architecture Overview

### What changes, what stays

| Layer | Current | After |
|-------|---------|-------|
| **Capture** (`src/capture/`) | DXGI, X11, PipeWire | Kept as-is |
| **Diff detection** (`src/diff/`) | Dirty rect detection | Kept — feeds ROI hints to encoder |
| **Codec** (`src/codec/`) | IEncoder/IDecoder + OpenH264 + NVENC/VAAPI/MF | Rewritten — hardware encoders wrapped in `webrtc::VideoEncoder`/`VideoDecoder`. OpenH264 + codec_factory deleted. |
| **Transport** (`src/transport/`) | TCP/UDP, FEC, congestion, hole punch | Deleted entirely — WebRTC ICE/DTLS-SRTP replaces it |
| **Signaling** (`src/signaling/`) | Custom JSON/TCP protocol | Adapted — same server, new message types for SDP + ICE candidates |
| **Session** (`src/session/`) | HostSession + ViewerSession | Heavily modified — orchestrate `webrtc::PeerConnection` instead of raw channels |
| **Render** (`src/render/`) | OpenGL I420 to RGB | Kept as-is |
| **UI** (`src/ui/`) | ImGui + GLFW | Modified — remove TcpChannel/relay references, add TURN config |
| **Core** (`src/core/`) | Types, logging, threading | Kept — types.h trimmed of transport-specific structs |

### Data flow (host)

```
DXGI Capture -> raw Frame -> CaptureTrackSource -> webrtc::VideoEncoder (NVENC wrapper)
    -> WebRTC RTP packetizer -> ICE/DTLS-SRTP -> network
```

### Data flow (viewer)

```
network -> ICE/DTLS-SRTP -> WebRTC depacketizer -> webrtc::VideoDecoder (HW wrapper)
    -> VideoSink -> GlRenderer -> ImGui
```

### Control flow (both directions)

```
InputEvent / Clipboard / CursorUpdate -> serialize -> DataChannel (reliable/ordered) -> deserialize
```

## New Module: `src/webrtc/`

### `webrtc_session.h/cpp` — PeerConnection lifecycle

- Creates `PeerConnectionFactory` (once, app-wide)
- Registers custom encoder/decoder factories with hardware backends
- Creates `PeerConnection` per session with ICE/TURN config
- Manages SDP offer/answer exchange via signaling client
- Trickles ICE candidates via signaling client
- Creates the video track from `CaptureTrackSource` and adds it
- Creates a reliable/ordered DataChannel for control messages
- Exposes callbacks: `onConnected()`, `onDisconnected()`, `onDataMessage()`
- Implements `webrtc::PeerConnectionObserver` for ICE state changes and track events

### `capture_track_source.h/cpp` — Video source

- Implements `rtc::VideoSourceInterface`
- Capture thread pushes raw `Frame` into it
- Converts `Frame` (BGRA) to `webrtc::VideoFrame` (I420 or NV12)
- WebRTC pulls frames from here into the encoder pipeline
- Respects WebRTC's `AdaptVideoFormat` hints for resolution/FPS adaptation

### `video_encoder_wrapper.h/cpp` — Hardware encoder bridge

- Implements `webrtc::VideoEncoder`
- Wraps existing NVENC encoder (later VAAPI/MF)
- `InitEncode()` creates NVENC encoder with WebRTC's codec settings
- `Encode()` encodes `webrtc::VideoFrame`, returns `webrtc::EncodedImage` with H.264 NAL data
- `SetRates()` forwards bitrate/FPS targets from WebRTC's bandwidth estimator to NVENC
- Keyframe requests handled via `Encode()` frame types
- Also implements `webrtc::VideoEncoderFactory` to register hardware encoders + fallback to built-in

### `video_decoder_wrapper.h/cpp` — Hardware decoder bridge

- Implements `webrtc::VideoDecoder`
- Wraps hardware decoders (NVDEC on Windows first)
- `Decode()` decodes `webrtc::EncodedImage`, delivers `webrtc::VideoFrame` via callback
- Also implements `webrtc::VideoDecoderFactory` — tries hardware first, falls back to libwebrtc built-in

### `video_sink.h/cpp` — Frame delivery to renderer

- Implements `webrtc::VideoSinkInterface<webrtc::VideoFrame>`
- Receives decoded frames from WebRTC on the decode thread
- Converts `webrtc::VideoFrame` to `Frame` (I420)
- Queues for GL upload (same `pendingFrame_` / `processOnGlThread()` pattern)

## Signaling Adaptation

### New message types

```json
{ "type": "sdp_offer",     "to": "<userId>", "sdp": "<full SDP string>" }
{ "type": "sdp_answer",    "to": "<userId>", "sdp": "<full SDP string>" }
{ "type": "ice_candidate", "to": "<userId>", "candidate": "<SDP candidate>",
  "sdpMid": "<mid>", "sdpMLineIndex": 0 }
```

### SignalingClient changes

- Add: `sendSdpOffer()`, `sendSdpAnswer()`, `sendIceCandidate()`
- Add callbacks: `onSdpOffer`, `onSdpAnswer`, `onIceCandidate`
- `handleMessage()` routes new types to callbacks
- Remove: `sendRelayData()` and `RelayDataCallback` (TURN replaces custom relay)

### SignalingServer changes

- Route `sdp_offer`, `sdp_answer`, `ice_candidate` to target user (same forwarding pattern as `connect_request`/`connect_accept`)
- Remove `handleRelayData()`

### Unchanged

- User registration, heartbeats, reconnection logic
- `connect_request` / `connect_accept` / `connect_reject` flow (user consent before WebRTC negotiation)
- TCP transport for signaling itself

### Connection flow

```
1. Viewer -> connect_request -> Host
2. Host -> connect_accept -> Viewer
3. Host creates PeerConnection, creates offer
4. Host -> sdp_offer -> Viewer
5. Viewer creates PeerConnection, sets remote description, creates answer
6. Viewer -> sdp_answer -> Host
7. Both trickle ICE candidates via ice_candidate messages
8. ICE connects (direct P2P or via TURN)
9. Video track + DataChannel open
```

## Session Layer Changes

### HostSession

- **Keeps:** capture thread, dirty region detection, content classifier, quality tuner
- **Loses:** encode thread, `IEncoder`, `RingBuffer`, `SendCallback`, `EncodedPacket` output
- **New role:** Captures frames and pushes them into `CaptureTrackSource`. WebRTC owns the encode pipeline.
- `requestKeyFrame()` signals through the encoder wrapper
- `onQualityReport()` removed — WebRTC RTCP receiver reports handle this automatically
- `AdaptiveBitrateController` removed — WebRTC `BitrateAllocator` + `SetRates()` replaces it
- Dirty rect / content classification info passed as frame metadata or ROI QP map if NVENC supports it

### ViewerSession

- **Keeps:** `GlRenderer`, `CursorOverlay`, `SharpeningFilter`, `CursorPredictor`, `processOnGlThread()`
- **Loses:** decode thread, `IDecoder`, `onVideoPacket()`, manual FPS/bitrate tracking
- **New role:** Owns a `VideoSink` that receives decoded frames from WebRTC. Sink queues frames for GL upload.
- Stats (`latencyMs`, `packetLoss`, `bitrate`) come from `webrtc::RTCStatsReport`
- `onCursorUpdate()` arrives via DataChannel

### App class

- **Remove:** `TcpChannel dataListener_/dataChannel_`, `dataAcceptThread_`, `dataRecvThread_`, `relayMode_`, `relayPeerId_`, `sendVideoPacket()`, `viewerReceiveLoop()`
- **Add:** `std::unique_ptr<WebRtcSession> webrtcSession_`
- **Add to AppConfig:** `turnServer`, `turnUser`, `turnPassword`
- After `connect_accept`, create `WebRtcSession` and start SDP exchange
- DataChannel messages dispatched to viewer's `onCursorUpdate()` / host's input handler

## Build System

### libwebrtc integration strategy

Use prebuilt libwebrtc binaries. Build libwebrtc separately via `gn`/`ninja`, archive the static lib + headers, and link as an external library.

```
cmake/
  FetchLibWebRTC.cmake    — downloads prebuilt libwebrtc for the target platform
  FindLibWebRTC.cmake     — sets include paths and link targets
```

### CMake target changes

| Target | Change |
|--------|--------|
| `omnidesk_codec` | Remove OpenH264 sources. Add `video_encoder_wrapper`, `video_decoder_wrapper`. Link `libwebrtc`. |
| `omnidesk_transport` | Delete entirely |
| `omnidesk_webrtc` | New — `webrtc_session`, `capture_track_source`, `video_sink`. Links `libwebrtc`, `omnidesk_codec`, `omnidesk_signaling`. |
| `omnidesk_signaling` | Remove dependency on `omnidesk_transport`. Fold minimal TCP into signaling directly. |
| `omnidesk_session` | Depends on `omnidesk_webrtc` instead of `omnidesk_transport` + `omnidesk_codec` |
| `omnidesk24` | Links `omnidesk_session`, `omnidesk_webrtc`, `omnidesk_ui` |

libwebrtc is statically linked — single-exe property preserved. BoringSSL bundled inside libwebrtc. No new DLL dependencies.

### Files deleted from build

- `cmake/FetchOpenH264.cmake`
- All `src/transport/` sources
- `src/codec/openh264_loader.h/cpp`
- `src/codec/openh264_encoder.h/cpp`
- `src/codec/openh264_decoder.h/cpp`
- `src/codec/codec_factory.h/cpp`

## Deletion Inventory

### Entire directories removed

- `src/transport/` — tcp_channel, udp_channel, fec, congestion, hole_punch, protocol.h (~1800 LOC)

### Individual files removed

- `src/codec/openh264_loader.h/cpp`
- `src/codec/openh264_encoder.h/cpp`
- `src/codec/openh264_decoder.h/cpp`
- `src/codec/codec_factory.h/cpp`
- `cmake/FetchOpenH264.cmake`

### Removed from `core/types.h`

- `EncodedPacket` — WebRTC uses `webrtc::EncodedImage` internally
- `QualityReport` — WebRTC RTCP handles this

### Kept in `core/types.h`

- `Frame`, `PixelFormat`, `Rect`, `RegionInfo`, `ContentType` — capture layer uses these
- `CursorInfo`, `InputEvent`, `InputType` — serialized over DataChannel
- `EncoderConfig`, `CaptureConfig`, `MonitorInfo` — configuration
- `UserID`, `PeerAddress`, `EncoderInfo` — signaling/UI

### Removed from `transport/protocol.h` before deleting

- `MessageType` enum — replaced by DataChannel message type byte
- `ControlHeader`, `VideoHeader` — WebRTC handles framing
- Serialization helpers (`writeU16`, etc.) — minimal copy kept in `core/` for DataChannel serialization

### Total removed

~2500 LOC across transport + OpenH264 codec layer.
