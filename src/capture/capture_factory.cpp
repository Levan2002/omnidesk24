#include "capture/capture.h"
#include "core/logger.h"

#ifdef OMNIDESK_HAS_PIPEWIRE
#include "capture/pipewire_capture.h"
#endif
#ifdef OMNIDESK_HAS_X11
#include "capture/x11_capture.h"
#endif
#ifdef OMNIDESK_PLATFORM_WINDOWS
#include "capture/dxgi_capture.h"
#endif

namespace omnidesk {

std::unique_ptr<ICaptureSource> createCaptureSource() {
#ifdef OMNIDESK_PLATFORM_WINDOWS
    LOG_INFO("Creating DXGI Desktop Duplication capture source");
    return std::make_unique<DxgiCapture>();
#elif defined(OMNIDESK_PLATFORM_LINUX)
    #ifdef OMNIDESK_HAS_PIPEWIRE
    LOG_INFO("Creating PipeWire capture source (Wayland)");
    auto pw = std::make_unique<PipeWireCapture>();
    // PipeWire might fail if not on Wayland, fall through to X11
    return pw;
    #endif
    #ifdef OMNIDESK_HAS_X11
    LOG_INFO("Creating X11 capture source");
    return std::make_unique<X11Capture>();
    #endif
    LOG_ERROR("No capture source available on this platform");
    return nullptr;
#else
    LOG_ERROR("No capture source available on this platform");
    return nullptr;
#endif
}

} // namespace omnidesk
