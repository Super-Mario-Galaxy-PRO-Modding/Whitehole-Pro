#include "whitehole/app/gui_theme.hpp"

#include "imgui.h"
#include "whitehole/app/theme_palette.hpp"

namespace whitehole::app {
namespace {

ImVec4 toVec(const Rgba& c) { return ImVec4(c.r, c.g, c.b, c.a); }

// Same hue as `c`, different alpha -- for slots ImGui draws as a wash.
ImVec4 alphaOf(const Rgba& c, float alpha) { return ImVec4(c.r, c.g, c.b, alpha); }

// Moves `base` towards `tint`. Interactive states are derived this way instead
// of being spelled out per theme, which is what previously let the light palette
// inherit dark button fills.
ImVec4 mix(const ImVec4& base, const ImVec4& tint, float amount) {
    return ImVec4(base.x + (tint.x - base.x) * amount,
                  base.y + (tint.y - base.y) * amount,
                  base.z + (tint.z - base.z) * amount, 1.0F);
}

} // namespace

unsigned int accentColor(bool dark) {
    return ImGui::GetColorU32(toVec(themePalette(dark).accent));
}

void applyWhiteholeTheme(bool dark, float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    const Palette& p = themePalette(dark);
    const ImVec4 accent = toVec(p.accent);

    // --- geometry -----------------------------------------------------------
    // One rounding radius and one padding scale drive the whole look. Soft
    // corners, calm spacing, hairline borders; dock tabs stay square-ish so the
    // tab strip reads as one connected bar.
    style.WindowRounding = 8.0F * scale;
    style.ChildRounding = 8.0F * scale;
    style.FrameRounding = 6.0F * scale;
    style.PopupRounding = 8.0F * scale;
    style.ScrollbarRounding = 9.0F * scale;
    style.GrabRounding = 5.0F * scale;
    style.TabRounding = 4.0F * scale;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.ScrollbarSize = 13.0F * scale;
    style.GrabMinSize = 10.0F * scale;
    style.WindowPadding = ImVec2(12.0F * scale, 10.0F * scale);
    style.FramePadding = ImVec2(10.0F * scale, 5.0F * scale);
    style.ItemSpacing = ImVec2(10.0F * scale, 7.0F * scale);
    style.ItemInnerSpacing = ImVec2(6.0F * scale, 4.0F * scale);
    style.CellPadding = ImVec2(6.0F * scale, 3.0F * scale);
    style.IndentSpacing = 18.0F * scale;
    style.WindowTitleAlign = ImVec2(0.0F, 0.5F);

    // ImGui defaults this to 0.60, which left text inside BeginDisabled() at
    // 2.87:1 in dark mode and 2.34:1 in light. The palette raises it and the
    // contrast test pins the outcome.
    style.DisabledAlpha = p.disabledAlpha;

    // A few slots want a theme-specific wash rather than a palette role.
    const ImVec4 rowAltWash = dark ? ImVec4(1, 1, 1, 0.03F) : ImVec4(0, 0, 0, 0.025F);
    const float treeLineAlpha = dark ? 0.55F : 0.75F;
    const float tableBorderLightAlpha = dark ? 0.35F : 0.45F;
    const float checkboxAlpha = dark ? 1.00F : 0.85F;
    const float dropTargetAlpha = dark ? 0.14F : 0.12F;

    // --- surfaces -----------------------------------------------------------
    colors[ImGuiCol_WindowBg] = toVec(p.windowBg);
    colors[ImGuiCol_ChildBg] = toVec(p.panelBg);
    colors[ImGuiCol_PopupBg] = toVec(p.panelBg);
    colors[ImGuiCol_MenuBarBg] = toVec(p.windowBg);
    colors[ImGuiCol_TitleBg] = toVec(p.windowBg);
    colors[ImGuiCol_TitleBgActive] = toVec(p.panelBg);
    colors[ImGuiCol_TitleBgCollapsed] = toVec(p.windowBg);
    colors[ImGuiCol_DockingEmptyBg] = toVec(p.windowBg);
    colors[ImGuiCol_Border] = toVec(p.border);
    colors[ImGuiCol_Separator] = toVec(p.border);

    // --- inputs, buttons ----------------------------------------------------
    colors[ImGuiCol_FrameBg] = toVec(p.frameBg);
    colors[ImGuiCol_FrameBgHovered] = toVec(p.frameHover);
    colors[ImGuiCol_FrameBgActive] = toVec(p.frameActive);
    colors[ImGuiCol_Button] = toVec(p.frameBg);
    colors[ImGuiCol_ButtonHovered] = mix(toVec(p.frameHover), toVec(p.accentFg), 0.35F);
    colors[ImGuiCol_ButtonActive] = toVec(p.frameActive);

    // --- rows, headers ------------------------------------------------------
    colors[ImGuiCol_Header] = toVec(p.header);
    colors[ImGuiCol_HeaderHovered] = mix(toVec(p.headerHover), toVec(p.accentFg), 0.25F);
    colors[ImGuiCol_HeaderActive] = toVec(p.headerActive);

    // --- tabs ---------------------------------------------------------------
    colors[ImGuiCol_Tab] = toVec(p.windowBg);
    colors[ImGuiCol_TabHovered] = mix(toVec(p.frameHover), toVec(p.accentFg), 0.35F);
    colors[ImGuiCol_TabSelected] = toVec(p.tabSelected);
    colors[ImGuiCol_TabDimmed] = toVec(p.windowBg);
    colors[ImGuiCol_TabDimmedSelected] = toVec(p.tabSelected);
    colors[ImGuiCol_TabSelectedOverline] = toVec(p.accentFg);
    colors[ImGuiCol_TabDimmedSelectedOverline] = alphaOf(p.accentFg, 0.45F);

    // --- ink ----------------------------------------------------------------
    colors[ImGuiCol_Text] = toVec(p.text);
    colors[ImGuiCol_TextDisabled] = toVec(p.textDim);
    colors[ImGuiCol_TextSelectedBg] = toVec(p.selectionBg);
    colors[ImGuiCol_TextLink] = toVec(p.accentFg);
    colors[ImGuiCol_InputTextCursor] = accent;

    // --- glyphs drawn on a frame surface ------------------------------------
    // This is why the palette carries a separate accentFg: the bright decorative
    // accent is only 3.75:1 on a light frame, fine for an overline but not for a
    // check mark someone actually has to read.
    colors[ImGuiCol_CheckMark] = toVec(p.accentFg);
    colors[ImGuiCol_SliderGrab] = toVec(p.accentFg);
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_CheckboxSelectedBg] = alphaOf(p.accent, checkboxAlpha);

