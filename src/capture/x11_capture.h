#pragma once

#include "capture/capture.h"

#ifdef OMNIDESK_HAS_X11

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace omnidesk {

class X11Capture : public ICaptureSource {
public:
    X11Capture();
    ~X11Capture() override;

    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    bool init(const CaptureConfig& config) override;
    CaptureResult captureFrame(Frame& frame) override;
    std::vector<MonitorInfo> enumMonitors() override;
    void release() override;

private:
    bool initShm();
    void destroyShm();
    bool initDamage();
    void destroyDamage();
    std::vector<Rect> fetchDamageRects();
    CursorInfo captureCursor();
    MonitorInfo findMonitor(int32_t monitorId) const;

    Display*            display_     = nullptr;
    Window              rootWindow_  = None;
    int                 screen_      = 0;

    // XShm capture
    XImage*             shmImage_    = nullptr;
    XShmSegmentInfo     shmInfo_     = {};
    bool                shmAttached_ = false;

    // XDamage tracking
    Damage              damage_      = None;
    int                 damageEventBase_ = 0;
    int                 damageErrorBase_ = 0;
    bool                damageActive_    = false;

    // XFixes cursor
    int                 fixesEventBase_ = 0;
    int                 fixesErrorBase_ = 0;

    // XRandR multi-monitor
    int                 randrEventBase_ = 0;
    int                 randrErrorBase_ = 0;

    // State
    CaptureConfig       config_      = {};
    MonitorInfo         targetMonitor_ = {};
    uint64_t            frameCounter_ = 0;
    bool                initialized_ = false;
    bool                fullFrameNeeded_ = true;
};

} // namespace omnidesk

#endif // OMNIDESK_HAS_X11
