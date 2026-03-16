#ifdef OMNIDESK_HAS_X11

#include "capture/x11_capture.h"

#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace omnidesk {

X11Capture::X11Capture() = default;

X11Capture::~X11Capture() {
    release();
}

bool X11Capture::init(const CaptureConfig& config) {
    release();
    config_ = config;

    // Open display
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        return false;
    }

    screen_ = DefaultScreen(display_);
    rootWindow_ = RootWindow(display_, screen_);

    // Query XRandR for multi-monitor support
    if (!XRRQueryExtension(display_, &randrEventBase_, &randrErrorBase_)) {
        // XRandR not available; fall back to single-screen geometry
        randrEventBase_ = 0;
    }

    // Determine which monitor to capture
    auto monitors = enumMonitors();
    if (monitors.empty()) {
        release();
        return false;
    }

    targetMonitor_ = findMonitor(config.monitorId);

    // Initialise XShm for fast full-frame grabs
    if (!initShm()) {
        release();
        return false;
    }

    // Initialise XDamage for dirty-rect tracking
    if (!initDamage()) {
        // Non-fatal: we can still capture full frames each time
    }

    // Query XFixes for cursor capture
    if (config.captureCursor) {
        XFixesQueryExtension(display_, &fixesEventBase_, &fixesErrorBase_);
        // Select cursor-change notifications on root window
        XFixesSelectCursorInput(display_, rootWindow_,
                                XFixesDisplayCursorNotifyMask);
    }

    initialized_ = true;
    fullFrameNeeded_ = true;
    frameCounter_ = 0;
    return true;
}

CaptureResult X11Capture::captureFrame(Frame& frame) {
    CaptureResult result;
    if (!initialized_ || !display_) {
        result.status = CaptureResult::ERROR;
        return result;
    }

    auto captureStart = std::chrono::steady_clock::now();

    // Drain pending X events (damage, cursor, RandR)
    while (XPending(display_)) {
        XEvent ev;
        XNextEvent(display_, &ev);

        // Check for RandR screen-change events
        if (randrEventBase_ &&
            ev.type == randrEventBase_ + RRScreenChangeNotify) {
            result.status = CaptureResult::DISPLAY_CHANGED;
            return result;
        }
    }

    // Collect dirty regions from XDamage
    std::vector<Rect> dirty;
    if (damageActive_ && !fullFrameNeeded_) {
        dirty = fetchDamageRects();
        if (dirty.empty()) {
            // Nothing changed — return TIMEOUT to signal "no new content"
            result.status = CaptureResult::TIMEOUT;
            return result;
        }
    }

    // Perform XShmGetImage for the target monitor region
    const auto& mon = targetMonitor_;
    if (!XShmGetImage(display_, rootWindow_, shmImage_,
                      mon.bounds.x, mon.bounds.y, AllPlanes)) {
        result.status = CaptureResult::ERROR;
        return result;
    }

    // Copy pixel data into Frame
    frame.allocate(mon.bounds.width, mon.bounds.height, PixelFormat::BGRA);
    frame.timestampUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            captureStart.time_since_epoch())
            .count());
    frame.frameId = frameCounter_++;

    const int srcStride = shmImage_->bytes_per_line;
    const int dstStride = frame.stride;
    const int rowBytes = mon.bounds.width * 4;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(shmImage_->data);
    uint8_t* dst = frame.data.data();

    for (int32_t row = 0; row < mon.bounds.height; ++row) {
        std::memcpy(dst + row * dstStride, src + row * srcStride, rowBytes);
    }

    // Build dirty-rect list
    if (fullFrameNeeded_ || dirty.empty()) {
        result.dirtyRects.push_back(
            Rect{0, 0, mon.bounds.width, mon.bounds.height});
        fullFrameNeeded_ = false;
    } else {
        // Clamp dirty rects to monitor-relative coordinates
        for (auto& r : dirty) {
            r.x -= mon.bounds.x;
            r.y -= mon.bounds.y;
            r.x = std::max(r.x, 0);
            r.y = std::max(r.y, 0);
            r.width = std::min(r.width, mon.bounds.width - r.x);
            r.height = std::min(r.height, mon.bounds.height - r.y);
            if (!r.empty()) {
                result.dirtyRects.push_back(r);
            }
        }
        if (result.dirtyRects.empty()) {
            result.dirtyRects.push_back(
                Rect{0, 0, mon.bounds.width, mon.bounds.height});
        }
    }

    // Capture cursor
    if (config_.captureCursor) {
        result.cursor = captureCursor();
        // Make cursor position monitor-relative
        result.cursor.x -= mon.bounds.x;
        result.cursor.y -= mon.bounds.y;
    }

    auto captureEnd = std::chrono::steady_clock::now();
    result.captureTimeUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            captureEnd - captureStart)
            .count());
    result.status = CaptureResult::OK;
    return result;
}