    // --- decoration ---------------------------------------------------------
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = accent;
    colors[ImGuiCol_ResizeGrip] = toVec(p.border);
    colors[ImGuiCol_ResizeGripHovered] = accent;
    colors[ImGuiCol_ResizeGripActive] = accent;
    colors[ImGuiCol_DockingPreview] = alphaOf(p.accent, 0.35F);
    colors[ImGuiCol_NavCursor] = accent;
    colors[ImGuiCol_DragDropTarget] = accent;
    colors[ImGuiCol_DragDropTargetBg] = alphaOf(p.accent, dropTargetAlpha);
    colors[ImGuiCol_PlotLines] = accent;
    colors[ImGuiCol_PlotHistogram] = accent;
    colors[ImGuiCol_PlotLinesHovered] = accent;
    colors[ImGuiCol_PlotHistogramHovered] = accent;

    // --- scrollbars ---------------------------------------------------------
    colors[ImGuiCol_ScrollbarBg] = toVec(p.windowBg);
    colors[ImGuiCol_ScrollbarGrab] = toVec(p.frameBg);
    colors[ImGuiCol_ScrollbarGrabHovered] = toVec(p.frameHover);
    colors[ImGuiCol_ScrollbarGrabActive] = accent;

    // --- tables -------------------------------------------------------------
    colors[ImGuiCol_TableHeaderBg] = toVec(p.header);
    colors[ImGuiCol_TableBorderStrong] = toVec(p.border);
    colors[ImGuiCol_TableBorderLight] = alphaOf(p.border, tableBorderLightAlpha);
    colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] = rowAltWash;

    colors[ImGuiCol_TreeLines] = alphaOf(p.border, treeLineAlpha);
    colors[ImGuiCol_UnsavedMarker] = toVec(p.unsaved);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.35F);
}

} // namespace whitehole::app
