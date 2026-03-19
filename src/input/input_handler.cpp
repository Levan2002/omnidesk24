#include "input/input_handler.h"

namespace omnidesk {

void InputHandler::init(HWND hwnd, int remoteWidth, int remoteHeight) {
    hwnd_ = hwnd;
    remoteWidth_ = remoteWidth;
    remoteHeight_ = remoteHeight;
}

void InputHandler::setRemoteSize(int width, int height) {
    remoteWidth_ = width;
    remoteHeight_ = height;
}

void InputHandler::poll() {
    // Clipboard sync check (every N frames)
    // TODO: implement clipboard polling
}

void InputHandler::onMouseMove(int windowX, int windowY) {
    if (!enabled_ || !onMouse_) return;

    // Get client area dimensions for coordinate mapping
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    if (winW <= 0 || winH <= 0) return;

    lastMouseX_ = windowX;
    lastMouseY_ = windowY;

    MouseEvent evt;
    evt.x = static_cast<int32_t>(windowX * remoteWidth_ / winW);
    evt.y = static_cast<int32_t>(windowY * remoteHeight_ / winH);
    evt.isAbsolute = true;

    onMouse_(evt);
}

void InputHandler::onMouseButton(int button, bool pressed) {
    if (!enabled_ || !onMouse_) return;

    // Map last known cursor position to remote coords
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    if (winW <= 0 || winH <= 0) return;

    MouseEvent evt;
    evt.x = static_cast<int32_t>(lastMouseX_ * remoteWidth_ / winW);
    evt.y = static_cast<int32_t>(lastMouseY_ * remoteHeight_ / winH);
    evt.isAbsolute = true;
    evt.pressed = pressed;

    // Set button bitmask so the receiver knows which button changed
    if (button == 0) evt.buttons |= 1;       // left
    else if (button == 1) evt.buttons |= 2;  // right
    else if (button == 2) evt.buttons |= 4;  // middle

    onMouse_(evt);
}

void InputHandler::onScroll(int delta) {
    if (!enabled_ || !onMouse_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    if (winW <= 0 || winH <= 0) return;

    MouseEvent evt;
    evt.x = static_cast<int32_t>(lastMouseX_ * remoteWidth_ / winW);
    evt.y = static_cast<int32_t>(lastMouseY_ * remoteHeight_ / winH);
    evt.scrollY = static_cast<int16_t>(delta);

    onMouse_(evt);
}

void InputHandler::onKey(UINT scancode, bool pressed) {
    if (!enabled_ || !onKey_) return;

    KeyEvent evt;
    evt.scancode = static_cast<uint32_t>(scancode);
    evt.pressed = pressed;

    onKey_(evt);
}

} // namespace omnidesk
