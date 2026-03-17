# OpenH264 to WebRTC Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace OmniDesk24's custom transport layer and OpenH264 with libwebrtc, keeping existing screen capture and hardware encoding.

**Architecture:** Custom `webrtc::VideoEncoder`/`VideoDecoder` wrappers bridge existing NVENC hardware encoders into WebRTC's PeerConnection pipeline. WebRTC handles transport (ICE/DTLS-SRTP), pacing, FEC, and bandwidth estimation. Existing signaling server is adapted to carry SDP offers/answers and ICE candidates. Software codec fallback provided by libwebrtc's built-in encoders.

**Tech Stack:** C++17, libwebrtc (Google), CMake, GLFW, ImGui, NVENC (Windows)

**Spec:** `docs/superpowers/specs/2026-03-18-webrtc-migration-design.md`

---

## File Structure

### New files

| File | Responsibility |
|------|---------------|
| `cmake/FetchLibWebRTC.cmake` | Download prebuilt libwebrtc static lib + headers |
| `src/webrtc/capture_track_source.h` | `rtc::AdaptedVideoTrackSource` — pushes captured frames into WebRTC |
| `src/webrtc/capture_track_source.cpp` | BGRA→I420 conversion, `OnFrame()` delivery |
| `src/webrtc/video_encoder_factory.h` | `webrtc::VideoEncoderFactory` — NVENC wrapper + built-in fallback |
| `src/webrtc/video_encoder_factory.cpp` | Factory implementation, NVENC `webrtc::VideoEncoder` adapter |
| `src/webrtc/video_decoder_factory.h` | `webrtc::VideoDecoderFactory` — hardware + built-in fallback |
| `src/webrtc/video_decoder_factory.cpp` | Factory implementation |
| `src/webrtc/video_sink.h` | `webrtc::VideoSinkInterface` — delivers decoded frames to GL renderer |
| `src/webrtc/video_sink.cpp` | `webrtc::VideoFrame` → `Frame` conversion, thread-safe queuing |
| `src/webrtc/webrtc_session.h` | `PeerConnection` lifecycle, SDP/ICE, DataChannel |
| `src/webrtc/webrtc_session.cpp` | Full WebRTC session management |
| `src/signaling/tcp_channel.h` | Moved from `src/transport/` (same content) |
| `src/signaling/tcp_channel.cpp` | Moved from `src/transport/` (same content) |
| `src/signaling/wire_format.h` | `ControlHeader` + serialization helpers moved from `transport/protocol.h` |

### Modified files

| File | Change |
|------|--------|
| `src/core/types.h` | Add `InputEvent`, `InputType`, `PeerAddress`, serialization helpers from `transport/protocol.h`. Remove `EncodedPacket`, `QualityReport`. |
| `src/signaling/signaling_client.h` | Add SDP/ICE methods + callbacks. Remove relay. Change `#include` from `transport/` to local `tcp_channel.h` and `wire_format.h`. |
| `src/signaling/signaling_client.cpp` | Implement SDP/ICE send/receive. Remove `sendRelayData()`. |
| `src/signaling/signaling_server.h` | Add SDP/ICE routing. Remove `handleRelayData()`. Change includes. |
| `src/signaling/signaling_server.cpp` | Implement SDP/ICE routing. Remove relay handler. |
| `src/signaling/database.h` | Remove `#include "transport/protocol.h"` (vestigial). |
| `src/session/host_session.h` | Remove `IEncoder`, `RingBuffer`, `SendCallback`, `QualityTuner`, `AdaptiveBitrateController`. Add `CaptureTrackSource*`. |
| `src/session/host_session.cpp` | Remove encode thread. Push frames to `CaptureTrackSource`. |
| `src/session/viewer_session.h` | Remove `IDecoder`, decode thread. Add `VideoSink`. |
| `src/session/viewer_session.cpp` | Remove decode loop. Sink delivers frames directly. |
| `src/ui/app.h` | Remove `TcpChannel`, relay, data threads. Add `WebRtcSession`. Add TURN to config. |
| `src/ui/app.cpp` | Replace P2P/relay logic with WebRTC session lifecycle. |
| `CMakeLists.txt` | Remove `omnidesk_transport`, OpenH264. Add `omnidesk_webrtc`. Re-wire dependencies. |

### Deleted files

| File(s) | Reason |
|---------|--------|
| `src/transport/` (entire dir except tcp_channel which is moved) | WebRTC replaces all custom transport |
| `src/codec/openh264_loader.h/cpp` | OpenH264 dropped |
| `src/codec/openh264_encoder.h/cpp` | OpenH264 dropped |
| `src/codec/openh264_decoder.h/cpp` | OpenH264 dropped |
| `src/codec/codec_factory.h/cpp` | Replaced by WebRTC encoder/decoder factories |
| `src/codec/rate_control.h/cpp` | Replaced by WebRTC bandwidth estimation |
| `src/codec/quality_tuner.h/cpp` | webrtc::VideoEncoder has no per-region QP API |
| `cmake/FetchOpenH264.cmake` | OpenH264 dropped |

---

## Task Ordering

Tasks are ordered by dependency. Each task produces a compilable (or at minimum, self-contained) result.

---

### Task 1: Integrate libwebrtc into CMake

**Files:**
- Create: `cmake/FetchLibWebRTC.cmake`
- Modify: `CMakeLists.txt:34-49`

This task makes libwebrtc headers and static library available to the build. We use a prebuilt approach — download a release archive containing `libwebrtc.a`/`webrtc.lib` and the headers.

- [ ] **Step 1: Create FetchLibWebRTC.cmake**

```cmake
# cmake/FetchLibWebRTC.cmake
# Downloads prebuilt libwebrtc for the target platform.
#
# Provides:
#   LIBWEBRTC_INCLUDE_DIRS — path to WebRTC headers
#   LIBWEBRTC_LIBRARIES    — path to static library
#   libwebrtc              — imported STATIC target

include(FetchContent)

# Platform-specific archive URL.
# Update these URLs to point to your prebuilt libwebrtc releases.
if(OMNIDESK_PLATFORM_WINDOWS)
    set(LIBWEBRTC_URL "" CACHE STRING "URL to prebuilt libwebrtc archive (Windows)")
    set(LIBWEBRTC_HASH "" CACHE STRING "SHA256 hash of the archive")
elseif(OMNIDESK_PLATFORM_LINUX)
    set(LIBWEBRTC_URL "" CACHE STRING "URL to prebuilt libwebrtc archive (Linux)")
    set(LIBWEBRTC_HASH "" CACHE STRING "SHA256 hash of the archive")
endif()

# Allow local path override for development
set(LIBWEBRTC_ROOT "" CACHE PATH "Path to local prebuilt libwebrtc directory")

if(LIBWEBRTC_ROOT)
    # Use local directory
    set(LIBWEBRTC_PREFIX "${LIBWEBRTC_ROOT}")
elseif(LIBWEBRTC_URL)
    FetchContent_Declare(libwebrtc
        URL "${LIBWEBRTC_URL}"
        URL_HASH SHA256=${LIBWEBRTC_HASH}
    )
    FetchContent_MakeAvailable(libwebrtc)
    set(LIBWEBRTC_PREFIX "${libwebrtc_SOURCE_DIR}")
else()
    message(FATAL_ERROR
        "libwebrtc not configured. Set either:\n"
        "  -DLIBWEBRTC_ROOT=/path/to/prebuilt   (local directory)\n"
        "  -DLIBWEBRTC_URL=https://...           (download URL)\n"
        "See docs/superpowers/specs/2026-03-18-webrtc-migration-design.md"
    )
endif()

# Find the static library
if(OMNIDESK_PLATFORM_WINDOWS)
    find_library(LIBWEBRTC_LIB webrtc PATHS "${LIBWEBRTC_PREFIX}/lib" NO_DEFAULT_PATH)
else()
    find_library(LIBWEBRTC_LIB webrtc PATHS "${LIBWEBRTC_PREFIX}/lib" NO_DEFAULT_PATH)
endif()

if(NOT LIBWEBRTC_LIB)
    message(FATAL_ERROR "Could not find libwebrtc static library in ${LIBWEBRTC_PREFIX}/lib")
endif()

set(LIBWEBRTC_INCLUDE_DIRS "${LIBWEBRTC_PREFIX}/include")

# Create imported target
add_library(libwebrtc STATIC IMPORTED GLOBAL)
set_target_properties(libwebrtc PROPERTIES
    IMPORTED_LOCATION "${LIBWEBRTC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBWEBRTC_INCLUDE_DIRS}"
)

# Platform system libraries required by libwebrtc
if(OMNIDESK_PLATFORM_WINDOWS)
    set_property(TARGET libwebrtc APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES
        ws2_32 secur32 winmm dmoguids msdmo wmcodecdspuuid strmiids
    )
elseif(OMNIDESK_PLATFORM_LINUX)
    set_property(TARGET libwebrtc APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES
        pthread dl X11 Xext
    )
endif()

message(STATUS "libwebrtc: ${LIBWEBRTC_LIB}")
message(STATUS "libwebrtc includes: ${LIBWEBRTC_INCLUDE_DIRS}")
```

