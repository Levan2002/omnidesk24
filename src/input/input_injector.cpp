#include "input/input_injector.h"
#include "core/logger.h"

#ifdef OMNIDESK_PLATFORM_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace omnidesk {

void InputInjector::setScreenSize(int width, int height) {
    screenWidth_ = width;
    screenHeight_ = height;
}

void InputInjector::inject(const InputEvent& ev) {
    switch (ev.type) {
        case InputType::MOUSE_MOVE:
        case InputType::MOUSE_DOWN:
        case InputType::MOUSE_UP:
        case InputType::MOUSE_SCROLL:
            injectMouse(ev);
            break;
        case InputType::KEY_DOWN:
        case InputType::KEY_UP:
            injectKey(ev);
            break;
    }
}

#ifdef OMNIDESK_PLATFORM_WINDOWS

void InputInjector::injectMouse(const InputEvent& ev) {
    INPUT input = {};
    input.type = INPUT_MOUSE;

    // Convert desktop-relative coordinates to the 0..65535 absolute range
    // that SendInput expects for MOUSEEVENTF_ABSOLUTE.
    if (screenWidth_ > 0 && screenHeight_ > 0) {
        input.mi.dx = static_cast<LONG>(ev.x * 65535 / screenWidth_);
        input.mi.dy = static_cast<LONG>(ev.y * 65535 / screenHeight_);
    }

    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

    switch (ev.type) {
        case InputType::MOUSE_MOVE:
            // Move only — flags already set above
            break;
        case InputType::MOUSE_DOWN:
            if (ev.button == 0)      input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            else if (ev.button == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
            else if (ev.button == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
            break;
        case InputType::MOUSE_UP:
            if (ev.button == 0)      input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            else if (ev.button == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
            else if (ev.button == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
            break;
        case InputType::MOUSE_SCROLL:
            input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(ev.y);  // scroll delta in ev.y
            break;
        default:
            break;
    }

    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::injectKey(const InputEvent& ev) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(ev.scancode);
    input.ki.dwFlags = KEYEVENTF_SCANCODE;

    if (ev.type == InputType::KEY_UP) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    // Extended keys (right ctrl, right alt, arrows, etc.) have scancode >= 0x100
    if (ev.scancode & 0xFF00) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        input.ki.wScan = static_cast<WORD>(ev.scancode & 0xFF);
    }

    SendInput(1, &input, sizeof(INPUT));
}

#else
// Stub for non-Windows platforms (Linux uinput would go here)

void InputInjector::injectMouse(const InputEvent& /*ev*/) {
    // TODO: implement via uinput or XTest on Linux
}

void InputInjector::injectKey(const InputEvent& /*ev*/) {
    // TODO: implement via uinput or XTest on Linux
}

#endif

} // namespace omnidesk
