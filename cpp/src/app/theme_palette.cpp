#include "whitehole/app/theme_palette.hpp"

#include <algorithm>
#include <cmath>

namespace whitehole::app {
namespace {

// sRGB -> linear, then the WCAG 2.1 luminance weights.
double linearize(double channel) noexcept {
    if (channel <= 0.03928) {
        return channel / 12.92;
    }
    return std::pow((channel + 0.055) / 1.055, 2.4);
}

Rgba rgb(float r, float g, float b) noexcept { return Rgba{r, g, b, 1.0F}; }

// Dark palette. Neutral greys with a faint cool tint; surfaces step darker as
// they go deeper so the hierarchy reads without needing borders everywhere.
const Palette kDark{
    /*windowBg    */ rgb(0.086F, 0.094F, 0.114F),
    /*panelBg     */ rgb(0.114F, 0.125F, 0.153F),
    /*frameBg     */ rgb(0.149F, 0.165F, 0.200F),
    /*frameHover  */ rgb(0.196F, 0.216F, 0.263F),
    /*frameActive */ rgb(0.235F, 0.259F, 0.314F),
    /*header      */ rgb(0.141F, 0.153F, 0.184F),
    /*headerHover */ rgb(0.180F, 0.196F, 0.235F),
    /*headerActive*/ rgb(0.208F, 0.227F, 0.275F),
    /*tabSelected */ rgb(0.176F, 0.192F, 0.231F),
    /*border      */ Rgba{0.216F, 0.235F, 0.286F, 0.627F},
    /*text        */ rgb(0.918F, 0.925F, 0.941F),
    // Three earlier values failed here. 0.545/0.573/0.639 reached only 4.10:1 on
    // a pressed frame and 3.21:1 once disabledAlpha applied; 0.620/0.650/0.710
    // fixed the plain case but still fell to 3.98:1 when dimmed. This clears
    // 4.5:1 on every rest surface both plain (6.51:1) and dimmed (4.76:1), which
    // is stricter than WCAG asks of inactive controls.
    /*textDim     */ rgb(0.690F, 0.720F, 0.780F),
    /*accent      */ rgb(0.310F, 0.760F, 0.970F),
    // The dark accent already holds >=4.97:1 on every frame surface, so glyphs
    // can share it; only the light palette needs a separate, darker ink.
    /*accentFg    */ rgb(0.310F, 0.760F, 0.970F),
    /*unsaved     */ rgb(0.980F, 0.750F, 0.300F),
    /*error       */ rgb(1.000F, 0.520F, 0.500F),
    /*selectionBg */ Rgba{0.310F, 0.760F, 0.970F, 0.30F},
    0.80F,
};

// Light palette. Surfaces step *up* from the window so an active tab and a
// hovered row stay unmistakable even on a dim or over-bright display.
const Palette kLight{
    /*windowBg    */ rgb(0.965F, 0.969F, 0.976F),
    /*panelBg     */ rgb(1.000F, 1.000F, 1.000F),
    /*frameBg     */ rgb(0.906F, 0.914F, 0.929F),
    /*frameHover  */ rgb(0.839F, 0.851F, 0.875F),
    /*frameActive */ rgb(0.784F, 0.800F, 0.831F),
    /*header      */ rgb(0.886F, 0.898F, 0.918F),
    /*headerHover */ rgb(0.831F, 0.847F, 0.875F),
    /*headerActive*/ rgb(0.784F, 0.800F, 0.831F),
    /*tabSelected */ rgb(1.000F, 1.000F, 1.000F),
    /*border      */ rgb(0.769F, 0.784F, 0.816F),
    /*text        */ rgb(0.125F, 0.137F, 0.161F),
    // Was 0.412/0.443/0.502, which sat at 4.04:1 on a frame -- so every input
    // placeholder ("Search objects...") failed AA -- and 3.47:1 once hovered.
    // 0.310/0.340/0.400 fixed the plain case but still fell to 3.76:1 when
    // dimmed, so the palette darkens further: 7.95:1 plain, 4.79:1 dimmed.
    /*textDim     */ rgb(0.230F, 0.260F, 0.320F),
    /*accent      */ rgb(0.050F, 0.320F, 0.660F),
    // Decorative accent above is only 3.75:1 on a frame, so glyph roles get this
    // instead: >=5.8:1 on a frame, >=4.6:1 even on a pressed one.
    /*accentFg    */ rgb(0.060F, 0.320F, 0.630F),
    // The dark marker is 1.66:1 on white -- effectively invisible -- so the
    // light theme uses a deep amber instead.
    /*unsaved     */ rgb(0.620F, 0.360F, 0.000F),
    // The toast red this replaces was hard-coded as 1.0/0.45/0.45, which is
    // 2.65:1 on white -- an error message the user could not actually read.
    /*error       */ rgb(0.650F, 0.120F, 0.120F),
    /*selectionBg */ Rgba{0.110F, 0.460F, 0.850F, 0.25F},
    0.80F,
};

} // namespace

double relativeLuminance(const Rgba& color) noexcept {
    return 0.2126 * linearize(color.r) + 0.7152 * linearize(color.g) +
           0.0722 * linearize(color.b);
}

double contrastRatio(const Rgba& a, const Rgba& b) noexcept {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double hi = std::max(la, lb);
    const double lo = std::min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

Rgba blend(const Rgba& fg, const Rgba& bg, float alpha) noexcept {
    const float a = std::clamp(alpha, 0.0F, 1.0F);
    return Rgba{fg.r * a + bg.r * (1.0F - a),
                fg.g * a + bg.g * (1.0F - a),
                fg.b * a + bg.b * (1.0F - a),
                1.0F};
}

const Palette& themePalette(bool dark) noexcept { return dark ? kDark : kLight; }

Rgba shellBackground(bool dark) noexcept { return themePalette(dark).windowBg; }

} // namespace whitehole::app
