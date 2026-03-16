#pragma once

#include "capture/capture.h"

#ifdef OMNIDESK_HAS_PIPEWIRE

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omnidesk {

/// Screen capture via the PipeWire ScreenCast portal (xdg-desktop-portal).
/// Works under both Wayland and X11 (when the portal is available).
/// Prefers DMA-BUF buffer sharing, falls back to SHM memfd buffers.
class PipeWireCapture : public ICaptureSource {
public:
    PipeWireCapture();
    ~PipeWireCapture() override;

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    bool init(const CaptureConfig& config) override;
    CaptureResult captureFrame(Frame& frame) override;
    std::vector<MonitorInfo> enumMonitors() override;
    void release() override;

private:
    // D-Bus portal session management
    bool openPortalSession();
    bool selectSource();
    bool startStream();

    // PipeWire stream handling
    bool connectPipeWire();
    void disconnectPipeWire();

    CaptureConfig config_       = {};
    uint64_t      frameCounter_ = 0;
    bool          initialized_  = false;

    // Portal state
    std::string   sessionHandle_;
    uint32_t      pipeWireNodeId_ = 0;
    int           pipeWireFd_     = -1;

    // Buffer preference
    bool          useDmaBuf_    = false;

    // Opaque pointers — forward-declared to avoid pulling PipeWire headers
    // into every translation unit that includes this header.
    struct PwState;
    std::unique_ptr<PwState> pw_;
};

} // namespace omnidesk

#endif // OMNIDESK_HAS_PIPEWIRE