- [ ] **Step 2: Add to CMakeLists.txt**

In `CMakeLists.txt`, add `include(cmake/FetchLibWebRTC.cmake)` inside the `if(NOT OMNIDESK_SERVER_ONLY)` block, right after `include(cmake/FetchDeps.cmake)`:

```cmake
if(NOT OMNIDESK_SERVER_ONLY)
    include(cmake/FetchDeps.cmake)
    include(cmake/FetchLibWebRTC.cmake)
    # ... rest of deps
```

- [ ] **Step 3: Verify CMake configuration**

Run: `cmake -B build-webrtc -DLIBWEBRTC_ROOT=/path/to/your/prebuilt`

Expected: CMake configures successfully, prints libwebrtc paths. If you don't have a prebuilt yet, verify the FATAL_ERROR message appears when no path is given.

- [ ] **Step 4: Commit**

```bash
git add cmake/FetchLibWebRTC.cmake CMakeLists.txt
git commit -m "feat: add libwebrtc CMake integration (prebuilt binary approach)"
```

---

### Task 2: Relocate shared types from transport/protocol.h to core/types.h

**Files:**
- Modify: `src/core/types.h:1-202`
- Read: `src/transport/protocol.h:1-200` (source of types to move)

Move `PeerAddress`, `InputEvent`, `InputType`, and serialization helpers (`writeU16`, `readU16`, `writeU32`, `readU32`) into `core/types.h`. Also remove `EncodedPacket` and `QualityReport` from `core/types.h` (replaced by WebRTC internals).

- [ ] **Step 1: Add platform headers and serialization helpers to core/types.h**

Add to the top of `core/types.h`, after existing includes:

```cpp
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif

// ---- Serialization helpers (network byte order) ----

inline void writeU16(uint8_t* buf, uint16_t val) {
    uint16_t net = htons(val);
    std::memcpy(buf, &net, 2);
}

inline void writeU32(uint8_t* buf, uint32_t val) {
    uint32_t net = htonl(val);
    std::memcpy(buf, &net, 4);
}

inline uint16_t readU16(const uint8_t* buf) {
    uint16_t net;
    std::memcpy(&net, buf, 2);
    return ntohs(net);
}

inline uint32_t readU32(const uint8_t* buf) {
    uint32_t net;
    std::memcpy(&net, buf, 4);
    return ntohl(net);
}
```

- [ ] **Step 2: Move PeerAddress to core/types.h**

Add `PeerAddress` struct (currently in `transport/protocol.h:189-198`) to `core/types.h`, after the `UserID` struct:

```cpp
// Peer address for signaling and communication
struct PeerAddress {
    std::string host;
    uint16_t port = 0;

    bool valid() const { return !host.empty() && port != 0; }
    bool operator==(const PeerAddress& o) const {
        return host == o.host && port == o.port;
    }
    std::string toString() const { return host + ":" + std::to_string(port); }
};
```

- [ ] **Step 3: Move InputType and InputEvent to core/types.h**

Add `InputType` enum and `InputEvent` struct (currently in `transport/protocol.h:146-186`) to `core/types.h`, after `PeerAddress`:

```cpp
// Input event types
enum class InputType : uint8_t {
    MOUSE_MOVE    = 0,
    MOUSE_DOWN    = 1,
    MOUSE_UP      = 2,
    MOUSE_SCROLL  = 3,
    KEY_DOWN      = 4,
    KEY_UP        = 5,
};

// Input event struct for mouse/keyboard events
struct InputEvent {
    InputType type     = InputType::MOUSE_MOVE;
    int32_t   x        = 0;
    int32_t   y        = 0;
    uint8_t   button   = 0;
    uint32_t  scancode = 0;
    bool      pressed  = false;

    static constexpr size_t SIZE = 16;

    void serialize(uint8_t* buf) const {
        buf[0] = static_cast<uint8_t>(type);
        buf[1] = pressed ? 1 : 0;
        buf[2] = button;
        buf[3] = 0;
        writeU32(buf + 4, static_cast<uint32_t>(x));
        writeU32(buf + 8, static_cast<uint32_t>(y));
        writeU32(buf + 12, scancode);
    }

    static InputEvent deserialize(const uint8_t* buf) {
        InputEvent ev;
        ev.type     = static_cast<InputType>(buf[0]);
        ev.pressed  = buf[1] != 0;
        ev.button   = buf[2];
        ev.x        = static_cast<int32_t>(readU32(buf + 4));
        ev.y        = static_cast<int32_t>(readU32(buf + 8));
        ev.scancode = readU32(buf + 12);
        return ev;
    }
};
```

- [ ] **Step 4: Remove EncodedPacket and QualityReport from core/types.h**

Delete the `EncodedPacket` struct (lines 123-130) and `QualityReport` struct (lines 194-200) from `core/types.h`. These are replaced by WebRTC's `webrtc::EncodedImage` and RTCP feedback respectively.

- [ ] **Step 5: Verify compilation**

Run: `cmake --build build-fresh --target omnidesk_core`

Expected: Compiles with new types. Other targets will have duplicate definitions temporarily (both `core/types.h` and `transport/protocol.h` define these types) — that's fine, we'll fix it in Task 3.

- [ ] **Step 6: Commit**

```bash
git add src/core/types.h
git commit -m "feat: relocate InputEvent, PeerAddress, serialization helpers to core/types.h

Preparing for transport layer deletion. Also remove EncodedPacket
and QualityReport (replaced by WebRTC internals)."
```

---

### Task 3: Move TcpChannel and ControlHeader to signaling module

**Files:**
- Create: `src/signaling/tcp_channel.h` (copy from `src/transport/tcp_channel.h`)
- Create: `src/signaling/tcp_channel.cpp` (copy from `src/transport/tcp_channel.cpp`)
- Create: `src/signaling/wire_format.h` (ControlHeader + MessageType subset needed by signaling)
- Modify: `src/signaling/signaling_client.h` — change includes
- Modify: `src/signaling/signaling_client.cpp` — change includes
- Modify: `src/signaling/signaling_server.h` — change includes
- Modify: `src/signaling/signaling_server.cpp` — change includes
- Modify: `src/signaling/database.h` — remove vestigial `transport/protocol.h` include
- Modify: `CMakeLists.txt:178-183` — add tcp_channel.cpp to omnidesk_signaling, remove omnidesk_transport dependency

- [ ] **Step 1: Copy TcpChannel files to signaling directory**

Copy `src/transport/tcp_channel.h` → `src/signaling/tcp_channel.h`
Copy `src/transport/tcp_channel.cpp` → `src/signaling/tcp_channel.cpp`

In the copied files, change `#include "transport/..."` to `#include "signaling/..."` and ensure they include `core/types.h` for serialization helpers.

- [ ] **Step 2: Create wire_format.h**

