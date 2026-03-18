#include "ui/session_view.h"
#include "ui/theme.h"
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

    // Full-screen session window (no decoration, pure black background)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##Session", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (viewer) {
        GlRenderer* renderer = viewer->renderer();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (renderer && renderer->textureId() != 0) {
            renderer->render(static_cast<int>(avail.x), static_cast<int>(avail.y));
            ImTextureID texId = reinterpret_cast<ImTextureID>(
                static_cast<uintptr_t>(renderer->textureId()));
            ImGui::Image(texId, avail);
        } else {
            // Centered "waiting" message
            const char* waitMsg = "Waiting for first frame...";
            float tw = ImGui::CalcTextSize(waitMsg).x;
            ImGui::SetCursorPos(ImVec2((avail.x - tw) * 0.5f, avail.y * 0.5f));
            ImGui::TextDisabled("%s", waitMsg);
        }
    } else if (host) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        // Centered host status
        const char* msg1 = "Hosting Session";
        const char* msg2 = "A viewer is connected to your desktop.";
        float tw1 = ImGui::CalcTextSize(msg1).x;
        float tw2 = ImGui::CalcTextSize(msg2).x;
        ImGui::SetCursorPos(ImVec2((avail.x - tw1) * 0.5f, avail.y * 0.45f));
        ImGui::SetWindowFontScale(1.3f);
        ImGui::TextColored(ImVec4(Theme::kAccentR, Theme::kAccentG, Theme::kAccentB, 1.0f),
                           "%s", msg1);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SetCursorPosX((avail.x - tw2) * 0.5f);
        ImGui::TextDisabled("%s", msg2);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ============= Floating Toolbar =============
    // Appears when mouse is near the top of the screen.
    // Uses a pill-shaped bar centered at top with subtle transparency.
    bool showToolbar = (io.MousePos.y < 48.0f) || (io.MousePos.y < 80.0f && io.MousePos.y >= 0);
    if (showToolbar) {
        float toolbarW = 380.0f;
        float toolbarH = 42.0f;

        ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - toolbarW) * 0.5f, 8));
        ImGui::SetNextWindowSize(ImVec2(toolbarW, toolbarH));
        ImGui::SetNextWindowBgAlpha(0.88f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 6));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.28f, 0.50f));

        ImGui::Begin("##Toolbar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        float btnH = 28.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        // Disconnect button (red accent)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Theme::kRedR, Theme::kRedG, Theme::kRedB, 0.90f));
        if (ImGui::Button("Disconnect", ImVec2(100, btnH))) {
            onDisconnect();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 12);

        // Vertical separator
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.30f, 0.35f, 1.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ImGui::Text("|");
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4);
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 12);

        // Stats button (subtle)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
        if (ImGui::Button("Stats", ImVec2(60, btnH))) {
            onToggleStats();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 6);

        // Settings button (subtle)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
        if (ImGui::Button("Settings", ImVec2(76, btnH))) {
            onToggleSettings();
        }
        ImGui::PopStyleColor(2);

        ImGui::PopStyleVar(); // FrameRounding

        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

} // namespace omnidesk
