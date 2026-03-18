#pragma once

#include "core/types.h"
#include <cstdint>

namespace omnidesk {

// Injects InputEvent into the host's operating system via platform APIs.
// On Windows this uses SendInput; on Linux it would use uinput/XTest.
class InputInjector {
public:
    InputInjector() = default;
    ~InputInjector() = default;

    // Set the remote desktop dimensions for absolute coordinate mapping.
    void setScreenSize(int width, int height);

    // Inject a single input event into the system.
    void inject(const InputEvent& ev);

private:
    void injectMouse(const InputEvent& ev);
    void injectKey(const InputEvent& ev);

    int screenWidth_ = 1920;
    int screenHeight_ = 1080;
};

} // namespace omnidesk