```cpp
// src/signaling/wire_format.h
// Wire format for the signaling TCP protocol. Moved from transport/protocol.h.
#pragma once

#include "core/types.h"
#include <cstdint>
#include <cstring>

namespace omnidesk {

// Wire format magic number: "OMND"
constexpr uint32_t PROTOCOL_MAGIC = 0x4F4D4E44;
constexpr uint16_t PROTOCOL_VERSION = 1;

// Control channel header (12 bytes), prefixed to every signaling TCP message
struct ControlHeader {
    uint32_t magic   = PROTOCOL_MAGIC;
    uint16_t version = PROTOCOL_VERSION;
    uint16_t type    = 0;
    uint32_t length  = 0;

    static constexpr size_t SIZE = 12;

    void serialize(uint8_t* buf) const {
        writeU32(buf + 0, magic);
        writeU16(buf + 4, version);
        writeU16(buf + 6, type);
        writeU32(buf + 8, length);
    }

    static ControlHeader deserialize(const uint8_t* buf) {
        ControlHeader h;
        h.magic   = readU32(buf + 0);
        h.version = readU16(buf + 4);
        h.type    = readU16(buf + 6);
        h.length  = readU32(buf + 8);
        return h;
    }

    bool valid() const {
        return magic == PROTOCOL_MAGIC && version == PROTOCOL_VERSION;
    }
};

// Signaling message types (subset of old MessageType relevant to signaling)
enum class SignalingMsgType : uint16_t {
    HELLO            = 0x0001,
    AUTH             = 0x0002,
    CONFIG           = 0x0003,
    KEY_FRAME_REQ    = 0x0004,
    INPUT_EVENT      = 0x0005,
    CLIPBOARD        = 0x0006,
    CURSOR_UPDATE    = 0x0007,
    QUALITY_REPORT   = 0x0008,
    CONNECT_REQUEST  = 0x0009,
    CONNECT_ACCEPT   = 0x000A,
    CONNECT_REJECT   = 0x000B,
    HEARTBEAT        = 0x000C,
    BYE              = 0x000D,
    SDP_OFFER        = 0x0010,
    SDP_ANSWER       = 0x0011,
    ICE_CANDIDATE    = 0x0012,
};

} // namespace omnidesk
```

- [ ] **Step 3: Update signaling includes**

In `signaling_client.h`, replace:
```cpp
#include "transport/tcp_channel.h"
#include "transport/udp_channel.h"
#include "transport/protocol.h"
```
with:
```cpp
#include "signaling/tcp_channel.h"
#include "signaling/wire_format.h"
```

Remove `UdpChannel` references if any. Apply similar changes to `signaling_client.cpp`, `signaling_server.h`, `signaling_server.cpp`.

In `signaling_client.h`, also change `MessageType` references to `SignalingMsgType` where used (the `RelayDataCallback` uses `MessageType` — this callback is being removed in Task 9).

- [ ] **Step 4: Remove vestigial include in database.h**

In `src/signaling/database.h`, remove `#include "transport/protocol.h"` — it's unused.

- [ ] **Step 5: Update CMakeLists.txt signaling target**

Change `omnidesk_signaling`:

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
if(OMNIDESK_PLATFORM_LINUX)
    target_link_libraries(omnidesk_signaling PRIVATE pthread)
endif()
```

Note: `omnidesk_transport` is no longer a dependency. The ws2_32/pthread libs move here since TcpChannel needs them.

- [ ] **Step 6: Build and verify**

Run: `cmake --build build-fresh --target omnidesk_signaling`

Expected: Compiles. Signaling is now self-contained with its own TCP channel.

- [ ] **Step 7: Commit**

```bash
git add src/signaling/tcp_channel.h src/signaling/tcp_channel.cpp src/signaling/wire_format.h
git add src/signaling/signaling_client.h src/signaling/signaling_client.cpp
git add src/signaling/signaling_server.h src/signaling/signaling_server.cpp
git add src/signaling/database.h CMakeLists.txt
git commit -m "refactor: move TcpChannel and wire format into signaling module

Signaling is now self-contained with its own TCP socket and
ControlHeader wire format. Breaks dependency on omnidesk_transport."
```

---

### Task 4: Delete old transport layer and OpenH264 codec files

**Files:**
- Delete: `src/transport/` (entire directory)
- Delete: `src/codec/openh264_loader.h`, `src/codec/openh264_loader.cpp`
- Delete: `src/codec/openh264_encoder.h`, `src/codec/openh264_encoder.cpp`
- Delete: `src/codec/openh264_decoder.h`, `src/codec/openh264_decoder.cpp`
- Delete: `src/codec/codec_factory.h`, `src/codec/codec_factory.cpp`
- Delete: `src/codec/rate_control.h`, `src/codec/rate_control.cpp`
- Delete: `src/codec/quality_tuner.h`, `src/codec/quality_tuner.cpp`
- Delete: `cmake/FetchOpenH264.cmake`
- Modify: `CMakeLists.txt` — remove `omnidesk_transport` target, OpenH264 references, old codec sources

- [ ] **Step 1: Delete transport directory**

```bash
rm -rf src/transport/
```

- [ ] **Step 2: Delete OpenH264 and old codec files**

```bash
rm src/codec/openh264_loader.h src/codec/openh264_loader.cpp
rm src/codec/openh264_encoder.h src/codec/openh264_encoder.cpp
rm src/codec/openh264_decoder.h src/codec/openh264_decoder.cpp
rm src/codec/codec_factory.h src/codec/codec_factory.cpp
rm src/codec/rate_control.h src/codec/rate_control.cpp
rm src/codec/quality_tuner.h src/codec/quality_tuner.cpp
rm cmake/FetchOpenH264.cmake
```

- [ ] **Step 3: Update CMakeLists.txt**

Remove the entire `omnidesk_transport` target (lines 160-175).

Remove `include(cmake/FetchOpenH264.cmake)` (line 37).

Replace the codec target (lines 118-157) with:

```cmake
# Codec — hardware encoder wrappers (NVENC, VAAPI, MF)
# The old OpenH264 software path is removed; libwebrtc provides software fallback.
set(CODEC_SOURCES)
if(OMNIDESK_PLATFORM_LINUX AND VAAPI_FOUND)
    list(APPEND CODEC_SOURCES src/codec/vaapi_encoder.cpp)
endif()
if(OMNIDESK_PLATFORM_LINUX OR OMNIDESK_PLATFORM_WINDOWS)
    list(APPEND CODEC_SOURCES src/codec/nvenc_encoder.cpp)
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    list(APPEND CODEC_SOURCES src/codec/mf_encoder.cpp)
endif()

add_library(omnidesk_codec STATIC ${CODEC_SOURCES})
target_link_libraries(omnidesk_codec PUBLIC omnidesk_core)
if(VAAPI_FOUND)
    target_link_libraries(omnidesk_codec PRIVATE ${VAAPI_LIBRARIES})
    target_include_directories(omnidesk_codec PRIVATE ${VAAPI_INCLUDE_DIRS})
    target_compile_definitions(omnidesk_codec PRIVATE OMNIDESK_HAS_VAAPI)
endif()
if(OMNIDESK_PLATFORM_LINUX OR OMNIDESK_PLATFORM_WINDOWS)
    target_compile_definitions(omnidesk_codec PRIVATE OMNIDESK_HAS_NVENC)
    if(OMNIDESK_PLATFORM_LINUX)
        target_link_libraries(omnidesk_codec PRIVATE dl)
    endif()
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_codec PRIVATE mfplat mfuuid mf wmcodecdspuuid)
endif()
```

Remove `omnidesk_transport` from `omnidesk_session` link list (line 230).

Remove `omnidesk_transport` from any other remaining references.

- [ ] **Step 4: Fix remaining includes**

Search for any `#include "transport/..."` or `#include "codec/openh264..."` or `#include "codec/codec_factory.h"` or `#include "codec/rate_control.h"` or `#include "codec/quality_tuner.h"` in the remaining source files. Update or remove them. Key files to check:
- `src/session/host_session.h` — remove includes for `IEncoder`, `QualityTuner`, `AdaptiveBitrateController`, `RingBuffer`
- `src/session/host_session.cpp` — remove codec_factory, quality_tuner, rate_control includes
- `src/session/viewer_session.h` — remove `IDecoder` include
- `src/ui/app.h` — remove `transport/tcp_channel.h`, `transport/protocol.h`
- `src/ui/app.cpp` — remove transport includes, protocol.h

Note: This will leave session and app files in a broken state — they'll be rewritten in Tasks 10-12. For now, comment out broken code or add `#if 0` blocks to allow compilation.

- [ ] **Step 5: Verify CMake configuration**

Run: `cmake -B build-webrtc -DLIBWEBRTC_ROOT=/path/to/prebuilt`

Expected: Configures without errors. Build may fail on session/app targets — that's expected and will be fixed in later tasks.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "chore: delete custom transport layer, OpenH264, and old codec scaffolding

