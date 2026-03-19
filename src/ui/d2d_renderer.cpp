#include "ui/d2d_renderer.h"

#include <cmath>
#include <cstring>

// Link against D2D1 and DWrite
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

namespace omnidesk {

// Static member definitions (C++17 inline constexpr — but provide for older linkers)
constexpr float D2DRenderer::kCachedSizes[];
constexpr D2D1_COLOR_F D2DRenderer::kBg;
constexpr D2D1_COLOR_F D2DRenderer::kCard;
constexpr D2D1_COLOR_F D2DRenderer::kSurface;
constexpr D2D1_COLOR_F D2DRenderer::kAccent;
constexpr D2D1_COLOR_F D2DRenderer::kGreen;
constexpr D2D1_COLOR_F D2DRenderer::kRed;
constexpr D2D1_COLOR_F D2DRenderer::kText;
constexpr D2D1_COLOR_F D2DRenderer::kTextMuted;
constexpr D2D1_COLOR_F D2DRenderer::kBtnSubtle;
constexpr D2D1_COLOR_F D2DRenderer::kBtnSubtleHover;
constexpr D2D1_COLOR_F D2DRenderer::kBorder;

D2DRenderer::~D2DRenderer() {
    shutdown();
}

bool D2DRenderer::init(HWND hwnd) {
    hwnd_ = hwnd;

    // Create D2D factory (single-threaded for simplicity)
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory_);
    if (FAILED(hr)) return false;

    // Create DirectWrite factory
    // Use explicit IID — __uuidof may not work in MinGW
    static const IID IID_IDWriteFactory = {
        0xb859ee5a, 0xd838, 0x4b5b,
        {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}
    };
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        IID_IDWriteFactory,
        reinterpret_cast<IUnknown**>(&pDWriteFactory_)
    );
    if (FAILED(hr)) return false;

    // Create pre-cached text formats for all common sizes
    for (int i = 0; i < kNumCachedSizes; ++i) {
        float size = kCachedSizes[i];

        // Regular weight
        hr = pDWriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"en-us",
            &pTextFormats_[i]
        );
        if (FAILED(hr)) return false;

        pTextFormats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        pTextFormats_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Bold weight
        hr = pDWriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"en-us",
            &pTextFormatsBold_[i]
        );
        if (FAILED(hr)) return false;

        pTextFormatsBold_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        pTextFormatsBold_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Create the render target and brush
    if (!createDeviceResources()) return false;

    return true;
}

void D2DRenderer::shutdown() {
    discardDeviceResources();

    for (int i = 0; i < kNumCachedSizes; ++i) {
        if (pTextFormats_[i])     { pTextFormats_[i]->Release();     pTextFormats_[i] = nullptr; }
        if (pTextFormatsBold_[i]) { pTextFormatsBold_[i]->Release(); pTextFormatsBold_[i] = nullptr; }
    }

    if (pDWriteFactory_) { pDWriteFactory_->Release(); pDWriteFactory_ = nullptr; }
    if (pFactory_)       { pFactory_->Release();       pFactory_ = nullptr; }
}

bool D2DRenderer::createDeviceResources() {
    if (pRenderTarget_) return true; // Already created

    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false; // window not ready yet

    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top)
    );

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(hwnd_, size);

    HRESULT hr = pFactory_->CreateHwndRenderTarget(rtProps, hwndProps, &pRenderTarget_);
    if (FAILED(hr)) return false;

    // Enable anti-aliased text
    pRenderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    // Create a reusable solid color brush (color set before each use)
    hr = pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pBrush_);
    if (FAILED(hr)) return false;

    return true;
}

void D2DRenderer::discardDeviceResources() {
    if (pBrush_)        { pBrush_->Release();        pBrush_ = nullptr; }
    if (pRenderTarget_) { pRenderTarget_->Release();  pRenderTarget_ = nullptr; }
}

void D2DRenderer::beginDraw() {
    createDeviceResources();
    if (pRenderTarget_) {
        pRenderTarget_->BeginDraw();
    }
}

void D2DRenderer::endDraw() {
    if (!pRenderTarget_) return;

    HRESULT hr = pRenderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

void D2DRenderer::resize(int w, int h) {
    if (pRenderTarget_) {
        D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h));
        HRESULT hr = pRenderTarget_->Resize(size);
        if (FAILED(hr)) {
            // If resize fails, discard and recreate on next draw
            discardDeviceResources();
        }
    }
}

void D2DRenderer::clear(D2D1_COLOR_F color) {
    if (pRenderTarget_) {
        pRenderTarget_->Clear(color);
    }
}

void D2DRenderer::fillRect(float x, float y, float w, float h, D2D1_COLOR_F color) {
    if (!pRenderTarget_ || !pBrush_) return;
    pBrush_->SetColor(color);
    pRenderTarget_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), pBrush_);
}

void D2DRenderer::fillRoundedRect(float x, float y, float w, float h, float radius, D2D1_COLOR_F color) {
    if (!pRenderTarget_ || !pBrush_) return;
    pBrush_->SetColor(color);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius);
    pRenderTarget_->FillRoundedRectangle(rr, pBrush_);
}

void D2DRenderer::drawRoundedRect(float x, float y, float w, float h, float radius, D2D1_COLOR_F color, float strokeWidth) {
    if (!pRenderTarget_ || !pBrush_) return;
    pBrush_->SetColor(color);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius);
    pRenderTarget_->DrawRoundedRectangle(rr, pBrush_, strokeWidth);
}

IDWriteTextFormat* D2DRenderer::selectTextFormat(float fontSize, bool bold) const {
    // Find the cached format with the closest size
    int bestIdx = 2; // default index (15px)
    float bestDiff = 1e9f;

    for (int i = 0; i < kNumCachedSizes; ++i) {
        float diff = std::fabs(kCachedSizes[i] - fontSize);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = i;
        }
    }

    return bold ? pTextFormatsBold_[bestIdx] : pTextFormats_[bestIdx];
}

void D2DRenderer::drawText(const wchar_t* text, float x, float y, float w, float h,
                           D2D1_COLOR_F color, float fontSize, bool bold,
                           DWRITE_TEXT_ALIGNMENT align) {
    if (!pRenderTarget_ || !pBrush_ || !text) return;

    // Use default size (15px) if not specified
    if (fontSize <= 0.0f) fontSize = 15.0f;

    IDWriteTextFormat* fmt = selectTextFormat(fontSize, bold);
    if (!fmt) return;

    // Set alignment for this draw call
    fmt->SetTextAlignment(align);

    pBrush_->SetColor(color);

    UINT32 len = 0;
    const wchar_t* p = text;
    while (*p++) ++len;

    D2D1_RECT_F layoutRect = D2D1::RectF(x, y, x + w, y + h);
    pRenderTarget_->DrawText(text, len, fmt, layoutRect, pBrush_);
}

void D2DRenderer::drawButton(float x, float y, float w, float h, const wchar_t* label,
                             D2D1_COLOR_F bgColor, D2D1_COLOR_F textColor,
                             float radius, float fontSize) {
    fillRoundedRect(x, y, w, h, radius, bgColor);
    drawText(label, x, y, w, h, textColor, fontSize, false, DWRITE_TEXT_ALIGNMENT_CENTER);
}

} // namespace omnidesk
