#include "ui/history_panel.h"
#include <imgui.h>

namespace omnidesk {

void HistoryPanel::render() {
    ImGuiIO& io = ImGui::GetIO();

    // History panel below the main card
    float panelW = 480.0f;
    float panelH = 180.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f + 220),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH));

    ImGui::Begin("##History", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove);

    ImGui::Text("Recent Connections");
    ImGui::Separator();
    ImGui::Spacing();

    // TODO: Load from persistent storage
    ImGui::TextDisabled("No recent connections");

    ImGui::End();
}

} // namespace omnidesk