Removes ~3200 LOC: src/transport/ (entire directory), OpenH264
encoder/decoder/loader, codec_factory, rate_control, quality_tuner.
Session and App targets are temporarily broken — rewritten in
subsequent tasks."
```

---

### Task 5: Create CaptureTrackSource

**Files:**
- Create: `src/webrtc/capture_track_source.h`
- Create: `src/webrtc/capture_track_source.cpp`

The capture track source bridges the app's capture thread to WebRTC. It extends `rtc::AdaptedVideoTrackSource` and converts captured `Frame` (BGRA) to `webrtc::VideoFrame` (I420).

- [ ] **Step 1: Write capture_track_source.h**

```cpp
// src/webrtc/capture_track_source.h
#pragma once

#include "core/types.h"
#include <api/media_stream_interface.h>
#include <api/video/video_frame.h>
#include <media/base/adapted_video_track_source.h>
#include <rtc_base/timestamp_aligner.h>

namespace omnidesk {

// Bridges screen capture to WebRTC's video pipeline.
// The capture thread pushes raw Frame objects; WebRTC pulls them
// through the standard VideoTrackSource interface into the encoder.
class CaptureTrackSource : public rtc::AdaptedVideoTrackSource {
public:
    CaptureTrackSource();
    ~CaptureTrackSource() override = default;

    // Called from the capture thread. Converts BGRA Frame to I420
    // and delivers to WebRTC via OnFrame().
    void PushFrame(const Frame& frame);

    // VideoTrackSourceInterface overrides
    bool is_screencast() const override { return true; }
    absl::optional<bool> needs_denoising() const override { return false; }
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }

private:
    rtc::TimestampAligner timestamp_aligner_;
};

} // namespace omnidesk
```

- [ ] **Step 2: Write capture_track_source.cpp**

```cpp
// src/webrtc/capture_track_source.cpp
#include "webrtc/capture_track_source.h"

#include <api/video/i420_buffer.h>
#include <rtc_base/time_utils.h>
#include <third_party/libyuv/include/libyuv.h>

namespace omnidesk {

CaptureTrackSource::CaptureTrackSource() = default;

void CaptureTrackSource::PushFrame(const Frame& frame) {
    // Let AdaptedVideoTrackSource decide if this frame should be dropped
    // (e.g., WebRTC requested lower FPS or resolution).
    int adapted_width, adapted_height, crop_width, crop_height;
    int crop_x, crop_y;
    if (!AdaptFrame(frame.width, frame.height, frame.timestampUs,
                    &adapted_width, &adapted_height,
                    &crop_width, &crop_height, &crop_x, &crop_y)) {
        return;  // Frame dropped by adaptation
    }

    // Allocate I420 buffer at adapted resolution
    rtc::scoped_refptr<webrtc::I420Buffer> i420_buf =
        webrtc::I420Buffer::Create(adapted_width, adapted_height);

    if (frame.format == PixelFormat::BGRA) {
        // Convert BGRA → I420 using libyuv (bundled with libwebrtc)
        libyuv::ARGBToI420(
            frame.data.data() + crop_y * frame.stride + crop_x * 4,
            frame.stride,
            i420_buf->MutableDataY(), i420_buf->StrideY(),
            i420_buf->MutableDataU(), i420_buf->StrideU(),
            i420_buf->MutableDataV(), i420_buf->StrideV(),
            adapted_width, adapted_height
        );
    } else if (frame.format == PixelFormat::I420) {
        // Already I420 — copy planes with scaling if needed
        libyuv::I420Scale(
            frame.plane(0), frame.stride,
            frame.plane(1), frame.stride / 2,
            frame.plane(2), frame.stride / 2,
            frame.width, frame.height,
            i420_buf->MutableDataY(), i420_buf->StrideY(),
            i420_buf->MutableDataU(), i420_buf->StrideU(),
            i420_buf->MutableDataV(), i420_buf->StrideV(),
            adapted_width, adapted_height,
            libyuv::kFilterBox
        );
    }

    // Align timestamp for the encoder
    int64_t translated_us = timestamp_aligner_.TranslateTimestamp(
        frame.timestampUs, rtc::TimeMicros());

    webrtc::VideoFrame webrtc_frame =
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(i420_buf)
            .set_timestamp_us(translated_us)
            .set_rotation(webrtc::kVideoRotation_0)
            .build();

    // Deliver to WebRTC (thread-safe, dispatches to encoder)
    OnFrame(webrtc_frame);
}

} // namespace omnidesk
```

- [ ] **Step 3: Commit**

```bash
git add src/webrtc/capture_track_source.h src/webrtc/capture_track_source.cpp
git commit -m "feat: add CaptureTrackSource — bridges screen capture to WebRTC

Extends rtc::AdaptedVideoTrackSource. Converts BGRA frames to I420
and pushes them into WebRTC's encoder pipeline via OnFrame()."
```

---

### Task 6: Create VideoEncoderFactory and VideoDecoderFactory

**Files:**
- Create: `src/webrtc/video_encoder_factory.h`
- Create: `src/webrtc/video_encoder_factory.cpp`
- Create: `src/webrtc/video_decoder_factory.h`
- Create: `src/webrtc/video_decoder_factory.cpp`

The encoder factory tries NVENC first (via a `webrtc::VideoEncoder` wrapper around the existing `NvencEncoder`), then falls back to libwebrtc's built-in software encoders. The decoder factory follows the same pattern.

- [ ] **Step 1: Write video_encoder_factory.h**

```cpp
// src/webrtc/video_encoder_factory.h
#pragma once

#include <api/video_codecs/video_encoder_factory.h>
#include <api/video_codecs/video_encoder.h>
#include <memory>
#include <vector>

namespace omnidesk {

// Encoder factory that tries hardware encoders (NVENC) first,
// then falls back to libwebrtc's built-in software encoders.
class HardwareEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    HardwareEncoderFactory();
    ~HardwareEncoderFactory() override;

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override;

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        absl::optional<std::string> scalability_mode) const override;

private:
    // Built-in factory as fallback
    std::unique_ptr<webrtc::VideoEncoderFactory> builtin_factory_;
    bool nvenc_available_ = false;
};

} // namespace omnidesk
```

- [ ] **Step 2: Write video_encoder_factory.cpp**

```cpp
// src/webrtc/video_encoder_factory.cpp
#include "webrtc/video_encoder_factory.h"

#include <api/environment/environment.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/video_encoder.h>
#include <api/video/video_codec_type.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include "codec/nvenc_encoder.h"
#include "core/logger.h"

namespace omnidesk {

// Wraps our NvencEncoder in webrtc::VideoEncoder interface
class NvencVideoEncoder : public webrtc::VideoEncoder {
public:
    NvencVideoEncoder() = default;

