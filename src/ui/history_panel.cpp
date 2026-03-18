#include "ui/history_panel.h"
#include "ui/theme.h"
#include <imgui.h>

namespace omnidesk {

void HistoryPanel::render() {
    ImGuiIO& io = ImGui::GetIO();

    float uiScale = (io.DisplaySize.x / 1280.0f) < (io.DisplaySize.y / 800.0f)
                    ? (io.DisplaySize.x / 1280.0f) : (io.DisplaySize.y / 800.0f);
    if (uiScale < 0.75f) uiScale = 0.75f;
    if (uiScale > 1.5f) uiScale = 1.5f;

    float panelW = 440.0f * uiScale;
    float panelH = 140.0f * uiScale;

    // Position below the main dashboard card
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f + 230 * uiScale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(Theme::kCardR, Theme::kCardG, Theme::kCardB, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.20f, 0.25f, 0.25f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * uiScale, 16 * uiScale));

    ImGui::Begin("##History", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove);

    ImGui::TextDisabled("Recent Connections");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.22f, 0.22f, 0.27f, 0.35f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // TODO: Load from persistent storage
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10 * uiScale);
    float tw = ImGui::CalcTextSize("No recent connections").x;
    ImGui::SetCursorPosX((panelW - tw) * 0.5f - 20 * uiScale + 10 * uiScale);
    ImGui::TextDisabled("No recent connections");

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

} // namespace omnidesk
