#pragma once

namespace omnidesk {

class Theme {
public:
    // Apply the modern dark theme (AnyDesk / Figma inspired)
    static void apply();

    // Primary accent: vibrant blue
    static constexpr float kAccentR = 0.26f, kAccentG = 0.52f, kAccentB = 0.96f;
    // Secondary accent: subtle purple tint
    static constexpr float kAccent2R = 0.55f, kAccent2G = 0.38f, kAccent2B = 0.92f;
    // Background: deep charcoal
    static constexpr float kBgR = 0.09f, kBgG = 0.09f, kBgB = 0.11f;
    // Card / surface: slightly lighter
    static constexpr float kCardR = 0.13f, kCardG = 0.13f, kCardB = 0.16f;
    // Elevated surface (inputs, sub-panels)
    static constexpr float kSurfaceR = 0.16f, kSurfaceG = 0.16f, kSurfaceB = 0.20f;
    // Success green
    static constexpr float kGreenR = 0.25f, kGreenG = 0.78f, kGreenB = 0.42f;
    // Danger red
    static constexpr float kRedR = 0.90f, kRedG = 0.30f, kRedB = 0.30f;
    // Text primary
    static constexpr float kTextR = 0.94f, kTextG = 0.95f, kTextB = 0.96f;
    // Text secondary / muted
    static constexpr float kTextMutedR = 0.55f, kTextMutedG = 0.56f, kTextMutedB = 0.60f;
};

} // namespace omnidesk
