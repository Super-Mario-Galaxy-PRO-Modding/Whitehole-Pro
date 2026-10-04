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

#include "whitehole/util/json.hpp"

#include <string>

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

// ---- custom themes: export / import -----------------------------------------
//
// A theme is just a Palette, and the Palette is deliberately ImGui-free, so
// serialising it needs no GUI code and the whole round trip is unit-testable in
// whitehole_core -- the same property that let the contrast suite exist at all.

// The palette as JSON, with a "name" and a "dark" flag alongside the colours, so
// an exported file is self-describing and can be read back without the caller
// having to remember which mode it was.
[[nodiscard]] util::JsonValue themeToJson(const Palette& palette, std::string name, bool dark);

// A palette from JSON. `fallback` supplies every slot the document does not
// define.
//
// PER-SLOT FALLBACK IS THE WHOLE DESIGN: a theme file is meant to be hand-edited
// and shared, so a typo in one colour, a value out of range, or a non-finite
// number must cost the author that ONE slot -- not the entire theme. Anything
// unusable silently keeps `fallback`'s value, which is why loading can never
// throw and never leaves a half-applied palette.
//
// Non-colour garbage is treated the same way: a missing "name" is not an error,
// it just means "no name". Malformed JSON throws from util::parseJson, which
// callers surface as "could not read theme file" rather than as a crash.
[[nodiscard]] Palette themeFromJson(const util::JsonValue& value, const Palette& fallback);

// Convenience: the built-in palette for `dark`, with no file involved.
[[nodiscard]] Palette defaultPalette(bool dark) noexcept;

// Read/write a theme file. Both return false instead of throwing, matching the
// best-effort contract Settings::save() already uses -- a theme file is not
// worth interrupting an editing session over.
//
// The write goes through io::writeFile's staged temp-and-rename, so an
// interrupted save cannot leave a truncated theme file that fails to parse on
// the next launch.
[[nodiscard]] bool saveThemeFile(const std::string& path, const Palette& palette, std::string name,
                                 bool dark);

// Reads and parses a theme file. False on any failure (missing file, malformed
// JSON, not an object); `outPalette` is only touched on success.
[[nodiscard]] bool loadThemeFile(const std::string& path, Palette& outPalette);

// Minimum acceptable contrast for body copy, per WCAG AA.
inline constexpr double kTextContrastMinimum = 4.5;
// Minimum for non-text glyphs and decoration, per WCAG AA.
inline constexpr double kGlyphContrastMinimum = 3.0;

} // namespace whitehole::app
