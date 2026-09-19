#include "whitehole/app/gui_theme.hpp"

#include "imgui.h"

namespace whitehole::app {
namespace {

// The single accent: electric cyan in dark mode, deep azure in light mode.
// Everything decorative stays neutral; this hue alone signals interaction.
constexpr ImVec4 kAccentDark{0.31F, 0.76F, 0.97F, 1.00F};  // #4FC3F7
constexpr ImVec4 kAccentLight{0.11F, 0.46F, 0.85F, 1.00F}; // #1C75D9

} // namespace

unsigned int accentColor(bool dark) {
    const ImVec4& c = dark ? kAccentDark : kAccentLight;
    return ImGui::GetColorU32(c);
}

void applyWhiteholeTheme(bool dark, float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // --- geometry -----------------------------------------------------------
    // One rounding radius + one padding scale drive the whole look. Rounded
    // corners everywhere, generous but not puffy spacing, hairline borders.
    style.WindowRounding = 7.0F * scale;
    style.ChildRounding = 7.0F * scale;
    style.FrameRounding = 5.0F * scale;
    style.PopupRounding = 6.0F * scale;
    style.ScrollbarRounding = 9.0F * scale;
    style.GrabRounding = 4.0F * scale;
    style.TabRounding = 5.0F * scale;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.ScrollbarSize = 13.0F * scale;
    style.GrabMinSize = 10.0F * scale;
    style.WindowPadding = ImVec2(10.0F * scale, 8.0F * scale);
    style.FramePadding = ImVec2(8.0F * scale, 4.0F * scale);
    style.ItemSpacing = ImVec2(8.0F * scale, 6.0F * scale);
    style.ItemInnerSpacing = ImVec2(6.0F * scale, 4.0F * scale);
    style.CellPadding = ImVec2(6.0F * scale, 3.0F * scale);
    style.IndentSpacing = 18.0F * scale;
    style.WindowTitleAlign = ImVec2(0.0F, 0.5F);

    // Neutral greys with a faint cool tint; surfaces step darker as they go
    // deeper (window > panel > frame), giving natural visual hierarchy.
    const ImVec4 windowBg{0.086F, 0.094F, 0.114F, 1.0F};   // #16181d
    const ImVec4 panelBg{0.114F, 0.125F, 0.153F, 1.0F};    // #1d2027
    const ImVec4 frameBg{0.149F, 0.165F, 0.200F, 1.0F};    // #262a33
    const ImVec4 frameHover{0.196F, 0.216F, 0.263F, 1.0F};
    const ImVec4 frameActive{0.235F, 0.259F, 0.314F, 1.0F};

    const ImVec4& accent = dark ? kAccentDark : kAccentLight;

    if (dark) {
        const ImVec4 text{0.918F, 0.925F, 0.941F, 1.0F};     // #eaeaf0
        const ImVec4 textDim{0.545F, 0.573F, 0.639F, 1.0F};  // #8b92a3
        const ImVec4 border{0.216F, 0.235F, 0.286F, 0.627F}; // #373c49
        const ImVec4 header{0.141F, 0.153F, 0.184F, 1.0F};   // #24272f

        colors[ImGuiCol_Text] = text;
        colors[ImGuiCol_TextDisabled] = textDim;
        colors[ImGuiCol_WindowBg] = windowBg;
        colors[ImGuiCol_ChildBg] = panelBg;
        colors[ImGuiCol_PopupBg] = panelBg;
        colors[ImGuiCol_Border] = border;
        colors[ImGuiCol_TitleBg] = windowBg;
        colors[ImGuiCol_TitleBgActive] = panelBg;
        colors[ImGuiCol_TitleBgCollapsed] = windowBg;
        colors[ImGuiCol_MenuBarBg] = windowBg;
        colors[ImGuiCol_ScrollbarBg] = windowBg;
        colors[ImGuiCol_ScrollbarGrab] = frameBg;
        colors[ImGuiCol_ScrollbarGrabHovered] = frameHover;
        colors[ImGuiCol_ScrollbarGrabActive] = accent;
        colors[ImGuiCol_FrameBg] = frameBg;
        colors[ImGuiCol_FrameBgHovered] = frameHover;
        colors[ImGuiCol_FrameBgActive] = frameActive;
        colors[ImGuiCol_Button] = frameBg;
        colors[ImGuiCol_ButtonHovered] = frameHover;
        colors[ImGuiCol_ButtonActive] = frameActive;
        colors[ImGuiCol_Header] = header;
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.180F, 0.196F, 0.235F, 1.0F);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.208F, 0.227F, 0.275F, 1.0F);
        colors[ImGuiCol_Separator] = border;
        colors[ImGuiCol_SeparatorHovered] = accent;
        colors[ImGuiCol_SeparatorActive] = accent;
        colors[ImGuiCol_ResizeGrip] = border;
        colors[ImGuiCol_ResizeGripHovered] = accent;
        colors[ImGuiCol_ResizeGripActive] = accent;
        colors[ImGuiCol_Tab] = windowBg;
        colors[ImGuiCol_TabHovered] = frameHover;
        colors[ImGuiCol_TabSelected] = panelBg;
        colors[ImGuiCol_TabDimmed] = windowBg;
        colors[ImGuiCol_TabDimmedSelected] = panelBg;
        colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.35F);
        colors[ImGuiCol_DockingEmptyBg] = windowBg;
        colors[ImGuiCol_PlotLines] = accent;
        colors[ImGuiCol_PlotHistogram] = accent;
        colors[ImGuiCol_TableHeaderBg] = header;
        colors[ImGuiCol_TableBorderStrong] = border;
        colors[ImGuiCol_TableBorderLight] = ImVec4(border.x, border.y, border.z, 0.35F);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.03F);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.30F);
        colors[ImGuiCol_DragDropTarget] = accent;
        colors[ImGuiCol_NavCursor] = accent;
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.55F);
    } else {
        // --- light palette --------------------------------------------------
        const ImVec4 windowBgL{0.965F, 0.969F, 0.976F, 1.0F};   // #f6f7f9
        const ImVec4 panelBgL{1.0F, 1.0F, 1.0F, 1.0F};
        const ImVec4 frameBgL{0.906F, 0.914F, 0.929F, 1.0F};    // #e7e9ed
        const ImVec4 frameHoverL{0.839F, 0.851F, 0.875F, 1.0F};
        const ImVec4 frameActiveL{0.784F, 0.800F, 0.831F, 1.0F};
        const ImVec4 textL{0.125F, 0.137F, 0.161F, 1.0F};       // #202329
        const ImVec4 textDimL{0.447F, 0.475F, 0.533F, 1.0F};
        const ImVec4 borderL{0.769F, 0.784F, 0.816F, 1.0F};     // #c4c8d0
        const ImVec4 headerL{0.886F, 0.898F, 0.918F, 1.0F};

        colors[ImGuiCol_Text] = textL;
        colors[ImGuiCol_TextDisabled] = textDimL;
        colors[ImGuiCol_WindowBg] = windowBgL;
        colors[ImGuiCol_ChildBg] = panelBgL;
        colors[ImGuiCol_PopupBg] = panelBgL;
        colors[ImGuiCol_Border] = borderL;
        colors[ImGuiCol_TitleBg] = windowBgL;
        colors[ImGuiCol_TitleBgActive] = panelBgL;
        colors[ImGuiCol_TitleBgCollapsed] = windowBgL;
        colors[ImGuiCol_MenuBarBg] = windowBgL;
        colors[ImGuiCol_ScrollbarBg] = windowBgL;
        colors[ImGuiCol_ScrollbarGrab] = frameBgL;
        colors[ImGuiCol_ScrollbarGrabHovered] = frameHoverL;
        colors[ImGuiCol_ScrollbarGrabActive] = accent;
        colors[ImGuiCol_FrameBg] = frameBgL;
        colors[ImGuiCol_FrameBgHovered] = frameHoverL;
        colors[ImGuiCol_FrameBgActive] = frameActiveL;
        colors[ImGuiCol_Button] = frameBgL;
        colors[ImGuiCol_ButtonHovered] = frameHoverL;
        colors[ImGuiCol_ButtonActive] = frameActiveL;
        colors[ImGuiCol_Header] = headerL;
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.831F, 0.847F, 0.875F, 1.0F);
        colors[ImGuiCol_HeaderActive] = frameActiveL;
        colors[ImGuiCol_Separator] = borderL;
        colors[ImGuiCol_SeparatorHovered] = accent;
        colors[ImGuiCol_SeparatorActive] = accent;
        colors[ImGuiCol_ResizeGrip] = borderL;
        colors[ImGuiCol_ResizeGripHovered] = accent;
        colors[ImGuiCol_ResizeGripActive] = accent;
        colors[ImGuiCol_Tab] = windowBgL;
        colors[ImGuiCol_TabHovered] = frameHoverL;
        colors[ImGuiCol_TabSelected] = panelBgL;
        colors[ImGuiCol_TabDimmed] = windowBgL;
        colors[ImGuiCol_TabDimmedSelected] = panelBgL;
        colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.35F);
        colors[ImGuiCol_DockingEmptyBg] = windowBgL;
        colors[ImGuiCol_PlotLines] = accent;
        colors[ImGuiCol_PlotHistogram] = accent;
        colors[ImGuiCol_TableHeaderBg] = headerL;
        colors[ImGuiCol_TableBorderStrong] = borderL;
        colors[ImGuiCol_TableBorderLight] = ImVec4(borderL.x, borderL.y, borderL.z, 0.45F);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.0F, 0.0F, 0.0F, 0.025F);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.25F);
        colors[ImGuiCol_DragDropTarget] = accent;
        colors[ImGuiCol_NavCursor] = accent;
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.35F);
    }

    // Accent-driven interactive states, identical logic for both themes.
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_PlotLinesHovered] = accent;
    colors[ImGuiCol_PlotHistogramHovered] = accent;
}

} // namespace whitehole::app
