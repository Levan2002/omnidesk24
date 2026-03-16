#ifdef OMNIDESK_HAS_PIPEWIRE

#include "capture/pipewire_capture.h"
#include "core/logger.h"

#include <chrono>
#include <cstring>
#include <unistd.h>

// TODO: Include PipeWire and D-Bus headers when building with portal support:
//   #include <pipewire/pipewire.h>
//   #include <spa/param/video/format-utils.h>
//   #include <spa/buffer/alloc.h>
//   #include <gio/gio.h>        // GDBus for portal calls

namespace omnidesk {

// ---------------------------------------------------------------------------
// Opaque PipeWire state — will hold pw_main_loop, pw_stream, etc.
// ---------------------------------------------------------------------------
struct PipeWireCapture::PwState {
    // TODO: Populate with actual PipeWire objects:
    //   pw_main_loop*   loop       = nullptr;
    //   pw_stream*      stream     = nullptr;
    //   spa_hook        streamListener = {};
    //   spa_video_info  format     = {};
    //   int32_t         width      = 0;
    //   int32_t         height     = 0;
    //   int32_t         stride     = 0;
    //   bool            haveDmaBuf = false;
    //   const uint8_t*  lastFrameData = nullptr;
    //   bool            newFrameReady = false;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PipeWireCapture::PipeWireCapture() = default;

PipeWireCapture::~PipeWireCapture() {
    release();
}

// ---------------------------------------------------------------------------
// ICaptureSource interface
// ---------------------------------------------------------------------------

bool PipeWireCapture::init(const CaptureConfig& config) {
    release();
    config_ = config;

    // TODO: Full init sequence:
    //
    // 1. Call pw_init(nullptr, nullptr) to initialise PipeWire client library.
    //
    // 2. Open a portal session via D-Bus:
    //      org.freedesktop.portal.ScreenCast.CreateSession()
    //    Store the returned session handle in sessionHandle_.
    //
    // 3. Select the capture source (monitor / window):
    //      org.freedesktop.portal.ScreenCast.SelectSources()
    //    Pass cursor_mode = EMBEDDED (2) if config.captureCursor is true.
    //    Pass types = MONITOR (1).
    //    Set persist_mode = 2 for session persistence across restarts.
    //
    // 4. Start the stream:
    //      org.freedesktop.portal.ScreenCast.Start()
    //    The portal responds with a PipeWire node_id.
    //    Store it in pipeWireNodeId_.
    //
    // 5. Open the PipeWire remote fd:
    //      org.freedesktop.portal.ScreenCast.OpenPipeWireRemote()
    //    Store the fd in pipeWireFd_.
    //
    // 6. Connect to PipeWire using pw_main_loop_new(), pw_context_new(),
    //    pw_context_connect_fd(pipeWireFd_), pw_stream_new(), etc.
    //
    // 7. Negotiate buffer format:
    //    - Attempt DMA-BUF (SPA_DATA_DmaBuf) first for zero-copy GPU access.
    //    - Fall back to SHM (SPA_DATA_MemFd / SPA_DATA_MemPtr) if DMA-BUF
    //      is not supported by the compositor.
    //    Store the preference in useDmaBuf_.

    if (!openPortalSession()) {
        LOG_ERROR("PipeWireCapture: failed to open portal session");
        release();
        return false;
    }

    if (!selectSource()) {
        LOG_ERROR("PipeWireCapture: failed to select source");
        release();
        return false;
    }

    if (!startStream()) {
        LOG_ERROR("PipeWireCapture: failed to start stream");
        release();
        return false;
    }

    if (!connectPipeWire()) {
        LOG_ERROR("PipeWireCapture: failed to connect to PipeWire");
        release();
        return false;
    }

    initialized_ = true;
    frameCounter_ = 0;
    LOG_INFO("PipeWireCapture: initialised (dmabuf=%s)",
             useDmaBuf_ ? "yes" : "no");
    return true;
}

CaptureResult PipeWireCapture::captureFrame(Frame& frame) {
    CaptureResult result;

    if (!initialized_) {
        result.status = CaptureResult::CAPTURE_ERR;
        return result;
    }

    auto captureStart = std::chrono::steady_clock::now();

    // TODO: Capture flow:
    //
    // 1. Run one iteration of the PipeWire main loop to process events:
    //      pw_main_loop_run(pw_->loop);  (or use pw_loop_iterate for non-blocking)
    //
    // 2. In the stream "process" callback (registered during connectPipeWire):
    //    a. Dequeue buffer:  pw_stream_dequeue_buffer(pw_->stream)
    //    b. Access spa_buffer:
    //       - If spa_data.type == SPA_DATA_DmaBuf:
    //           Map the DMA-BUF fd for CPU read (or pass fd to GPU encoder).
    //       - If spa_data.type == SPA_DATA_MemFd / SPA_DATA_MemPtr:
    //           Read pixels directly from spa_data.data.
    //    c. Copy pixel data into Frame:
    //       frame.allocate(pw_->width, pw_->height, PixelFormat::BGRA);
    //       memcpy(frame.data.data(), pixelPtr, frame.data.size());
    //    d. Re-queue buffer: pw_stream_queue_buffer(pw_->stream, buf)
    //
    // 3. If no new frame was available, return CaptureResult::TIMEOUT.
    //
    // 4. The portal embeds cursor into the frame when cursor_mode = EMBEDDED,
    //    so cursor info may not be separately available.
    //    If cursor_mode = METADATA (1), extract cursor data from
    //    SPA_META_Cursor in the buffer metadata.

    // Stub: no frame available yet
    result.status = CaptureResult::TIMEOUT;

    auto captureEnd = std::chrono::steady_clock::now();
    result.captureTimeUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            captureEnd - captureStart)
            .count());

