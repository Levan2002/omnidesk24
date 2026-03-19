#include "ui/dashboard_panel.h"
#include <algorithm>

namespace omnidesk {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static bool inRect(float mx, float my, const DashboardPanel::Rect& r) {
    return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void DashboardPanel::render(D2DRenderer& d2d, float winW, float winH,
                            const DashboardPanel::State& state) {
    // 1. Full-window dark background
    d2d.fillRect(0, 0, winW, winH, D2DRenderer::kBg);

    // ---- responsive sizing ------------------------------------------------
    const float scale      = (std::min)(winW, winH) / 900.0f;   // reference 900
    const float cardW      = winW * 0.35f;                       // ~35 % of window
    const float minCardW   = 340.0f * scale;
    const float actualCardW = (std::max)(cardW, minCardW);
    const float pad        = 24.0f * scale;
    const float lineH      = 22.0f * scale;                      // spacing unit
    const float rowGap     = 10.0f * scale;

    // font sizes (clamped to cached sizes available in D2DRenderer)
    const float fTitle     = 22.0f;
    const float fId        = 26.0f;
    const float fLabel     = 14.0f;
    const float fBody      = 15.0f;
    const float fBtn       = 14.0f;
    const float fSmall     = 12.0f;

    // vertical metrics for main card content
    const float titleH     = 30.0f * scale;
    const float pillH      = 22.0f * scale;
    const float pillGap    = 8.0f  * scale;
    const float labelH     = 20.0f * scale;
    const float idBoxH     = 48.0f * scale;
    const float inputH     = 38.0f * scale;
    const float statusH    = 20.0f * scale;
    const float settingsBtnH = 30.0f * scale;

    const float cardContentH = titleH + pillGap + pillH + rowGap * 2
                              + labelH + rowGap + idBoxH + rowGap * 2
                              + labelH + rowGap + inputH + rowGap
                              + statusH + rowGap + settingsBtnH;
    const float cardH      = cardContentH + pad * 2;

    // recent-connections card
    const float recentGap  = 16.0f * scale;
    const float recentH    = 70.0f * scale;

    // center both cards as a group
    const float totalH     = cardH + recentGap + recentH;
    const float startY     = winH * 0.40f - totalH * 0.45f;      // ~40 % from top

    const float cardX      = (winW - actualCardW) * 0.5f;
    const float cardY      = startY;

    // ======================================================================
    // 2. Main card
    // ======================================================================
    d2d.fillRoundedRect(cardX, cardY, actualCardW, cardH, 14.0f, D2DRenderer::kCard);

    float cx = cardX + pad;
    float cy = cardY + pad;
    const float innerW = actualCardW - pad * 2;

    // -- Title: "OmniDesk24" centered ---------------------------------------
    d2d.drawText(L"OmniDesk24", cx, cy, innerW, titleH,
                 D2DRenderer::kAccent, fTitle, /*bold=*/true,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    cy += titleH + pillGap;

    // -- Status pill --------------------------------------------------------
    {
        const bool online = state.signalingConnected;
        const wchar_t* pillLabel = online ? L"Online" : L"Offline";
        const D2D1_COLOR_F pillColor = online ? D2DRenderer::kGreen : D2DRenderer::kRed;

        const float pillW = 72.0f * scale;
        const float pillX = cx + (innerW - pillW) * 0.5f;
        d2d.fillRoundedRect(pillX, cy, pillW, pillH, pillH * 0.5f, pillColor);
        d2d.drawText(pillLabel, pillX, cy, pillW, pillH,
                     D2DRenderer::kText, fSmall, /*bold=*/true,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    cy += pillH + rowGap * 2;

    // -- "Your Device ID" label ---------------------------------------------
    d2d.drawText(L"Your Device ID", cx, cy, innerW, labelH,
                 D2DRenderer::kTextMuted, fLabel, false,
                 DWRITE_TEXT_ALIGNMENT_LEADING);
    cy += labelH + rowGap;

    // -- ID display box with Copy button ------------------------------------
    {
        const float copyBtnW = 56.0f * scale;
        const float gap      = 8.0f * scale;
        const float idW      = innerW - copyBtnW - gap;

        // surface background
        d2d.fillRoundedRect(cx, cy, idW, idBoxH, 8.0f, D2DRenderer::kSurface);

        // ID text centered inside box
        d2d.drawText(state.myId.c_str(), cx, cy, idW, idBoxH,
                     D2DRenderer::kText, fId, /*bold=*/true,
                     DWRITE_TEXT_ALIGNMENT_CENTER);

        // Copy button
        const float copyBtnX = cx + idW + gap;
        d2d.drawButton(copyBtnX, cy + (idBoxH - 30.0f * scale) * 0.5f,
                       copyBtnW, 30.0f * scale, L"Copy",
                       D2DRenderer::kBtnSubtle, D2DRenderer::kText, 6.0f, fBtn);
        copyBtn_ = { copyBtnX, cy + (idBoxH - 30.0f * scale) * 0.5f,
                     copyBtnW, 30.0f * scale };
    }
    cy += idBoxH + rowGap * 2;

    // -- "Connect to Remote Desktop" label ----------------------------------
    d2d.drawText(L"Connect to Remote Desktop", cx, cy, innerW, labelH,
                 D2DRenderer::kTextMuted, fLabel, false,
                 DWRITE_TEXT_ALIGNMENT_LEADING);
    cy += labelH + rowGap;

    // -- Connect input + Connect button -------------------------------------
    {
        const float connectBtnW = 80.0f * scale;
        const float gap         = 8.0f * scale;
        const float inputW      = innerW - connectBtnW - gap;

        // input field background
        d2d.fillRoundedRect(cx, cy, inputW, inputH, 6.0f, D2DRenderer::kSurface);
        d2d.drawRoundedRect(cx, cy, inputW, inputH, 6.0f, D2DRenderer::kBorder);

        // text inside input (or placeholder)
        if (state.connectId.empty()) {
            d2d.drawText(L"Enter ID...", cx + 10.0f * scale, cy, inputW - 12.0f * scale, inputH,
                         D2DRenderer::kTextMuted, fBody, false,
                         DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            d2d.drawText(state.connectId.c_str(), cx + 10.0f * scale, cy,
                         inputW - 12.0f * scale, inputH,
                         D2DRenderer::kText, fBody, false,
                         DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        connectInput_ = { cx, cy, inputW, inputH };

        // Connect button (accent blue)
        const float btnX = cx + inputW + gap;
        d2d.drawButton(btnX, cy, connectBtnW, inputH, L"Connect",
                       D2DRenderer::kAccent, D2DRenderer::kText, 6.0f, fBtn);
        connectBtn_ = { btnX, cy, connectBtnW, inputH };
    }
    cy += inputH + rowGap;

    // -- Status message (amber/orange warning) ------------------------------
    if (!state.statusMessage.empty()) {
        const D2D1_COLOR_F kAmber = { 0.95f, 0.70f, 0.20f, 1.0f };
        d2d.drawText(state.statusMessage.c_str(), cx, cy, innerW, statusH,
                     kAmber, fSmall, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    cy += statusH + rowGap;

    // -- Settings button (bottom-right, subtle) -----------------------------
    {
        const float settBtnW = 76.0f * scale;
        const float settBtnX = cx + innerW - settBtnW;
        d2d.drawButton(settBtnX, cy, settBtnW, settingsBtnH, L"Settings",
                       D2DRenderer::kBtnSubtle, D2DRenderer::kTextMuted, 6.0f, fSmall);
        settingsBtn_ = { settBtnX, cy, settBtnW, settingsBtnH };
    }

    // ======================================================================
    // 3. Recent Connections card
    // ======================================================================
    const float recentY = cardY + cardH + recentGap;
    d2d.fillRoundedRect(cardX, recentY, actualCardW, recentH, 14.0f, D2DRenderer::kCard);

    d2d.drawText(L"Recent Connections", cardX + pad, recentY + 10.0f * scale,
                 innerW, 22.0f * scale,
                 D2DRenderer::kText, fBody, /*bold=*/true,
                 DWRITE_TEXT_ALIGNMENT_LEADING);
    d2d.drawText(L"No recent connections", cardX + pad, recentY + 34.0f * scale,
                 innerW, 20.0f * scale,
                 D2DRenderer::kTextMuted, fSmall, false,
                 DWRITE_TEXT_ALIGNMENT_LEADING);
}

// ---------------------------------------------------------------------------
// hitTest
// ---------------------------------------------------------------------------
DashboardPanel::HitResult DashboardPanel::hitTest(float mx, float my) const {
    if (inRect(mx, my, connectBtn_))   return HitResult::CONNECT_BTN;
    if (inRect(mx, my, copyBtn_))      return HitResult::COPY_BTN;
    if (inRect(mx, my, settingsBtn_))  return HitResult::SETTINGS_BTN;
    if (inRect(mx, my, connectInput_)) return HitResult::CONNECT_INPUT;
    return HitResult::NONE;
}

// ---------------------------------------------------------------------------
// connectInputRect
// ---------------------------------------------------------------------------
DashboardPanel::Rect DashboardPanel::connectInputRect() const {
    return connectInput_;
}

} // namespace omnidesk
