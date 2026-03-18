#include "ui/theme.h"
#include <imgui.h>

namespace omnidesk {

void Theme::apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // ---- Geometry: generous rounding for a modern, pill-shaped feel ----
    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    // ---- Spacing: breathable layout ----
    style.WindowPadding     = ImVec2(20, 20);
    style.FramePadding      = ImVec2(12, 6);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(8, 6);
    style.IndentSpacing     = 22.0f;
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 10.0f;

    // ---- Borders: subtle, only where needed ----
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;
    style.TabBarBorderSize  = 0.0f;

    // ---- Anti-aliased ----
    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;

    // ====================================================================
    //  Color Palette
    // ====================================================================

    // -- Backgrounds --
    colors[ImGuiCol_WindowBg]             = ImVec4(kBgR, kBgG, kBgB, 1.00f);
    colors[ImGuiCol_ChildBg]              = ImVec4(kCardR, kCardG, kCardB, 1.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.11f, 0.14f, 0.98f);

    // -- Borders --
    colors[ImGuiCol_Border]               = ImVec4(0.20f, 0.20f, 0.25f, 0.40f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // -- Text --
    colors[ImGuiCol_Text]                 = ImVec4(kTextR, kTextG, kTextB, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(kTextMutedR, kTextMutedG, kTextMutedB, 1.00f);

    // -- Input frames: elevated surface --
    colors[ImGuiCol_FrameBg]              = ImVec4(kSurfaceR, kSurfaceG, kSurfaceB, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(kSurfaceR + 0.04f, kSurfaceG + 0.04f, kSurfaceB + 0.05f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(kSurfaceR + 0.07f, kSurfaceG + 0.07f, kSurfaceB + 0.09f, 1.00f);

    // -- Title bar --
    colors[ImGuiCol_TitleBg]              = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.07f, 0.07f, 0.09f, 0.60f);

    // -- Menu bar --
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);

    // -- Scrollbar --
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);  // transparent track
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.28f, 0.33f, 0.70f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.40f, 0.85f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.42f, 0.48f, 1.00f);

    // -- Primary action buttons: vivid accent --
    colors[ImGuiCol_Button]               = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(kAccentR + 0.08f, kAccentG + 0.06f, kAccentB + 0.02f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(kAccentR - 0.06f, kAccentG - 0.06f, kAccentB - 0.04f, 1.00f);

    // -- Headers (collapsing headers, table headers) --
    colors[ImGuiCol_Header]               = ImVec4(kSurfaceR, kSurfaceG, kSurfaceB, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(kAccentR, kAccentG, kAccentB, 0.30f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(kAccentR, kAccentG, kAccentB, 0.50f);

    // -- Separators --
    colors[ImGuiCol_Separator]            = ImVec4(0.22f, 0.22f, 0.27f, 0.40f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(kAccentR, kAccentG, kAccentB, 0.50f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(kAccentR, kAccentG, kAccentB, 0.80f);

    // -- Resize grips --
    colors[ImGuiCol_ResizeGrip]           = ImVec4(kAccentR, kAccentG, kAccentB, 0.15f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(kAccentR, kAccentG, kAccentB, 0.50f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(kAccentR, kAccentG, kAccentB, 0.80f);

    // -- Tabs --
    colors[ImGuiCol_Tab]                  = ImVec4(kCardR, kCardG, kCardB, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(kAccentR, kAccentG, kAccentB, 0.55f);
    colors[ImGuiCol_TabSelected]          = ImVec4(kAccentR * 0.6f, kAccentG * 0.6f, kAccentB * 0.6f, 1.00f);

    // -- Checkboxes, sliders --
    colors[ImGuiCol_CheckMark]            = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(kAccentR, kAccentG, kAccentB, 0.90f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);

    // -- Tables --
    colors[ImGuiCol_TableHeaderBg]        = ImVec4(kCardR, kCardG, kCardB, 1.00f);
    colors[ImGuiCol_TableBorderStrong]    = ImVec4(0.22f, 0.22f, 0.27f, 0.80f);
    colors[ImGuiCol_TableBorderLight]     = ImVec4(0.18f, 0.18f, 0.22f, 0.60f);

    // -- Modal overlay dimming --
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

} // namespace omnidesk
