#include "ui/session_view.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "render/gl_renderer.h"
#include <imgui.h>
#include <cstdint>

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
        GlRenderer* renderer = viewer->renderer();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (renderer && renderer->textureId() != 0) {
            // Render the I420→RGB pass so the output texture is up to date
            renderer->render(static_cast<int>(avail.x), static_cast<int>(avail.y));
            // Display the remote desktop texture
            ImTextureID texId = reinterpret_cast<ImTextureID>(
                static_cast<uintptr_t>(renderer->textureId()));
            ImGui::Image(texId, avail);
        } else {
            ImGui::Text("Waiting for first frame...");
        }
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
