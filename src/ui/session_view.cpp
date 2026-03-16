#include "ui/session_view.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include <imgui.h>

namespace omnidesk {

void SessionView::render(HostSession* host, ViewerSession* viewer,
                         std::function<void()> onDisconnect,
                         std::function<void()> onToggleStats,
                         std::function<void()> onToggleSettings) {
    ImGuiIO& io = ImGui::GetIO();

    // Full-screen session window (no decoration)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##Session", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (viewer) {
        // Remote desktop texture will be rendered here by the GL renderer
        // For now, show placeholder
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::Text("Remote Desktop View (%dx%d)", static_cast<int>(avail.x), static_cast<int>(avail.y));

        // TODO: Draw GL texture with ImGui::Image() once renderer provides texture ID
    } else if (host) {
        ImGui::Text("Hosting session...");
        ImGui::Text("A viewer is connected to your desktop.");
    }

    ImGui::End();

    // Floating toolbar (appears on mouse hover at top)
    bool showToolbar = io.MousePos.y < 40.0f;
    if (showToolbar) {
        float toolbarW = 320.0f;
        ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - toolbarW) * 0.5f, 0));
        ImGui::SetNextWindowSize(ImVec2(toolbarW, 36));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##Toolbar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        if (ImGui::Button("Disconnect")) {
            onDisconnect();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stats")) {
            onToggleStats();
        }
        ImGui::SameLine();
        if (ImGui::Button("Settings")) {
            onToggleSettings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("| Ctrl+Alt+F = Fullscreen");

        ImGui::End();
    }
}

} // namespace omnidesk