std::vector<MonitorInfo> X11Capture::enumMonitors() {
    std::vector<MonitorInfo> monitors;
    if (!display_) {
        return monitors;
    }

    // Attempt XRandR enumeration
    XRRScreenResources* res = XRRGetScreenResources(display_, rootWindow_);
    if (res) {
        RROutput primaryOutput = XRRGetOutputPrimary(display_, rootWindow_);

        for (int i = 0; i < res->noutput; ++i) {
            XRROutputInfo* outInfo =
                XRRGetOutputInfo(display_, res, res->outputs[i]);
            if (!outInfo || outInfo->connection != RR_Connected ||
                outInfo->crtc == None) {
                if (outInfo) XRRFreeOutputInfo(outInfo);
                continue;
            }

            XRRCrtcInfo* crtcInfo =
                XRRGetCrtcInfo(display_, res, outInfo->crtc);
            if (crtcInfo) {
                MonitorInfo mi;
                mi.id = static_cast<int32_t>(i);
                mi.name = outInfo->name ? outInfo->name : "";
                mi.bounds.x = static_cast<int32_t>(crtcInfo->x);
                mi.bounds.y = static_cast<int32_t>(crtcInfo->y);
                mi.bounds.width = static_cast<int32_t>(crtcInfo->width);
                mi.bounds.height = static_cast<int32_t>(crtcInfo->height);
                mi.primary = (res->outputs[i] == primaryOutput);
                monitors.push_back(mi);
                XRRFreeCrtcInfo(crtcInfo);
            }

            XRRFreeOutputInfo(outInfo);
        }

        XRRFreeScreenResources(res);
    }

    // Fallback: if XRandR produced nothing, use the root window dimensions
    if (monitors.empty()) {
        MonitorInfo mi;
        mi.id = 0;
        mi.name = "default";
        mi.bounds.x = 0;
        mi.bounds.y = 0;
        mi.bounds.width = DisplayWidth(display_, screen_);
        mi.bounds.height = DisplayHeight(display_, screen_);
        mi.primary = true;
        monitors.push_back(mi);
    }

    return monitors;
}

void X11Capture::release() {
    destroyDamage();
    destroyShm();

    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }

    rootWindow_ = None;
    initialized_ = false;
    fullFrameNeeded_ = true;
    frameCounter_ = 0;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool X11Capture::initShm() {
    // Query MIT-SHM extension
    int major, minor;
    Bool hasPixmaps;
    if (!XShmQueryVersion(display_, &major, &minor, &hasPixmaps)) {
        return false;
    }

    const int w = targetMonitor_.bounds.width;
    const int h = targetMonitor_.bounds.height;
    const int depth = DefaultDepth(display_, screen_);
    Visual* visual = DefaultVisual(display_, screen_);

    shmImage_ = XShmCreateImage(display_, visual, depth, ZPixmap,
                                nullptr, &shmInfo_, w, h);
    if (!shmImage_) {
        return false;
    }

    shmInfo_.shmid = shmget(IPC_PRIVATE,
                            static_cast<size_t>(shmImage_->bytes_per_line) * h,
                            IPC_CREAT | 0600);
    if (shmInfo_.shmid < 0) {
        XDestroyImage(shmImage_);
        shmImage_ = nullptr;
        return false;
    }

    shmInfo_.shmaddr = static_cast<char*>(shmat(shmInfo_.shmid, nullptr, 0));
    if (shmInfo_.shmaddr == reinterpret_cast<char*>(-1)) {
        shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
        XDestroyImage(shmImage_);
        shmImage_ = nullptr;
        return false;
    }

    shmImage_->data = shmInfo_.shmaddr;
    shmInfo_.readOnly = False;

    if (!XShmAttach(display_, &shmInfo_)) {
        shmdt(shmInfo_.shmaddr);
        shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
        XDestroyImage(shmImage_);
        shmImage_ = nullptr;
        return false;
    }

    // Mark for removal once all processes detach
    shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
    shmAttached_ = true;
    return true;
}