    return result;
}

std::vector<MonitorInfo> PipeWireCapture::enumMonitors() {
    std::vector<MonitorInfo> monitors;

    // TODO: Monitor enumeration on Wayland via D-Bus:
    //
    // Option 1: Use org.freedesktop.portal.ScreenCast — the SelectSources
    //           dialog lets the user pick a monitor. We don't get a list
    //           programmatically from the portal.
    //
    // Option 2: Use org.gnome.Mutter.DisplayConfig (GNOME-specific) or
    //           org.kde.KWin.ScreenPool (KDE-specific) to enumerate outputs.
    //
    // Option 3: If running under XWayland, fall back to XRandR enumeration
    //           via X11Capture::enumMonitors().
    //
    // For now, return a single "default" monitor as a placeholder.

    MonitorInfo mi;
    mi.id = 0;
    mi.name = "pipewire-default";
    mi.bounds = {0, 0, 1920, 1080};
    mi.primary = true;
    monitors.push_back(mi);

    return monitors;
}

void PipeWireCapture::release() {
    disconnectPipeWire();

    // TODO: Close the portal session via D-Bus:
    //   If sessionHandle_ is non-empty, call
    //     org.freedesktop.portal.Session.Close()
    //   on the session object path.

    sessionHandle_.clear();
    pipeWireNodeId_ = 0;

    if (pipeWireFd_ >= 0) {
        ::close(pipeWireFd_);
        pipeWireFd_ = -1;
    }

    useDmaBuf_ = false;
    initialized_ = false;
    frameCounter_ = 0;
}

// ---------------------------------------------------------------------------
// Private: D-Bus portal session management
// ---------------------------------------------------------------------------

bool PipeWireCapture::openPortalSession() {
    // TODO: Implement D-Bus call to create a ScreenCast session:
    //
    //   Bus:        org.freedesktop.portal.Desktop
    //   Object:     /org/freedesktop/portal/desktop
    //   Interface:  org.freedesktop.portal.ScreenCast
    //   Method:     CreateSession(options: a{sv})
    //
    //   options should include:
    //     "handle_token" -> unique token string
    //     "session_handle_token" -> unique session token
    //
    //   The response signal delivers the session_handle.
    //   Store it in sessionHandle_.
    //
    //   Use GDBusConnection (libgio) or sd-bus for the D-Bus calls.

    LOG_WARN("PipeWireCapture::openPortalSession() not yet implemented");
    return false;
}