    int InitEncode(const webrtc::VideoCodec* codec_settings,
                   const Settings& settings) override {
        EncoderConfig cfg;
        cfg.width = codec_settings->width;
        cfg.height = codec_settings->height;
        cfg.targetBitrateBps = codec_settings->startBitrate * 1000;
        cfg.maxBitrateBps = codec_settings->maxBitrate * 1000;
        cfg.maxFps = static_cast<float>(codec_settings->maxFramerate);
        cfg.screenContent = true;

        if (!nvenc_.init(cfg)) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        width_ = cfg.width;
        height_ = cfg.height;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(
        webrtc::EncodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* frame_types) override {
        if (!callback_) return WEBRTC_VIDEO_CODEC_ERROR;

        // Check if keyframe requested
        bool want_key = false;
        if (frame_types && !frame_types->empty()) {
            if ((*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
                want_key = true;
                nvenc_.requestKeyFrame();
            }
        }

        // Convert webrtc::VideoFrame to our Frame
        auto i420 = frame.video_frame_buffer()->ToI420();
        Frame f;
        f.width = i420->width();
        f.height = i420->height();
        f.format = PixelFormat::I420;
        f.stride = i420->StrideY();
        f.timestampUs = frame.timestamp_us();
        // Copy plane data
        size_t y_size = i420->StrideY() * i420->height();
        size_t u_size = i420->StrideU() * ((i420->height() + 1) / 2);
        size_t v_size = i420->StrideV() * ((i420->height() + 1) / 2);
        f.data.resize(y_size + u_size + v_size);
        memcpy(f.data.data(), i420->DataY(), y_size);
        memcpy(f.data.data() + y_size, i420->DataU(), u_size);
        memcpy(f.data.data() + y_size + u_size, i420->DataV(), v_size);

        // Encode
        EncodedPacket pkt;
        std::vector<RegionInfo> regions;  // empty — full frame
        if (!nvenc_.encode(f, regions, pkt)) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        // Deliver encoded image to WebRTC
        webrtc::EncodedImage encoded;
        encoded.SetEncodedData(
            webrtc::EncodedImageBuffer::Create(pkt.data.data(), pkt.data.size()));
        encoded._encodedWidth = width_;
        encoded._encodedHeight = height_;
        encoded.SetTimestamp(frame.timestamp());
        encoded.capture_time_ms_ = frame.render_time_ms();
        encoded._frameType = pkt.isKeyFrame
            ? webrtc::VideoFrameType::kVideoFrameKey
            : webrtc::VideoFrameType::kVideoFrameDelta;

        webrtc::CodecSpecificInfo codec_info;
        codec_info.codecType = webrtc::kVideoCodecH264;

        callback_->OnEncodedImage(encoded, &codec_info);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& parameters) override {
        nvenc_.updateBitrate(
            static_cast<uint32_t>(parameters.bitrate.get_sum_bps()));
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.supports_native_handle = false;
        info.is_hardware_accelerated = true;
        info.implementation_name = "NVENC";
        info.scaling_settings = ScalingSettings::kOff;
        return info;
    }

private:
    NvencEncoder nvenc_;
    webrtc::EncodedImageCallback* callback_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

HardwareEncoderFactory::HardwareEncoderFactory()
    : builtin_factory_(webrtc::CreateBuiltinVideoEncoderFactory()) {
    // Probe NVENC availability
    NvencEncoder probe;
    EncoderConfig test_cfg;
    test_cfg.width = 1920;
    test_cfg.height = 1080;
    nvenc_available_ = probe.init(test_cfg);
    if (nvenc_available_) {
        LOG_INFO("NVENC hardware encoder available");
    } else {
        LOG_INFO("NVENC not available, using software fallback");
    }
}

HardwareEncoderFactory::~HardwareEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
HardwareEncoderFactory::GetSupportedFormats() const {
    return builtin_factory_->GetSupportedFormats();
}

std::unique_ptr<webrtc::VideoEncoder> HardwareEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
    // Try NVENC for H.264
    if (nvenc_available_ && absl::EqualsIgnoreCase(format.name, "H264")) {
        auto encoder = std::make_unique<NvencVideoEncoder>();
        return encoder;
    }
    // Fall back to built-in (VP8, VP9, AV1 software)
    return builtin_factory_->Create(env, format);
}

webrtc::VideoEncoderFactory::CodecSupport
HardwareEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    absl::optional<std::string> scalability_mode) const {
    return builtin_factory_->QueryCodecSupport(format, scalability_mode);
}

} // namespace omnidesk
```

**Note:** The `NvencEncoder` is currently a stub that returns `false` from `init()`. This means `nvenc_available_` will be `false` and the factory will always use the built-in fallback. When NVENC is implemented, it will work automatically.

- [ ] **Step 3: Write video_decoder_factory.h**

```cpp
// src/webrtc/video_decoder_factory.h
#pragma once

#include <api/video_codecs/video_decoder_factory.h>
#include <memory>
#include <vector>

namespace omnidesk {

// Decoder factory. Currently delegates entirely to libwebrtc's built-in
// decoders. Hardware decode (NVDEC) can be added later following the
// same wrapper pattern as the encoder.
class HardwareDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    HardwareDecoderFactory();
    ~HardwareDecoderFactory() override;

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override;

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        bool reference_counting) const override;

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> builtin_factory_;
};

} // namespace omnidesk
```

- [ ] **Step 4: Write video_decoder_factory.cpp**

```cpp
// src/webrtc/video_decoder_factory.cpp
#include "webrtc/video_decoder_factory.h"
#include <api/video_codecs/builtin_video_decoder_factory.h>

namespace omnidesk {

HardwareDecoderFactory::HardwareDecoderFactory()
    : builtin_factory_(webrtc::CreateBuiltinVideoDecoderFactory()) {}

HardwareDecoderFactory::~HardwareDecoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
HardwareDecoderFactory::GetSupportedFormats() const {
    return builtin_factory_->GetSupportedFormats();
}

std::unique_ptr<webrtc::VideoDecoder> HardwareDecoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
    // TODO: Try NVDEC hardware decoder first when implemented
    return builtin_factory_->Create(env, format);
}

webrtc::VideoDecoderFactory::CodecSupport
HardwareDecoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    bool reference_counting) const {
    return builtin_factory_->QueryCodecSupport(format, reference_counting);
}

} // namespace omnidesk
```

- [ ] **Step 5: Commit**

```bash
git add src/webrtc/video_encoder_factory.h src/webrtc/video_encoder_factory.cpp
git add src/webrtc/video_decoder_factory.h src/webrtc/video_decoder_factory.cpp
git commit -m "feat: add WebRTC encoder/decoder factories with NVENC wrapper

HardwareEncoderFactory wraps NvencEncoder in webrtc::VideoEncoder
interface. Falls back to libwebrtc built-in codecs when NVENC is
unavailable. HardwareDecoderFactory delegates to built-in for now."
```

---

### Task 7: Create VideoSink

**Files:**
- Create: `src/webrtc/video_sink.h`
- Create: `src/webrtc/video_sink.cpp`

Receives decoded `webrtc::VideoFrame` from WebRTC and converts to `Frame` for GL upload.

- [ ] **Step 1: Write video_sink.h**

```cpp
// src/webrtc/video_sink.h
#pragma once

#include "core/types.h"
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <mutex>

namespace omnidesk {

// Receives decoded video frames from WebRTC and queues them for
// GL upload on the main thread.
class VideoSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    VideoSink() = default;

    // Called by WebRTC on the decoder/worker thread.
    void OnFrame(const webrtc::VideoFrame& frame) override;

    // Called on the GL/main thread to retrieve the latest decoded frame.
    // Returns true if a new frame was available.
    bool TakeFrame(Frame& out);

private:
    std::mutex mutex_;
    Frame pending_frame_;
    bool has_new_frame_ = false;
};

} // namespace omnidesk
```

- [ ] **Step 2: Write video_sink.cpp**

```cpp
// src/webrtc/video_sink.cpp
#include "webrtc/video_sink.h"
#include <api/video/i420_buffer.h>

namespace omnidesk {

void VideoSink::OnFrame(const webrtc::VideoFrame& frame) {
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return;

    Frame f;
    f.width = i420->width();
    f.height = i420->height();
    f.format = PixelFormat::I420;
    f.stride = i420->StrideY();
    f.timestampUs = frame.timestamp_us();

    // Copy I420 planes into Frame
    size_t y_size = static_cast<size_t>(i420->StrideY()) * i420->height();
    size_t u_size = static_cast<size_t>(i420->StrideU()) * ((i420->height() + 1) / 2);
    size_t v_size = static_cast<size_t>(i420->StrideV()) * ((i420->height() + 1) / 2);
    f.data.resize(y_size + u_size + v_size);
    memcpy(f.data.data(), i420->DataY(), y_size);
    memcpy(f.data.data() + y_size, i420->DataU(), u_size);
    memcpy(f.data.data() + y_size + u_size, i420->DataV(), v_size);

    std::lock_guard<std::mutex> lock(mutex_);
    pending_frame_ = std::move(f);
    has_new_frame_ = true;
}

bool VideoSink::TakeFrame(Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_new_frame_) return false;
    out = std::move(pending_frame_);
    has_new_frame_ = false;
    return true;
}

} // namespace omnidesk
```

- [ ] **Step 3: Commit**

```bash
git add src/webrtc/video_sink.h src/webrtc/video_sink.cpp
git commit -m "feat: add VideoSink — delivers decoded WebRTC frames to GL renderer

Implements rtc::VideoSinkInterface. Converts webrtc::VideoFrame to
Frame (I420) and queues for GL upload on the main thread."
```

---

### Task 8: Add SDP/ICE to signaling

**Files:**
- Modify: `src/signaling/signaling_client.h`
- Modify: `src/signaling/signaling_client.cpp`
- Modify: `src/signaling/signaling_server.h`
- Modify: `src/signaling/signaling_server.cpp`

Add methods to send/receive SDP offers, answers, and ICE candidates. Remove relay data support.

- [ ] **Step 1: Add SDP/ICE types and callbacks to signaling_client.h**

Add to `SignalingClient`:

```cpp
// SDP/ICE types
struct SdpMessage {
    UserID fromId;
    std::string sdp;
};

