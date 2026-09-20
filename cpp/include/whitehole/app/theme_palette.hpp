#pragma once

// The Whitehole Pro colour palette, deliberately free of ImGui.
//
// These values used to live only inside gui_theme.cpp, which links ImGui and is
// compiled only for the desktop build -- so nothing could check them, and the
// dark and light palettes drifted into combinations that failed basic
// legibility (light-mode placeholder text sat at 4.04:1, and text inside
// BeginDisabled() fell to 2.87:1 in dark and 2.34:1 in light).
//
// Keeping the palette here makes it the single source of truth, lets
// gui_theme.cpp stay a thin mapping onto ImGuiStyle, and -- because
// whitehole_core is what the test binary links -- lets the suite assert every
// text-on-surface pair actually clears WCAG AA.

namespace whitehole::app {

// Linear-free sRGB triple with alpha, mirroring how ImGui consumes colours.
struct Rgba {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{1.0F};
};

// WCAG 2.1 relative luminance. Alpha is ignored: only opaque colours are
// contrasted directly, and translucent ones are flattened with blend() first.
[[nodiscard]] double relativeLuminance(const Rgba& color) noexcept;

// WCAG 2.1 contrast ratio: 1.0 for identical colours, 21.0 for black on white.
[[nodiscard]] double contrastRatio(const Rgba& a, const Rgba& b) noexcept;

// `fg` drawn over `bg` at `alpha`. This is what ImGui produces when style.Alpha
// is scaled down, which is exactly what BeginDisabled() does -- so it is how the
// suite reproduces the worst case rather than guessing at it.
[[nodiscard]] Rgba blend(const Rgba& fg, const Rgba& bg, float alpha) noexcept;

// Every surface and ink the theme uses, named by the role it plays rather than
// by the ImGui slot it happens to fill.
struct Palette {
    // Surfaces, deepest first: the hierarchy reads window > panel > frame.
    Rgba windowBg;     // window body, menu bar, unselected tab strip
    Rgba panelBg;      // docked panel body and popups
    Rgba frameBg;      // input, button and combo rest surface
    Rgba frameHover;   // ... hovered
    Rgba frameActive;  // ... pressed
    Rgba header;       // selectable / tree row rest
    Rgba headerHover;  // ... hovered
    Rgba headerActive; // ... pressed
    Rgba tabSelected;  // active tab, lifted clear of the strip
    Rgba border;       // hairlines between stacked panels

    // Ink.
    Rgba text;    // body copy
    Rgba textDim; // TextDisabled, hints, secondary labels

    // Two accents on purpose. `accent` is decorative and may be bright; the
    // glyph roles ImGui drives from an accent -- check marks, links, slider
    // grabs -- sit on frame surfaces, so they get `accentFg`, which is tuned to
    // stay legible there instead of merely looking lively.
    Rgba accent;   // overlines, separator hover, resize grips, dock preview
    Rgba accentFg; // check marks, links, slider grabs

    Rgba unsaved;     // unsaved-changes marker and warning copy
    Rgba error;       // failed actions and error toasts
    Rgba selectionBg; // text selection wash (translucent)

    // The alpha ImGui multiplies into everything inside BeginDisabled().
    // Raised from ImGui's 0.60 default, which is what collapsed disabled text
    // to 2.87:1 in dark mode and 2.34:1 in light mode.
    float disabledAlpha{0.80F};
};

[[nodiscard]] const Palette& themePalette(bool dark) noexcept;

// The colour the windowing layer clears to. The shell paints most of itself,
// but every pixel no ImGui window covers (dockspace spacing, an empty dock
// node, the strip between two panels) would otherwise show whatever the GPU
// back buffer was cleared with. That used to be a hard-coded dark grey, so in
// light mode every such gap read as a black bar. Keeping the value here makes
// it a palette fact instead of a second hard-coded constant, and lets the
// suite assert it always matches the theme the shell is drawn in.
[[nodiscard]] Rgba shellBackground(bool dark) noexcept;

// Minimum acceptable contrast for body copy, per WCAG AA.
inline constexpr double kTextContrastMinimum = 4.5;
// Minimum for non-text glyphs and decoration, per WCAG AA.
inline constexpr double kGlyphContrastMinimum = 3.0;

} // namespace whitehole::app
