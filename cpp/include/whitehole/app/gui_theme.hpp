#pragma once

// Whitehole Pro's visual identity: a single place that styles every Dear ImGui
// surface so the whole editor shares one look. The dark theme is the default;
// the light theme is the same layout with a warm paper palette. Both themes
// use one accent color reserved for selection, focus and the primary button,
// which is what keeps the interface calm.

struct ImGuiStyle;

namespace whitehole::app {

// Applies the Whitehole theme to the current ImGui context. `scale` multiplies
// every size/padding value (derive it from DPI so high-DPI screens stay crisp).
void applyWhiteholeTheme(bool dark, float scale);

// Accent color used by selection/focus highlights, as packed ABGR for ImVec4.
[[nodiscard]] unsigned int accentColor(bool dark);

} // namespace whitehole::app
