#include "ui/connection_dialog.h"
#include "ui/theme.h"
#include <imgui.h>

namespace omnidesk {

void ConnectionDialog::render(const UserID& fromUser,
                              std::function<void()> onAccept,
                              std::function<void()> onReject) {
    ImGui::OpenPopup("##IncomingConn");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380, 240));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(Theme::kCardR, Theme::kCardG, Theme::kCardB, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.28f, 0.40f));

    if (ImGui::BeginPopupModal("##IncomingConn", nullptr,
                                ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove)) {

        // Icon placeholder (user avatar circle)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 winPos = ImGui::GetWindowPos();
            float cx = winPos.x + 190.0f;
            float cy = winPos.y + 48.0f;
            draw->AddCircleFilled(ImVec2(cx, cy), 22.0f,
                                  IM_COL32(static_cast<int>(Theme::kAccentR * 255),
                                           static_cast<int>(Theme::kAccentG * 255),
                                           static_cast<int>(Theme::kAccentB * 255), 60), 32);
            draw->AddCircleFilled(ImVec2(cx, cy), 16.0f,
                                  IM_COL32(static_cast<int>(Theme::kAccentR * 255),
                                           static_cast<int>(Theme::kAccentG * 255),
                                           static_cast<int>(Theme::kAccentB * 255), 120), 32);
            // Simple user icon: head circle + body arc
            draw->AddCircleFilled(ImVec2(cx, cy - 4), 6.0f,
                                  IM_COL32(255, 255, 255, 200), 16);
            draw->AddCircleFilled(ImVec2(cx, cy + 10), 9.0f,
                                  IM_COL32(255, 255, 255, 200), 16);
        }

        ImGui::SetCursorPosY(78);

        // Title
        {
            const char* title = "Incoming Connection";
            float tw = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((380.0f - tw) * 0.5f - 28 + 14);
            ImGui::TextColored(ImVec4(Theme::kTextR, Theme::kTextG, Theme::kTextB, 1.0f), "%s", title);
        }

        ImGui::Spacing();

        // Requester ID
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(Theme::kSurfaceR, Theme::kSurfaceG, Theme::kSurfaceB, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::BeginChild("##ReqId", ImVec2(-1, 38), true);

            ImGui::SetWindowFontScale(1.4f);
            float idW = ImGui::CalcTextSize(fromUser.id.c_str()).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - idW) * 0.5f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
            ImGui::TextColored(ImVec4(Theme::kAccentR, Theme::kAccentG, Theme::kAccentB, 1.0f),
                               "%s", fromUser.id.c_str());
            ImGui::SetWindowFontScale(1.0f);

            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        {
            const char* desc = "wants to view your desktop";
            float dw = ImGui::CalcTextSize(desc).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - dw) * 0.5f);
            ImGui::TextDisabled("%s", desc);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Buttons: Accept (green) | Reject (subtle)
        float buttonW = 130.0f;
        float spacing = 16.0f;
        float totalW = buttonW * 2 + spacing;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - totalW) * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        // Accept button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(Theme::kGreenR, Theme::kGreenG, Theme::kGreenB, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Theme::kGreenR + 0.05f, Theme::kGreenG + 0.05f, Theme::kGreenB + 0.02f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(Theme::kGreenR - 0.05f, Theme::kGreenG - 0.05f, Theme::kGreenB - 0.02f, 1.0f));
        if (ImGui::Button("Accept", ImVec2(buttonW, 36))) {
            ImGui::CloseCurrentPopup();
            onAccept();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, spacing);

        // Reject button (subdued)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.18f, 0.18f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Theme::kRedR, Theme::kRedG, Theme::kRedB, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(Theme::kRedR - 0.05f, Theme::kRedG - 0.05f, Theme::kRedB, 0.90f));
        if (ImGui::Button("Reject", ImVec2(buttonW, 36))) {
            ImGui::CloseCurrentPopup();
            onReject();
        }
        ImGui::PopStyleColor(3);

        ImGui::PopStyleVar();

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace omnidesk
