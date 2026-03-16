#include "ui/connection_dialog.h"
#include "ui/theme.h"
#include <imgui.h>

namespace omnidesk {

void ConnectionDialog::render(const UserID& fromUser,
                              std::function<void()> onAccept,
                              std::function<void()> onReject) {
    ImGui::OpenPopup("Incoming Connection");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360, 180));

    if (ImGui::BeginPopupModal("Incoming Connection", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Spacing();
        ImGui::Text("Connection request from:");
        ImGui::Spacing();

        // Show requester's ID prominently
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
        ImGui::BeginChild("##RequesterId", ImVec2(-1, 40), true);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        float idW = ImGui::CalcTextSize(fromUser.id.c_str()).x * 1.5f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - idW) * 0.5f);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(Theme::kAccentR, Theme::kAccentG, Theme::kAccentB, 1.0f),
                           "%s", fromUser.id.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::TextDisabled("They want to view your desktop.");
        ImGui::Spacing();
        ImGui::Spacing();

        // Accept / Reject buttons
        float buttonW = 120.0f;
        float spacing = 20.0f;
        float totalW = buttonW * 2 + spacing;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - totalW) * 0.5f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.7f, 0.35f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.75f, 0.4f, 1.0f));
        if (ImGui::Button("Accept", ImVec2(buttonW, 32))) {
            ImGui::CloseCurrentPopup();
            onAccept();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, spacing);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.3f, 0.3f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.35f, 0.35f, 1.0f));
        if (ImGui::Button("Reject", ImVec2(buttonW, 32))) {
            ImGui::CloseCurrentPopup();
            onReject();
        }
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }
}

} // namespace omnidesk