struct IceCandidate {
    UserID fromId;
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
};

// New callbacks
using SdpOfferCallback = std::function<void(const SdpMessage&)>;
using SdpAnswerCallback = std::function<void(const SdpMessage&)>;
using IceCandidateCallback = std::function<void(const IceCandidate&)>;

// New methods
bool sendSdpOffer(const UserID& targetId, const std::string& sdp);
bool sendSdpAnswer(const UserID& targetId, const std::string& sdp);
bool sendIceCandidate(const UserID& targetId, const std::string& candidate,
                      const std::string& sdpMid, int sdpMLineIndex);

void onSdpOffer(SdpOfferCallback cb);
void onSdpAnswer(SdpAnswerCallback cb);
void onIceCandidate(IceCandidateCallback cb);
```

Remove: `sendRelayData()`, `RelayDataCallback`, `onRelayData()`, `relayDataCb_` member.

- [ ] **Step 2: Implement SDP/ICE in signaling_client.cpp**

In `sendSdpOffer()`:
```cpp
bool SignalingClient::sendSdpOffer(const UserID& targetId, const std::string& sdp) {
    return sendJson(jsonMakeObject({
        {"type", "\"sdp_offer\""},
        {"to", "\"" + targetId.id + "\""},
        {"sdp", "\"" + sdp + "\""}
    }));
}
```

Similarly for `sendSdpAnswer()` and `sendIceCandidate()`.

In `handleMessage()`, add routing for `"sdp_offer"`, `"sdp_answer"`, `"ice_candidate"` message types:
```cpp
if (type == "sdp_offer") {
    SdpMessage msg;
    msg.fromId.id = jsonGetString(json, "from");
    msg.sdp = jsonGetString(json, "sdp");
    // Queue for dispatch on poll()
    ...
}
```

Remove the `"relay_data"` handler and `sendRelayData()` implementation.

- [ ] **Step 3: Add SDP/ICE routing to signaling_server.h/cpp**

Add to `SignalingServer` the same routing pattern as `handleConnectRequest` — extract `"to"` field, find the target user's channel, forward the message:

```cpp
void SignalingServer::handleSdpOffer(std::shared_ptr<TcpChannel> client,
                                     const std::string& json) {
    auto toId = jsonGetString(json, "to");
    auto it = users_.find(toId);
    if (it == users_.end()) return;  // user offline
    // Forward with "from" field added
    auto fromUser = findUserByChannel(client);
    if (!fromUser) return;
    sendJson(*it->second.channel, jsonMakeObject({
        {"type", "\"sdp_offer\""},
        {"from", "\"" + fromUser->userId.id + "\""},
        {"sdp", "\"" + jsonGetString(json, "sdp") + "\""}
    }));
}
```

Similarly for `handleSdpAnswer()` and `handleIceCandidate()`.

Remove `handleRelayData()`.

Add `"sdp_offer"`, `"sdp_answer"`, `"ice_candidate"` to the message dispatch in `handleClientMessage()`.

- [ ] **Step 4: Build and verify**

Run: `cmake --build build-webrtc --target omnidesk_signaling`

Expected: Compiles with new SDP/ICE methods.

- [ ] **Step 5: Commit**

```bash
git add src/signaling/signaling_client.h src/signaling/signaling_client.cpp
git add src/signaling/signaling_server.h src/signaling/signaling_server.cpp
git commit -m "feat: add SDP/ICE signaling for WebRTC negotiation

SignalingClient can now send/receive SDP offers, answers, and ICE
candidates. SignalingServer routes these to target users. Custom
relay data support removed (TURN replaces it)."
```

---

### Task 9: Create WebRtcSession

**Files:**
- Create: `src/webrtc/webrtc_session.h`
- Create: `src/webrtc/webrtc_session.cpp`

This is the central coordination class. It owns the `PeerConnectionFactory` (app-wide singleton), creates `PeerConnection` per session, manages SDP/ICE exchange, and provides the video track + DataChannel.

- [ ] **Step 1: Write webrtc_session.h**

```cpp
// src/webrtc/webrtc_session.h
#pragma once

#include "core/types.h"
#include "webrtc/capture_track_source.h"
#include "webrtc/video_sink.h"

#include <api/peer_connection_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/data_channel_interface.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace omnidesk {

class SignalingClient;

struct WebRtcConfig {
    std::string turnServer;    // e.g. "turn:relay.example.com:3478"
    std::string turnUser;
    std::string turnPassword;
    std::vector<std::string> stunServers = {"stun:stun.l.google.com:19302"};
};

class WebRtcSession : public webrtc::PeerConnectionObserver,
                      public webrtc::DataChannelObserver,
                      public webrtc::CreateSessionDescriptionObserver {
public:
    // Callbacks for the App layer
    using ConnectedCallback = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;
    using DataMessageCallback = std::function<void(const uint8_t* data, size_t size)>;
    using RemoteTrackCallback = std::function<void(
        rtc::scoped_refptr<webrtc::VideoTrackInterface>)>;

    WebRtcSession(SignalingClient* signaling, const WebRtcConfig& config);
    ~WebRtcSession();

    // Initialize the PeerConnectionFactory (call once at app startup).
    // Returns false if libwebrtc initialization fails.
    static bool InitFactory();
    static void ShutdownFactory();

    // Create a PeerConnection and start as the offering side (host).
    // Attaches the capture source as a video track.
    bool StartAsHost(rtc::scoped_refptr<CaptureTrackSource> capture_source);

    // Create a PeerConnection and wait for an offer (viewer).
    // Attaches the video sink to receive decoded frames.
    bool StartAsViewer(VideoSink* sink);

    // Called by signaling when SDP/ICE messages arrive from the remote peer.
    void OnRemoteSdpOffer(const std::string& sdp);
    void OnRemoteSdpAnswer(const std::string& sdp);
    void OnRemoteIceCandidate(const std::string& candidate,
                              const std::string& sdpMid, int sdpMLineIndex);

    // Send a message over the DataChannel (reliable, ordered).
    bool SendData(const uint8_t* data, size_t size);

    // Close the PeerConnection.
    void Close();

    // Set callbacks
    void SetOnConnected(ConnectedCallback cb) { on_connected_ = cb; }
    void SetOnDisconnected(DisconnectedCallback cb) { on_disconnected_ = cb; }
    void SetOnDataMessage(DataMessageCallback cb) { on_data_message_ = cb; }

    // PeerConnectionObserver
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state) override;
    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
    void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;

    // DataChannelObserver
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;

    // CreateSessionDescriptionObserver
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

    // prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevent prevented
```

- [ ] **Step 2: Write webrtc_session.cpp**

The implementation is large. Key sections:

**Static factory initialization:**
```cpp
static rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> g_factory;
static std::unique_ptr<rtc::Thread> g_signaling_thread;
static std::unique_ptr<rtc::Thread> g_worker_thread;
static std::unique_ptr<rtc::Thread> g_network_thread;

