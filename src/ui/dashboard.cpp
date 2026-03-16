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

    // Main dashboard - centered card
    float cardW = 480.0f;
    float cardH = 400.0f;
    ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, winH * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH));

    ImGui::Begin("##Dashboard", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // Title
    ImGui::PushFont(nullptr); // TODO: use large font
    float titleW = ImGui::CalcTextSize("OmniDesk24").x;
    ImGui::SetCursorPosX((cardW - titleW) * 0.5f);
    ImGui::TextColored(ImVec4(Theme::kAccentR, Theme::kAccentG, Theme::kAccentB, 1.0f),
                       "OmniDesk24");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Connection status indicator
    {
        ImVec4 statusColor = signalingConnected
            ? ImVec4(0.3f, 0.85f, 0.4f, 1.0f)   // Green
            : ImVec4(0.85f, 0.3f, 0.3f, 1.0f);   // Red
        ImGui::TextColored(statusColor, "%s", signalingConnected ? "Online" : "Offline");
        ImGui::SameLine();
        ImGui::TextDisabled("Signaling Server");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Your ID section
    ImGui::Text("Your ID:");
    ImGui::Spacing();

    // Large ID display with copy button
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
        ImGui::BeginChild("##IdDisplay", ImVec2(-1, 52), true);

        // Center the ID text
        float idW = ImGui::CalcTextSize(myId.id.c_str()).x * 1.8f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - idW - 80) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);

        // Large monospace ID
        ImGui::SetWindowFontScale(1.8f);
        ImGui::Text("%s", myId.id.c_str());
        ImGui::SetWindowFontScale(1.0f);

        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(myId.id.c_str());
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Connect to peer section
    ImGui::Text("Connect to Remote Desktop:");
    ImGui::Spacing();

    ImGui::PushItemWidth(-100);
    bool enterPressed = ImGui::InputText("##ConnectId", connectIdBuf, connectIdBufSize,
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_CharsUppercase |
                                          ImGuiInputTextFlags_CharsNoBlank);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    bool connectClicked = ImGui::Button("Connect", ImVec2(84, 0));

    if ((enterPressed || connectClicked) && connectIdBuf[0] != '\0') {
        onConnect();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Status message
    if (!statusMessage.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", statusMessage.c_str());
    }

    // Settings button (bottom right)
    ImGui::SetCursorPos(ImVec2(cardW - 100, cardH - 52));
    if (ImGui::Button("Settings", ImVec2(80, 28))) {
        onSettings();
    }

    ImGui::End();
}

} // namespace omnidesk
