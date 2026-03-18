#pragma once

#include "core/types.h"
#include <functional>
#include <cstdint>

struct GLFWwindow;

namespace omnidesk {

struct MouseEvent {
    int32_t x = 0, y = 0;          // Absolute position in remote desktop coords
    int32_t deltaX = 0, deltaY = 0; // Relative movement
    uint8_t buttons = 0;            // Bitmask: bit0=left, bit1=right, bit2=middle
    int16_t scrollX = 0, scrollY = 0;
    bool isAbsolute = true;
    bool pressed = false;           // true = button down, false = button up (valid when buttons != 0)
};

struct KeyEvent {
    uint32_t scancode = 0;
    bool pressed = false;
};

struct ClipboardEvent {
    std::string text;
};

class InputHandler {
public:
    using MouseCallback = std::function<void(const MouseEvent&)>;
    using KeyCallback = std::function<void(const KeyEvent&)>;
    using ClipboardCallback = std::function<void(const ClipboardEvent&)>;

    void init(GLFWwindow* window, int remoteWidth, int remoteHeight);

    void setMouseCallback(MouseCallback cb) { onMouse_ = std::move(cb); }
    void setKeyCallback(KeyCallback cb) { onKey_ = std::move(cb); }
    void setClipboardCallback(ClipboardCallback cb) { onClipboard_ = std::move(cb); }

    // Update remote desktop dimensions (for coordinate mapping)
    void setRemoteSize(int width, int height);

    // Process pending events (called each frame)
    void poll();

    // Enable/disable input capture
    void setEnabled(bool enabled) { enabled_ = enabled; }

private:
    static void glfwMouseCallback(GLFWwindow* window, double x, double y);
    static void glfwButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void glfwScrollCallback(GLFWwindow* window, double xoff, double yoff);
    static void glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

    GLFWwindow* window_ = nullptr;
    MouseCallback onMouse_;
    KeyCallback onKey_;
    ClipboardCallback onClipboard_;

    int remoteWidth_ = 1920;
    int remoteHeight_ = 1080;
    bool enabled_ = false;
};

} // namespace omnidesk
