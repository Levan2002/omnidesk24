#pragma once

#include "ui/d2d_renderer.h"
#include <string>

namespace omnidesk {

class DashboardPanel {
public:
    struct State {
        std::wstring myId = L"--------";
        std::wstring connectId;   // what user typed
        bool signalingConnected = false;
        std::wstring statusMessage;
    };

    void render(D2DRenderer& d2d, float winW, float winH, const State& state);

    // Hit testing -- returns which element was clicked
    enum class HitResult { NONE, CONNECT_BTN, COPY_BTN, SETTINGS_BTN, CONNECT_INPUT };
    HitResult hitTest(float mx, float my) const;

    // Get the connect input rect for text input handling
    struct Rect { float x, y, w, h; };
    Rect connectInputRect() const;

private:
    // Cached layout rects (computed during render)
    Rect connectBtn_{}, copyBtn_{}, settingsBtn_{}, connectInput_{};
};

} // namespace omnidesk