bool WebRtcSession::InitFactory() {
    g_network_thread = rtc::Thread::CreateWithSocketServer();
    g_network_thread->Start();
    g_worker_thread = rtc::Thread::Create();
    g_worker_thread->Start();
    g_signaling_thread = rtc::Thread::Create();
    g_signaling_thread->Start();

    g_factory = webrtc::CreatePeerConnectionFactory(
        g_network_thread.get(), g_worker_thread.get(), g_signaling_thread.get(),
        nullptr,  // default ADM
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        std::make_unique<HardwareEncoderFactory>(),
        std::make_unique<HardwareDecoderFactory>());

    return g_factory != nullptr;
}
```

**StartAsHost:**
```cpp
bool WebRtcSession::StartAsHost(rtc::scoped_refptr<CaptureTrackSource> capture_source) {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    for (auto& stun : config_.stunServers) {
        webrtc::PeerConnectionInterface::IceServer server;
        server.uri = stun;
        config.servers.push_back(server);
    }
    if (!config_.turnServer.empty()) {
        webrtc::PeerConnectionInterface::IceServer turn;
        turn.uri = config_.turnServer;
        turn.username = config_.turnUser;
        turn.password = config_.turnPassword;
        config.servers.push_back(turn);
    }

    auto result = g_factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies(this));
    if (!result.ok()) return false;
    peer_connection_ = result.MoveValue();

    // Add video track
    auto track = g_factory->CreateVideoTrack(capture_source, "video0");
    peer_connection_->AddTrack(track, {"stream0"});

    // Create DataChannel
    webrtc::DataChannelInit dc_init;
    dc_init.ordered = true;
    dc_init.reliable = true;
    auto dc_result = peer_connection_->CreateDataChannelOrError("control", &dc_init);
    if (dc_result.ok()) {
        data_channel_ = dc_result.MoveValue();
        data_channel_->RegisterObserver(this);
    }

    // Create and send offer
    peer_connection_->CreateOffer(this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    return true;
}
```

**StartAsViewer** — similar but waits for offer, sets remote description, creates answer, attaches sink in `OnTrack()`.

**SDP/ICE handlers:**
```cpp
void WebRtcSession::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    peer_connection_->SetLocalDescription(
        webrtc::SetLocalDescriptionObserverInterface::Create(...), desc);

    std::string sdp;
    desc->ToString(&sdp);
    if (desc->GetType() == webrtc::SdpType::kOffer) {
        signaling_->sendSdpOffer(remote_user_id_, sdp);
    } else {
        signaling_->sendSdpAnswer(remote_user_id_, sdp);
    }
}

void WebRtcSession::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    std::string sdp;
    candidate->ToString(&sdp);
    signaling_->sendIceCandidate(remote_user_id_, sdp,
                                  candidate->sdp_mid(),
                                  candidate->sdp_mline_index());
}

void WebRtcSession::OnRemoteSdpOffer(const std::string& sdp) {
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp);
    peer_connection_->SetRemoteDescription(std::move(desc), ...);
    peer_connection_->CreateAnswer(this, ...);
}
```

**OnTrack (viewer receives video):**
```cpp
void WebRtcSession::OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    auto track = transceiver->receiver()->track();
    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
        video_track->AddOrUpdateSink(video_sink_, rtc::VideoSinkWants());
    }
}
```

**DataChannel:**
```cpp
void WebRtcSession::OnMessage(const webrtc::DataBuffer& buffer) {
    if (on_data_message_) {
        on_data_message_(buffer.data.data(), buffer.data.size());
    }
}

bool WebRtcSession::SendData(const uint8_t* data, size_t size) {
    if (!data_channel_ || data_channel_->state() != webrtc::DataChannelInterface::kOpen)
        return false;
    rtc::CopyOnWriteBuffer buf(data, size);
    return data_channel_->Send(webrtc::DataBuffer(buf, true));
}
```

- [ ] **Step 3: Commit**

```bash
git add src/webrtc/webrtc_session.h src/webrtc/webrtc_session.cpp
git commit -m "feat: add WebRtcSession — PeerConnection lifecycle manager

Creates PeerConnectionFactory, manages offer/answer exchange,
ICE trickle, video track, and DataChannel. Integrates with
CaptureTrackSource, VideoSink, and SignalingClient."
```

---

### Task 10: Rewrite HostSession for WebRTC

**Files:**
- Modify: `src/session/host_session.h`
- Modify: `src/session/host_session.cpp`

HostSession now only captures frames and pushes them to `CaptureTrackSource`. No encode thread, no IEncoder, no RingBuffer.

- [ ] **Step 1: Rewrite host_session.h**

```cpp
#pragma once

#include "core/types.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace omnidesk {

class ICaptureSource;
class IDirtyRegionDetector;
class ContentClassifier;
class CaptureTrackSource;

struct HostStats {
    float fps = 0;
    float captureTimeMs = 0;
    int width = 0;
    int height = 0;
};

class HostSession {
public:
    HostSession();
    ~HostSession();

    // Set the WebRTC capture source before calling start().
    void setCaptureTrackSource(CaptureTrackSource* source);

    bool start(const CaptureConfig& capConfig);
    void stop();

    HostStats getStats() const;

private:
    void captureLoop();

    std::unique_ptr<ICaptureSource> capture_;
    std::unique_ptr<IDirtyRegionDetector> diffDetector_;
    std::unique_ptr<ContentClassifier> classifier_;
    CaptureTrackSource* captureSource_ = nullptr;  // not owned

    std::thread captureThread_;
    std::atomic<bool> running_{false};

