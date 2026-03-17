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
| **Input** (`src/input/`) | Input handler, cursor predictor | Kept as-is |
| **Core** (`src/core/`) | Types, logging, threading | Kept — types.h absorbs `InputEvent`, `InputType`, `PeerAddress` from transport/protocol.h before transport deletion; trimmed of transport-specific structs (`ControlHeader`, `VideoHeader`, `MessageType`) |

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

- Extends `rtc::AdaptedVideoTrackSource` (provides adaptation, timestamping, and thread-safe frame delivery)
- Capture thread calls `OnFrame(webrtc::VideoFrame)` to push frames
- Converts `Frame` (BGRA) to `webrtc::VideoFrame` (I420 or NV12) before calling `OnFrame()`
- Respects WebRTC's `AdaptVideoFormat` hints for resolution/FPS adaptation via inherited `AdaptFrame()`

### `video_encoder_wrapper.h/cpp` — Hardware encoder bridge

- Implements `webrtc::VideoEncoder`
- Wraps existing NVENC encoder (later VAAPI/MF)
- `InitEncode()` creates NVENC encoder with WebRTC's codec settings
- `Encode()` encodes `webrtc::VideoFrame`, delivers result asynchronously via `EncodedImageCallback::OnEncodedImage()` (not a return value)
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

- **Keeps:** capture thread, dirty region detection, content classifier
- **Loses:** encode thread, `IEncoder`, `RingBuffer`, `SendCallback`, `EncodedPacket` output, `QualityTuner`, `AdaptiveBitrateController`
- **New role:** Captures frames and pushes them into `CaptureTrackSource`. WebRTC owns the encode pipeline.
- `requestKeyFrame()` signals through the encoder wrapper
- `onQualityReport()` removed — WebRTC RTCP receiver reports handle this automatically
- `AdaptiveBitrateController` removed — WebRTC `BitrateAllocator` + `SetRates()` replaces it
- `QualityTuner` removed — `webrtc::VideoEncoder::Encode()` does not accept per-region QP maps. Content classification info is retained for potential future use (e.g., passing hints via `webrtc::VideoFrame::set_video_frame_metadata()`) but is not wired into the encoder wrapper in the initial implementation

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

## libwebrtc Threading Model

libwebrtc creates and enforces three internal threads with strict affinity:

- **Signaling thread** — all `PeerConnection` API calls (create offer, set description, add track) must happen here
- **Worker thread** — media processing, encoder/decoder calls
- **Network thread** — ICE, DTLS, packet send/receive

**Integration plan:**

- `PeerConnectionFactory::Create()` is called once at app startup, on the main/UI thread. Pass `nullptr` for all three thread params to let libwebrtc create and manage its own threads.
- `WebRtcSession` posts all `PeerConnection` API calls to libwebrtc's signaling thread via `rtc::Thread::Invoke()` or `PostTask()`. The app's main thread never calls `PeerConnection` methods directly.
- `CaptureTrackSource::OnFrame()` is called from the app's capture thread. This is safe — `AdaptedVideoTrackSource::OnFrame()` is thread-safe and internally dispatches to the worker thread.
- `VideoEncoderWrapper::Encode()` is called by libwebrtc on its worker thread. The NVENC encoder must be initialized and used from that thread (NVENC is not thread-safe — all calls from the same thread are fine).
- `VideoSink::OnFrame()` is called by libwebrtc on its worker/decode thread. It queues the frame and the GL upload happens on the main thread via `processOnGlThread()` (existing pattern, no change).
- DataChannel `OnMessage()` callbacks arrive on the network thread. Messages are queued and dispatched on the main thread via the existing `queueAction()` pattern in `App`.

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
| `omnidesk_signaling` | Remove dependency on `omnidesk_transport`. Move `TcpChannel` (tcp_channel.h/cpp) and serialization helpers (`writeU16`/`readU16`/etc.) into `src/signaling/` as private implementation files. The signaling layer is the only remaining consumer of raw TCP sockets. |
| `omnidesk_session` | Depends on `omnidesk_webrtc` instead of `omnidesk_transport` + `omnidesk_codec` |
| `omnidesk24` | Links `omnidesk_session`, `omnidesk_webrtc`, `omnidesk_ui` |
| `omnidesk24-server` | Links `omnidesk_signaling` (which now contains its own TCP impl). Remove dependency on `omnidesk_transport`. Also fix `database.h` which includes `transport/protocol.h` — relocate needed types. |

libwebrtc is statically linked — single-exe property preserved. BoringSSL bundled inside libwebrtc. No new DLL dependencies.

**BoringSSL conflict note:** libwebrtc bundles BoringSSL. If the signaling TCP layer or any future dependency links OpenSSL, there will be symbol conflicts. The signaling layer must not introduce an OpenSSL dependency — use plain TCP (no TLS on the signaling socket) or rely on libwebrtc's BoringSSL if TLS is needed later.

### Files deleted from build

- `cmake/FetchOpenH264.cmake`
- All `src/transport/` sources
- `src/codec/openh264_loader.h/cpp`
- `src/codec/openh264_encoder.h/cpp`
- `src/codec/openh264_decoder.h/cpp`
- `src/codec/codec_factory.h/cpp`

## Deletion Inventory

### Entire directories removed

- `src/transport/` — udp_channel, fec, congestion, hole_punch, protocol.h (~2500 LOC). Note: `tcp_channel.h/cpp` is moved to `src/signaling/` before deletion (signaling still needs raw TCP).

### Individual files removed

- `src/codec/openh264_loader.h/cpp`
- `src/codec/openh264_encoder.h/cpp`
- `src/codec/openh264_decoder.h/cpp`
- `src/codec/codec_factory.h/cpp`
- `src/codec/rate_control.h/cpp` (AdaptiveBitrateController — replaced by WebRTC bandwidth estimation)
- `src/codec/quality_tuner.h/cpp` (QualityTuner — webrtc::VideoEncoder API does not support per-region QP)
- `cmake/FetchOpenH264.cmake`

### Relocated before transport deletion

- `InputEvent`, `InputType` structs + serialization — moved from `transport/protocol.h` to `core/types.h`
- `PeerAddress` struct — moved from `transport/protocol.h` to `core/types.h`
- `writeU16`, `readU16`, `writeU32`, `readU32` helpers — moved to `src/signaling/` (for ControlHeader framing) and `core/types.h` (for DataChannel message serialization)
- `ControlHeader` — moved to `src/signaling/` (still used for signaling wire format)
- `tcp_channel.h/cpp` — moved to `src/signaling/` (only remaining TCP consumer)

### Removed from `core/types.h`

- `EncodedPacket` — WebRTC uses `webrtc::EncodedImage` internally
- `QualityReport` — WebRTC RTCP handles this

### Kept in `core/types.h`

- `Frame`, `PixelFormat`, `Rect`, `RegionInfo`, `ContentType` — capture layer uses these
- `CursorInfo` — serialized over DataChannel
- `InputEvent`, `InputType` — relocated from `transport/protocol.h`, serialized over DataChannel
- `PeerAddress` — relocated from `transport/protocol.h`, used by signaling
- `EncoderConfig`, `CaptureConfig`, `MonitorInfo` — configuration
- `UserID`, `EncoderInfo` — signaling/UI

### Removed from `transport/protocol.h` before deleting

- `MessageType` enum — replaced by DataChannel message type byte
- `VideoHeader` — WebRTC handles framing
- (Note: `ControlHeader`, `TcpChannel`, and serialization helpers are relocated, not deleted — see "Relocated" section above)

### Total removed

~3200 LOC across transport + OpenH264 codec + rate control + quality tuner.
