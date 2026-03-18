#include "input/input_handler.h"
#include <GLFW/glfw3.h>

namespace omnidesk {

// Store handler pointer in GLFW window user data
void InputHandler::init(GLFWwindow* window, int remoteWidth, int remoteHeight) {
    window_ = window;
    remoteWidth_ = remoteWidth;
    remoteHeight_ = remoteHeight;
    glfwSetWindowUserPointer(window, this);

    glfwSetCursorPosCallback(window, glfwMouseCallback);
    glfwSetMouseButtonCallback(window, glfwButtonCallback);
    glfwSetScrollCallback(window, glfwScrollCallback);
    glfwSetKeyCallback(window, glfwKeyCallback);
}

void InputHandler::setRemoteSize(int width, int height) {
    remoteWidth_ = width;
    remoteHeight_ = height;
}

void InputHandler::poll() {
    // Clipboard sync check (every N frames)
    // TODO: implement clipboard polling
}

void InputHandler::glfwMouseCallback(GLFWwindow* window, double x, double y) {
    auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(window));
    if (!self || !self->enabled_ || !self->onMouse_) return;

    // Map window coordinates to remote desktop coordinates
    int winW, winH;
    glfwGetWindowSize(window, &winW, &winH);

    MouseEvent evt;
    evt.x = static_cast<int32_t>(x * self->remoteWidth_ / winW);
    evt.y = static_cast<int32_t>(y * self->remoteHeight_ / winH);
    evt.isAbsolute = true;

    self->onMouse_(evt);
}

void InputHandler::glfwButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/) {
    auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(window));
    if (!self || !self->enabled_ || !self->onMouse_) return;

    double x, y;
    glfwGetCursorPos(window, &x, &y);
    int winW, winH;
    glfwGetWindowSize(window, &winW, &winH);

    MouseEvent evt;
    evt.x = static_cast<int32_t>(x * self->remoteWidth_ / winW);
    evt.y = static_cast<int32_t>(y * self->remoteHeight_ / winH);
    evt.isAbsolute = true;
    evt.pressed = (action == GLFW_PRESS);

    // Always set button bitmask so the receiver knows which button changed
    if (button == GLFW_MOUSE_BUTTON_LEFT) evt.buttons |= 1;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) evt.buttons |= 2;
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) evt.buttons |= 4;

    self->onMouse_(evt);
}

void InputHandler::glfwScrollCallback(GLFWwindow* window, double xoff, double yoff) {
    auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(window));
    if (!self || !self->enabled_ || !self->onMouse_) return;

    double x, y;
    glfwGetCursorPos(window, &x, &y);
    int winW, winH;
    glfwGetWindowSize(window, &winW, &winH);

    MouseEvent evt;
    evt.x = static_cast<int32_t>(x * self->remoteWidth_ / winW);
    evt.y = static_cast<int32_t>(y * self->remoteHeight_ / winH);
    evt.scrollX = static_cast<int16_t>(xoff * 120);
    evt.scrollY = static_cast<int16_t>(yoff * 120);

    self->onMouse_(evt);
}

void InputHandler::glfwKeyCallback(GLFWwindow* window, int /*key*/, int scancode,
                                    int action, int /*mods*/) {
    auto* self = static_cast<InputHandler*>(glfwGetWindowUserPointer(window));
    if (!self || !self->enabled_ || !self->onKey_) return;

    KeyEvent evt;
    evt.scancode = static_cast<uint32_t>(scancode);
    evt.pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);

    self->onKey_(evt);
}

} // namespace omnidesk