bool PipeWireCapture::selectSource() {
    // TODO: Implement D-Bus call to select capture source:
    //
    //   Method: org.freedesktop.portal.ScreenCast.SelectSources()
    //   Args:   session_handle, options: a{sv}
    //
    //   options:
    //     "types"       -> uint32(1)   // MONITOR=1, WINDOW=2, VIRTUAL=4
    //     "cursor_mode" -> uint32(2)   // HIDDEN=0, EMBEDDED=2, METADATA=1
    //     "persist_mode"-> uint32(2)   // 0=none, 1=app, 2=permanent
    //     "multiple"    -> bool(false) // single source
    //
    //   If persist_mode >= 1, a "restore_token" can be provided on future
    //   calls to skip the interactive picker dialog.
    //
    //   Wait for the Response signal before proceeding.

    LOG_WARN("PipeWireCapture::selectSource() not yet implemented");
    return false;
}

bool PipeWireCapture::startStream() {
    // TODO: Implement D-Bus call to start streaming:
    //
    //   Method: org.freedesktop.portal.ScreenCast.Start()
    //   Args:   session_handle, parent_window (""), options: a{sv}
    //
    //   The Response signal contains:
    //     "streams" -> array of (node_id: uint32, properties: a{sv})
    //   Each stream entry gives us the PipeWire node_id to connect to.
    //   Store pipeWireNodeId_ = node_id.
    //
    //   Then call OpenPipeWireRemote() to get the fd:
    //     Method: org.freedesktop.portal.ScreenCast.OpenPipeWireRemote()
    //     Args:   session_handle, options: a{sv}
    //     Returns: fd (file descriptor)
    //   Store pipeWireFd_ = fd.

    LOG_WARN("PipeWireCapture::startStream() not yet implemented");
    return false;
}

// ---------------------------------------------------------------------------
// Private: PipeWire stream connection
// ---------------------------------------------------------------------------

bool PipeWireCapture::connectPipeWire() {
    // TODO: Connect to PipeWire using the fd from the portal:
    //
    //   1. pw_ = std::make_unique<PwState>();
    //   2. pw_->loop = pw_main_loop_new(nullptr);
    //   3. auto* context = pw_context_new(pw_main_loop_get_loop(pw_->loop), ...);
    //   4. auto* core = pw_context_connect_fd(context, pipeWireFd_, ...);
    //      (This takes ownership of pipeWireFd_; set it to -1.)
    //
    //   5. Create a pw_stream targeting pipeWireNodeId_:
    //      pw_->stream = pw_stream_new(core, "omnidesk-capture", ...);
    //
    //   6. Register stream events:
    //      - state_changed: track connection state
    //      - param_changed: negotiate video format (BGRA preferred)
    //      - process: dequeue frame buffers
    //
    //   7. In param_changed callback, build format params:
    //      - Try SPA_DATA_DmaBuf first: set useDmaBuf_ = true
    //      - Fall back to SPA_DATA_MemFd / SPA_DATA_MemPtr
    //      - Call pw_stream_update_params()
    //
    //   8. Connect the stream:
    //      pw_stream_connect(pw_->stream, PW_DIRECTION_INPUT,
    //                        pipeWireNodeId_, PW_STREAM_FLAG_AUTOCONNECT, ...);

    LOG_WARN("PipeWireCapture::connectPipeWire() not yet implemented");
    return false;
}

void PipeWireCapture::disconnectPipeWire() {
    // TODO: Tear down PipeWire objects:
    //
    //   if (pw_) {
    //       if (pw_->stream) {
    //           pw_stream_disconnect(pw_->stream);
    //           pw_stream_destroy(pw_->stream);
    //       }
    //       if (pw_->loop) {
    //           pw_main_loop_quit(pw_->loop);
    //           pw_main_loop_destroy(pw_->loop);
    //       }
    //       pw_.reset();
    //   }
    //   pw_deinit();

    pw_.reset();
}

} // namespace omnidesk

#endif // OMNIDESK_HAS_PIPEWIRE
