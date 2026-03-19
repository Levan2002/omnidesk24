#pragma once

namespace omnidesk {

// Color constants used by C++ code (glClearColor etc.)
// UI styling is in assets/styles/theme.rcss
class Theme {
public:
    static constexpr float kAccentR = 0.26f, kAccentG = 0.52f, kAccentB = 0.96f;
    static constexpr float kBgR = 0.09f, kBgG = 0.09f, kBgB = 0.11f;
    static constexpr float kCardR = 0.13f, kCardG = 0.13f, kCardB = 0.16f;
    static constexpr float kSurfaceR = 0.16f, kSurfaceG = 0.16f, kSurfaceB = 0.20f;
    static constexpr float kGreenR = 0.25f, kGreenG = 0.78f, kGreenB = 0.42f;
    static constexpr float kRedR = 0.90f, kRedG = 0.30f, kRedB = 0.30f;
    static constexpr float kTextR = 0.94f, kTextG = 0.95f, kTextB = 0.96f;
    static constexpr float kTextMutedR = 0.55f, kTextMutedG = 0.56f, kTextMutedB = 0.60f;
};

} // namespace omnidesk
