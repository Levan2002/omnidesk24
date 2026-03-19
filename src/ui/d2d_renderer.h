#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace omnidesk {

class D2DRenderer {
public:
    // Dark theme color constants
    static constexpr D2D1_COLOR_F kBg            = {0.09f, 0.09f, 0.11f, 1.0f};
    static constexpr D2D1_COLOR_F kCard          = {0.13f, 0.13f, 0.16f, 1.0f};
    static constexpr D2D1_COLOR_F kSurface       = {0.16f, 0.16f, 0.20f, 1.0f};
    static constexpr D2D1_COLOR_F kAccent        = {0.26f, 0.52f, 0.96f, 1.0f};
    static constexpr D2D1_COLOR_F kGreen         = {0.25f, 0.78f, 0.42f, 1.0f};
    static constexpr D2D1_COLOR_F kRed           = {0.90f, 0.30f, 0.30f, 1.0f};
    static constexpr D2D1_COLOR_F kText          = {0.94f, 0.95f, 0.96f, 1.0f};
    static constexpr D2D1_COLOR_F kTextMuted     = {0.55f, 0.56f, 0.60f, 1.0f};
    static constexpr D2D1_COLOR_F kBtnSubtle     = {0.18f, 0.18f, 0.22f, 1.0f};
    static constexpr D2D1_COLOR_F kBtnSubtleHover= {0.24f, 0.24f, 0.28f, 1.0f};
    static constexpr D2D1_COLOR_F kBorder        = {0.20f, 0.20f, 0.25f, 0.40f};

    D2DRenderer() = default;
    ~D2DRenderer();

    D2DRenderer(const D2DRenderer&) = delete;
    D2DRenderer& operator=(const D2DRenderer&) = delete;

    bool init(HWND hwnd);
    void shutdown();

    void beginDraw();
    void endDraw();

    void resize(int w, int h);
    void clear(D2D1_COLOR_F color);

    // Drawing helpers
    void fillRect(float x, float y, float w, float h, D2D1_COLOR_F color);
    void fillRoundedRect(float x, float y, float w, float h, float radius, D2D1_COLOR_F color);
    void drawRoundedRect(float x, float y, float w, float h, float radius, D2D1_COLOR_F color, float strokeWidth = 1.0f);
    void drawText(const wchar_t* text, float x, float y, float w, float h,
                  D2D1_COLOR_F color, float fontSize = 0, bool bold = false,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void drawButton(float x, float y, float w, float h, const wchar_t* label,
                    D2D1_COLOR_F bgColor, D2D1_COLOR_F textColor,
                    float radius = 6.0f, float fontSize = 14.0f);

    ID2D1HwndRenderTarget* target() const { return pRenderTarget_; }

private:
    bool createDeviceResources();
    void discardDeviceResources();
    IDWriteTextFormat* selectTextFormat(float fontSize, bool bold) const;

    HWND hwnd_ = nullptr;

    // D2D objects
    ID2D1Factory*          pFactory_      = nullptr;
    ID2D1HwndRenderTarget* pRenderTarget_ = nullptr;
    ID2D1SolidColorBrush*  pBrush_        = nullptr;

    // DirectWrite objects
    IDWriteFactory*    pDWriteFactory_ = nullptr;

    // Pre-cached text formats: regular and bold at common sizes
    // Sizes: 12, 14, 15 (default), 16, 22, 26, 30
    static constexpr int kNumCachedSizes = 7;
    static constexpr float kCachedSizes[kNumCachedSizes] = {12.0f, 14.0f, 15.0f, 16.0f, 22.0f, 26.0f, 30.0f};

    IDWriteTextFormat* pTextFormats_[kNumCachedSizes]     = {};
    IDWriteTextFormat* pTextFormatsBold_[kNumCachedSizes] = {};
};

} // namespace omnidesk
