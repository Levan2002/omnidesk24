#pragma once

#include "core/types.h"
#include <memory>
#include <vector>

namespace omnidesk {

struct CaptureResult {
    enum Status { OK, TIMEOUT, DISPLAY_CHANGED, CAPTURE_ERR };
    Status status = OK;
    std::vector<Rect> dirtyRects;
    uint64_t captureTimeUs = 0;
    CursorInfo cursor;
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    virtual bool init(const CaptureConfig& config) = 0;
    virtual CaptureResult captureFrame(Frame& frame) = 0;
    virtual std::vector<MonitorInfo> enumMonitors() = 0;
    virtual void release() = 0;
};

// Factory: creates the best available capture source for the current platform
std::unique_ptr<ICaptureSource> createCaptureSource();

} // namespace omnidesk