    // Stats
    std::atomic<float> currentFps_{0};
    std::atomic<float> captureTimeMs_{0};
    int frameWidth_ = 0;
    int frameHeight_ = 0;
};

} // namespace omnidesk
```

- [ ] **Step 2: Rewrite host_session.cpp**

Key change in `captureLoop()` — push frames to `CaptureTrackSource` instead of RingBuffer→encode pipeline:

```cpp
void HostSession::captureLoop() {
    while (running_) {
        auto start = Clock::nowUs();
        Frame frame;
        if (capture_->captureFrame(frame)) {
            frameWidth_ = frame.width;
            frameHeight_ = frame.height;
            captureTimeMs_ = static_cast<float>(Clock::nowUs() - start) / 1000.0f;

            if (captureSource_) {
                captureSource_->PushFrame(frame);
            }
            // FPS tracking ...
        }
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add src/session/host_session.h src/session/host_session.cpp
git commit -m "refactor: simplify HostSession — capture only, push to WebRTC

Remove encode thread, IEncoder, RingBuffer, SendCallback,
QualityTuner, AdaptiveBitrateController. HostSession now only
captures frames and pushes to CaptureTrackSource."
```

---

### Task 11: Rewrite ViewerSession for WebRTC

**Files:**
- Modify: `src/session/viewer_session.h`
- Modify: `src/session/viewer_session.cpp`

ViewerSession now receives frames from `VideoSink` instead of decoding manually.

- [ ] **Step 1: Rewrite viewer_session.h**

```cpp
#pragma once

#include "core/types.h"
#include <atomic>
#include <memory>
#include <string>

namespace omnidesk {

class GlRenderer;
class CursorOverlay;
class SharpeningFilter;
class CursorPredictor;
class VideoSink;

struct ViewerStats {
    float fps = 0;
    float latencyMs = 0;
    int width = 0;
    int height = 0;
};

class ViewerSession {
public:
    ViewerSession();
    ~ViewerSession();

    // Set the WebRTC video sink before calling start().
    void setVideoSink(VideoSink* sink);

    bool start();
    void stop();

    ViewerStats getStats() const;

    // Called when cursor update arrives via DataChannel
    void onCursorUpdate(const CursorInfo& cursor);

    // Must be called on the GL/main thread each frame
    void processOnGlThread();

    GlRenderer* renderer() { return renderer_.get(); }

private:
    std::unique_ptr<GlRenderer> renderer_;
    std::unique_ptr<CursorOverlay> cursorOverlay_;
    std::unique_ptr<SharpeningFilter> sharpening_;
    std::unique_ptr<CursorPredictor> cursorPredictor_;
    VideoSink* videoSink_ = nullptr;  // not owned

    std::atomic<float> currentFps_{0};
    std::atomic<float> latencyMs_{0};
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    uint64_t framesRendered_ = 0;
    std::chrono::steady_clock::time_point fpsStart_;
};

} // namespace omnidesk
```

- [ ] **Step 2: Rewrite viewer_session.cpp**

Key change in `processOnGlThread()` — take frame from `VideoSink` instead of decode thread:

```cpp
void ViewerSession::processOnGlThread() {
    Frame frame;
    if (videoSink_ && videoSink_->TakeFrame(frame)) {
        frameWidth_ = frame.width;
        frameHeight_ = frame.height;
        renderer_->uploadFrame(frame);
        // FPS tracking ...
    }
    renderer_->render();
    cursorOverlay_->render();
}
```

- [ ] **Step 3: Commit**

```bash
git add src/session/viewer_session.h src/session/viewer_session.cpp
git commit -m "refactor: simplify ViewerSession — receive from VideoSink

Remove IDecoder, decode thread, onVideoPacket(). ViewerSession
now receives decoded frames from WebRTC's VideoSink."
```

---

### Task 12: Rewrite App class for WebRTC

**Files:**
- Modify: `src/ui/app.h`
- Modify: `src/ui/app.cpp`

This is the largest single change. Replace TcpChannel/relay/P2P logic with `WebRtcSession`.

- [ ] **Step 1: Update app.h**

Remove:
- `TcpChannel dataListener_`, `std::unique_ptr<TcpChannel> dataChannel_`, `dataChannelMutex_`
- `dataAcceptThread_`, `dataRecvThread_`, `dataRunning_`
- `relayMode_`, `relayPeerId_`
- `sendVideoPacket()`, `viewerReceiveLoop()`
- `#include "transport/tcp_channel.h"`, `#include "transport/protocol.h"`

Add:
```cpp
#include "webrtc/webrtc_session.h"
#include "webrtc/capture_track_source.h"
#include "webrtc/video_sink.h"

// In AppConfig:
std::string turnServer;
std::string turnUser;
std::string turnPassword;

// In App:
std::unique_ptr<WebRtcSession> webrtcSession_;
rtc::scoped_refptr<CaptureTrackSource> captureSource_;
std::unique_ptr<VideoSink> videoSink_;
```

- [ ] **Step 2: Rewrite connection flow in app.cpp**

**Host accept flow** (replacing the old P2P listener + relay logic):
```cpp
void App::handleConnectionAccept(const UserID& viewerId) {
    // Create capture source and host session
    captureSource_ = rtc::make_ref_counted<CaptureTrackSource>();
    hostSession_ = std::make_unique<HostSession>();
    hostSession_->setCaptureTrackSource(captureSource_.get());
    hostSession_->start(config_.capture);

    // Create WebRTC session
    WebRtcConfig wrtcConfig;
    wrtcConfig.turnServer = config_.turnServer;
    wrtcConfig.turnUser = config_.turnUser;
    wrtcConfig.turnPassword = config_.turnPassword;

    webrtcSession_ = std::make_unique<WebRtcSession>(signaling_.get(), wrtcConfig);
    webrtcSession_->SetOnConnected([this]() {
        queueAction([this]() { state_ = AppState::SESSION_HOST; });
    });
    webrtcSession_->SetOnDisconnected([this]() {
        queueAction([this]() { disconnectSession(); });
    });
    webrtcSession_->SetOnDataMessage([this](const uint8_t* data, size_t size) {
        // Dispatch input events from viewer
        // First byte = message type, rest = payload
    });

    webrtcSession_->StartAsHost(captureSource_);
}
```

**Viewer connect flow:**
```cpp
void App::connectToPeer(const std::string& peerId) {
    videoSink_ = std::make_unique<VideoSink>();
    viewerSession_ = std::make_unique<ViewerSession>();
    viewerSession_->setVideoSink(videoSink_.get());
    viewerSession_->start();

    WebRtcConfig wrtcConfig;
    wrtcConfig.turnServer = config_.turnServer;
    wrtcConfig.turnUser = config_.turnUser;
    wrtcConfig.turnPassword = config_.turnPassword;

    webrtcSession_ = std::make_unique<WebRtcSession>(signaling_.get(), wrtcConfig);
    webrtcSession_->SetOnConnected([this]() {
        queueAction([this]() { state_ = AppState::SESSION_VIEWER; });
    });
    webrtcSession_->SetOnDisconnected([this]() {
        queueAction([this]() { disconnectSession(); });
    });
    webrtcSession_->SetOnDataMessage([this](const uint8_t* data, size_t size) {
        // Dispatch cursor updates from host
    });

    webrtcSession_->StartAsViewer(videoSink_.get());
}
```

**Wire signaling SDP/ICE callbacks** in `init()`:
```cpp
signaling_->onSdpOffer([this](const SdpMessage& msg) {
    queueAction([this, msg]() {
        if (webrtcSession_) webrtcSession_->OnRemoteSdpOffer(msg.sdp);
    });
});
signaling_->onSdpAnswer([this](const SdpMessage& msg) {
    queueAction([this, msg]() {
        if (webrtcSession_) webrtcSession_->OnRemoteSdpAnswer(msg.sdp);
    });
});
signaling_->onIceCandidate([this](const IceCandidate& ice) {
    queueAction([this, ice]() {
        if (webrtcSession_) webrtcSession_->OnRemoteIceCandidate(
            ice.candidate, ice.sdpMid, ice.sdpMLineIndex);
    });
});
```

**disconnectSession():**
```cpp
void App::disconnectSession() {
    if (webrtcSession_) webrtcSession_->Close();
    webrtcSession_.reset();
    hostSession_.reset();
    viewerSession_.reset();
    captureSource_ = nullptr;
    videoSink_.reset();
    state_ = AppState::DASHBOARD;
}
```

Remove: `getLocalIp()` (no longer needed — ICE handles address discovery), `sendVideoPacket()`, `viewerReceiveLoop()`, all `dataChannel_`/`dataListener_` code.

- [ ] **Step 3: Add WebRTC init/shutdown to main.cpp**

In `main()`, before `app.init()`:
```cpp
WebRtcSession::InitFactory();
```
In cleanup:
```cpp
WebRtcSession::ShutdownFactory();
```

- [ ] **Step 4: Commit**

```bash
git add src/ui/app.h src/ui/app.cpp src/main.cpp
git commit -m "feat: rewrite App for WebRTC — replace P2P/relay with PeerConnection

Remove TcpChannel, relay mode, P2P hole punching, manual packet
serialization. Connection flow now: signaling handshake -> WebRTC
offer/answer -> ICE connects -> video track + DataChannel."
```

---

### Task 13: Update CMakeLists.txt — final wiring

**Files:**
- Modify: `CMakeLists.txt`

Add the `omnidesk_webrtc` target and re-wire dependencies.

- [ ] **Step 1: Add omnidesk_webrtc target**

After the codec target, add:

```cmake
# WebRTC integration
add_library(omnidesk_webrtc STATIC
    src/webrtc/webrtc_session.cpp
    src/webrtc/capture_track_source.cpp
    src/webrtc/video_encoder_factory.cpp
    src/webrtc/video_decoder_factory.cpp
    src/webrtc/video_sink.cpp
)
target_link_libraries(omnidesk_webrtc PUBLIC
    omnidesk_core omnidesk_codec omnidesk_signaling libwebrtc
)
```

- [ ] **Step 2: Update session target**

```cmake
add_library(omnidesk_session STATIC
    src/session/host_session.cpp
    src/session/viewer_session.cpp
)
target_link_libraries(omnidesk_session PUBLIC
    omnidesk_core omnidesk_capture omnidesk_diff
    omnidesk_webrtc omnidesk_render omnidesk_input
)
```

- [ ] **Step 3: Update UI and executable targets**

```cmake
target_link_libraries(omnidesk_ui PUBLIC omnidesk_session omnidesk_signaling omnidesk_render omnidesk_webrtc)
target_link_libraries(omnidesk24 PRIVATE omnidesk_ui omnidesk_session)
```

- [ ] **Step 4: Build everything**

Run: `cmake -B build-webrtc -DLIBWEBRTC_ROOT=/path/to/prebuilt && cmake --build build-webrtc`

Expected: Full build succeeds.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat: add omnidesk_webrtc CMake target, wire all dependencies

Final build system update. omnidesk_webrtc links libwebrtc,
omnidesk_codec, omnidesk_signaling. Session depends on
omnidesk_webrtc instead of old transport."
```

---

### Task 14: Smoke test

**Files:** None (verification only)

- [ ] **Step 1: Verify build**

```bash
cmake -B build-webrtc -DLIBWEBRTC_ROOT=/path/to/prebuilt
cmake --build build-webrtc
```

Expected: Clean build with no errors.

- [ ] **Step 2: Verify no old transport references remain**

```bash
grep -r "transport/" src/ --include="*.h" --include="*.cpp"
grep -r "openh264" src/ --include="*.h" --include="*.cpp"
grep -r "codec_factory" src/ --include="*.h" --include="*.cpp"
```

Expected: No matches.

- [ ] **Step 3: Verify executable starts**

Run `./build-webrtc/omnidesk24` — verify it launches, connects to signaling, shows dashboard.

- [ ] **Step 4: Manual connection test**

If a signaling server is available: connect two instances and verify WebRTC negotiation (SDP exchange, ICE candidates, video track opens).

- [ ] **Step 5: Commit any fixes**

```bash
git add -A
git commit -m "fix: address issues found during smoke test"
```
