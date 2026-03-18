#include "ui/dashboard.h"
#include "ui/theme.h"
#include <imgui.h>

namespace omnidesk {

void Dashboard::render(const UserID& myId,
                       char* connectIdBuf, size_t connectIdBufSize,
                       std::function<void()> onConnect,
                       std::function<void()> onSettings,
                       bool signalingConnected,
                       const std::string& statusMessage) {
    ImGuiIO& io = ImGui::GetIO();
    float winW = io.DisplaySize.x;
    float winH = io.DisplaySize.y;

    // Adaptive scaling for different window sizes
    float uiScale = (winW / 1280.0f) < (winH / 800.0f) ? (winW / 1280.0f) : (winH / 800.0f);
    if (uiScale < 0.75f) uiScale = 0.75f;
    if (uiScale > 1.5f) uiScale = 1.5f;

    float cardW = 440.0f * uiScale;
    float cardH = 420.0f * uiScale;

    // Full-screen background fill (dark gradient simulation)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(Theme::kBgR, Theme::kBgG, Theme::kBgB, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##BgFill", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ============== Main Card ==============
    ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, winH * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(Theme::kCardR, Theme::kCardG, Theme::kCardB, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.20f, 0.25f, 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * uiScale, 24 * uiScale));

    ImGui::Begin("##Dashboard", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // ---- App Title ----
    {
        ImGui::SetWindowFontScale(1.4f);
        float titleW = ImGui::CalcTextSize("OmniDesk24").x;
        ImGui::SetCursorPosX((cardW - titleW) * 0.5f - 28 * uiScale + 14 * uiScale);
        ImGui::TextColored(ImVec4(Theme::kAccentR, Theme::kAccentG, Theme::kAccentB, 1.0f),
                           "OmniDesk24");
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::Spacing();

    // ---- Connection Status Pill ----
    {
        ImVec4 pillColor = signalingConnected
            ? ImVec4(Theme::kGreenR, Theme::kGreenG, Theme::kGreenB, 0.90f)
            : ImVec4(Theme::kRedR, Theme::kRedG, Theme::kRedB, 0.90f);
        const char* statusText = signalingConnected ? "  Online  " : "  Offline  ";
        float pillW = ImGui::CalcTextSize(statusText).x + 16;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - pillW) * 0.5f);

        ImGui::PushStyleColor(ImGuiCol_Button, pillColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pillColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, pillColor);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::SmallButton(statusText);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ---- Your ID Section ----
    {
        ImGui::TextDisabled("Your Device ID");
        ImGui::Spacing();

        // ID display box
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(Theme::kSurfaceR, Theme::kSurfaceG, Theme::kSurfaceB, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * uiScale);
        float idBoxH = 52.0f * uiScale;
        ImGui::BeginChild("##IdDisplay", ImVec2(-1, idBoxH), true);

        // Center ID text + copy button
        float innerW = ImGui::GetContentRegionAvail().x;
        ImGui::SetWindowFontScale(1.9f);
        float idTextW = ImGui::CalcTextSize(myId.id.c_str()).x;
        ImGui::SetWindowFontScale(1.0f);

        float copyBtnW = 50.0f * uiScale;
        float totalContentW = idTextW + copyBtnW + 12;
        float startX = (innerW - totalContentW) * 0.5f;
        if (startX < 4) startX = 4;

        ImGui::SetCursorPos(ImVec2(startX, (idBoxH - 28 * uiScale) * 0.5f - 4));
        ImGui::SetWindowFontScale(1.9f);
        ImGui::TextColored(ImVec4(Theme::kTextR, Theme::kTextG, Theme::kTextB, 1.0f),
                           "%s", myId.id.c_str());
        ImGui::SetWindowFontScale(1.0f);

        ImGui::SameLine(0, 12);
        ImGui::SetCursorPosY((idBoxH - 24 * uiScale) * 0.5f - 4);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.22f, 0.27f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.33f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(myId.id.c_str());
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    // ---- Connect Section ----
    {
        ImGui::TextDisabled("Connect to Remote Desktop");
        ImGui::Spacing();

        // Input field + button in one row
        float btnW = 100.0f * uiScale;
        float inputW = ImGui::GetContentRegionAvail().x - btnW - 10;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 10));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(Theme::kSurfaceR, Theme::kSurfaceG, Theme::kSurfaceB, 1.0f));

        ImGui::PushItemWidth(inputW);
        bool enterPressed = ImGui::InputText("##ConnectId", connectIdBuf, connectIdBufSize,
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_CharsUppercase |
                                              ImGuiInputTextFlags_CharsNoBlank);
        ImGui::PopItemWidth();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0, 10);

        // Connect button
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 10));
        bool connectClicked = ImGui::Button("Connect", ImVec2(btnW, 0));
        ImGui::PopStyleVar(2);

        if ((enterPressed || connectClicked) && connectIdBuf[0] != '\0') {
            onConnect();
        }
    }

    ImGui::Spacing();

    // ---- Status Message ----
    if (!statusMessage.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "%s", statusMessage.c_str());
    }

    // ---- Settings Button (bottom-right) ----
    {
        float settingsBtnW = 90.0f * uiScale;
        float settingsBtnH = 32.0f * uiScale;
        float bottomPad = 28.0f * uiScale;
        float rightPad = 28.0f * uiScale;
        ImGui::SetCursorPos(ImVec2(cardW - settingsBtnW - rightPad,
                                    cardH - settingsBtnH - bottomPad));

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.24f, 0.28f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (ImGui::Button("Settings", ImVec2(settingsBtnW, settingsBtnH))) {
            onSettings();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

} // namespace omnidesk