void X11Capture::destroyShm() {
    if (shmAttached_ && display_) {
        XShmDetach(display_, &shmInfo_);
        shmAttached_ = false;
    }
    if (shmInfo_.shmaddr && shmInfo_.shmaddr != reinterpret_cast<char*>(-1)) {
        shmdt(shmInfo_.shmaddr);
        shmInfo_.shmaddr = nullptr;
    }
    if (shmImage_) {
        // XDestroyImage frees shmImage_->data pointer, but we detached SHM
        // above, so set data to nullptr to avoid double-free.
        shmImage_->data = nullptr;
        XDestroyImage(shmImage_);
        shmImage_ = nullptr;
    }
}

bool X11Capture::initDamage() {
    if (!XDamageQueryExtension(display_, &damageEventBase_,
                               &damageErrorBase_)) {
        return false;
    }

    damage_ = XDamageCreate(display_, rootWindow_, XDamageReportRawRectangles);
    if (damage_ == None) {
        return false;
    }

    damageActive_ = true;
    return true;
}

void X11Capture::destroyDamage() {
    if (damageActive_ && display_ && damage_ != None) {
        XDamageDestroy(display_, damage_);
        damage_ = None;
        damageActive_ = false;
    }
}

std::vector<Rect> X11Capture::fetchDamageRects() {
    std::vector<Rect> rects;
    if (!damageActive_) return rects;

    // Subtract the damage region to retrieve and reset it atomically
    XserverRegion region = XFixesCreateRegion(display_, nullptr, 0);
    XDamageSubtract(display_, damage_, None, region);

    int nRects = 0;
    XRectangle* xRects = XFixesFetchRegion(display_, region, &nRects);
    if (xRects) {
        rects.reserve(static_cast<size_t>(nRects));
        for (int i = 0; i < nRects; ++i) {
            Rect r;
            r.x = xRects[i].x;
            r.y = xRects[i].y;
            r.width = xRects[i].width;
            r.height = xRects[i].height;
            rects.push_back(r);
        }
        XFree(xRects);
    }

    XFixesDestroyRegion(display_, region);
    return rects;
}

CursorInfo X11Capture::captureCursor() {
    CursorInfo info;
    XFixesCursorImage* cursor = XFixesGetCursorImage(display_);
    if (!cursor) {
        info.visible = false;
        return info;
    }

    info.x = cursor->x;
    info.y = cursor->y;
    info.hotspotX = cursor->xhot;
    info.hotspotY = cursor->yhot;
    info.width = static_cast<int32_t>(cursor->width);
    info.height = static_cast<int32_t>(cursor->height);
    info.visible = true;
    info.shapeChanged = true; // Simplified: always report changed

    // Convert XFixes cursor pixels (unsigned long ARGB) to BGRA
    const int pixelCount = info.width * info.height;
    info.imageData.resize(static_cast<size_t>(pixelCount) * 4);

    for (int i = 0; i < pixelCount; ++i) {
        // XFixes stores pixels as unsigned long (may be 64-bit), ARGB order
        const uint32_t argb = static_cast<uint32_t>(cursor->pixels[i]);
        const uint8_t a = (argb >> 24) & 0xFF;
        const uint8_t r = (argb >> 16) & 0xFF;
        const uint8_t g = (argb >> 8) & 0xFF;
        const uint8_t b = argb & 0xFF;
        info.imageData[i * 4 + 0] = b;
        info.imageData[i * 4 + 1] = g;
        info.imageData[i * 4 + 2] = r;
        info.imageData[i * 4 + 3] = a;
    }

    // Simple hash for cursor shape caching
    uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
    for (size_t i = 0; i < info.imageData.size(); ++i) {
        hash ^= info.imageData[i];
        hash *= 1099511628211ULL; // FNV prime
    }
    info.shapeHash = hash;

    XFree(cursor);
    return info;
}

MonitorInfo X11Capture::findMonitor(int32_t monitorId) const {
    // enumMonitors() is not const in the interface, so we do a fresh query here
    X11Capture* self = const_cast<X11Capture*>(this);
    auto monitors = self->enumMonitors();

    if (monitorId >= 0) {
        for (const auto& m : monitors) {
            if (m.id == monitorId) return m;
        }
    }

    // Default: return the primary monitor, or the first one
    for (const auto& m : monitors) {
        if (m.primary) return m;
    }
    return monitors.front();
}

} // namespace omnidesk

#endif // OMNIDESK_HAS_X11
