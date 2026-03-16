#pragma once

namespace omnidesk {

class Theme {
public:
    // Apply the dark modern theme (VS Code / Discord inspired)
    static void apply();

    // Color constants
    static constexpr float kAccentR = 0.35f, kAccentG = 0.55f, kAccentB = 0.95f;
    static constexpr float kBgR = 0.11f, kBgG = 0.11f, kBgB = 0.13f;
    static constexpr float kCardR = 0.16f, kCardG = 0.16f, kCardB = 0.19f;
};

} // namespace omnidesk
