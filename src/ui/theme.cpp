#include "ui/theme.h"
#include <imgui.h>

namespace omnidesk {

void Theme::apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Rounding
    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    // Spacing
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    // Borders
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    // Dark modern palette
    colors[ImGuiCol_WindowBg]           = ImVec4(kBgR, kBgG, kBgB, 1.00f);
    colors[ImGuiCol_ChildBg]            = ImVec4(kCardR, kCardG, kCardB, 1.00f);
    colors[ImGuiCol_PopupBg]            = ImVec4(0.14f, 0.14f, 0.16f, 0.98f);

    colors[ImGuiCol_Border]             = ImVec4(0.25f, 0.25f, 0.28f, 0.60f);
    colors[ImGuiCol_BorderShadow]       = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_Text]               = ImVec4(0.92f, 0.93f, 0.94f, 1.00f);
    colors[ImGuiCol_TextDisabled]       = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);

    colors[ImGuiCol_FrameBg]            = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]      = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);

    colors[ImGuiCol_TitleBg]            = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]      = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.09f, 0.09f, 0.11f, 0.60f);

    colors[ImGuiCol_MenuBarBg]          = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.40f, 0.44f, 1.00f);

    // Accent color for buttons and interactive elements
    colors[ImGuiCol_Button]             = ImVec4(kAccentR, kAccentG, kAccentB, 0.85f);
    colors[ImGuiCol_ButtonHovered]      = ImVec4(kAccentR + 0.05f, kAccentG + 0.05f, kAccentB + 0.03f, 1.00f);
    colors[ImGuiCol_ButtonActive]       = ImVec4(kAccentR - 0.05f, kAccentG - 0.05f, kAccentB - 0.03f, 1.00f);

    colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_HeaderHovered]      = ImVec4(kAccentR, kAccentG, kAccentB, 0.50f);
    colors[ImGuiCol_HeaderActive]       = ImVec4(kAccentR, kAccentG, kAccentB, 0.70f);

    colors[ImGuiCol_Separator]          = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]   = ImVec4(kAccentR, kAccentG, kAccentB, 0.60f);
    colors[ImGuiCol_SeparatorActive]    = ImVec4(kAccentR, kAccentG, kAccentB, 0.80f);

    colors[ImGuiCol_ResizeGrip]         = ImVec4(kAccentR, kAccentG, kAccentB, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]  = ImVec4(kAccentR, kAccentG, kAccentB, 0.60f);
    colors[ImGuiCol_ResizeGripActive]   = ImVec4(kAccentR, kAccentG, kAccentB, 0.85f);

    colors[ImGuiCol_Tab]                = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_TabHovered]         = ImVec4(kAccentR, kAccentG, kAccentB, 0.70f);
    colors[ImGuiCol_TabSelected]        = ImVec4(kAccentR * 0.7f, kAccentG * 0.7f, kAccentB * 0.7f, 1.00f);

    colors[ImGuiCol_CheckMark]          = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);
    colors[ImGuiCol_SliderGrab]         = ImVec4(kAccentR, kAccentG, kAccentB, 0.85f);
    colors[ImGuiCol_SliderGrabActive]   = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);

    colors[ImGuiCol_TableHeaderBg]      = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]  = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
    colors[ImGuiCol_TableBorderLight]   = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
}

} // namespace omnidesk
