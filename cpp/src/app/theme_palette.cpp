#include "whitehole/app/theme_palette.hpp"

#include "whitehole/io/binary_file.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

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

// ---- custom themes -----------------------------------------------------------
namespace {

// Writes one colour as an [r,g,b,a] array of doubles.
util::JsonValue toJson(const Rgba& color) {
    util::JsonArray values;
    values.push_back(util::JsonValue(static_cast<double>(color.r)));
    values.push_back(util::JsonValue(static_cast<double>(color.g)));
    values.push_back(util::JsonValue(static_cast<double>(color.b)));
    values.push_back(util::JsonValue(static_cast<double>(color.a)));
    return util::JsonValue(std::move(values));
}

// A channel is usable only if it is finite AND within [0, 1]. A NaN or an
// out-of-range value is what a hand-edited file produces by accident (a typo, or
// a colour copied from a 0-255 scale). Clamping silently would give a
// wrong-but-plausible colour; falling back keeps the slot honest.
bool usableChannel(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

// Reads one colour: an [r,g,b] or [r,g,b,a] array. Anything else -- a bare
// number, a string, a short array, a non-finite channel -- returns nullopt so
// the caller keeps the fallback for this ONE slot.
std::optional<Rgba> colourFromJson(const util::JsonValue& value) {
    if (!value.isArray()) {
        return std::nullopt;
    }
    const auto& values = value.asArray();
    if (values.size() < 3 || values.size() > 4) {
        return std::nullopt;
    }
    double channels[4] = {0.0, 0.0, 0.0, 1.0};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!values[index].isNumber() || !usableChannel(values[index].asNumber())) {
            return std::nullopt;
        }
        channels[index] = values[index].asNumber();
    }
    return Rgba{static_cast<float>(channels[0]), static_cast<float>(channels[1]),
                static_cast<float>(channels[2]), static_cast<float>(channels[3])};
}

} // namespace

Palette defaultPalette(bool dark) noexcept { return themePalette(dark); }

util::JsonValue themeToJson(const Palette& palette, std::string name, bool dark) {
    util::JsonObject root;
    root["name"] = util::JsonValue(std::move(name));
    root["dark"] = util::JsonValue(dark);
    root["disabledAlpha"] = util::JsonValue(static_cast<double>(palette.disabledAlpha));

    root["windowBg"] = toJson(palette.windowBg);
    root["panelBg"] = toJson(palette.panelBg);
    root["frameBg"] = toJson(palette.frameBg);
    root["frameHover"] = toJson(palette.frameHover);
    root["frameActive"] = toJson(palette.frameActive);
    root["header"] = toJson(palette.header);
    root["headerHover"] = toJson(palette.headerHover);
    root["headerActive"] = toJson(palette.headerActive);
    root["tabSelected"] = toJson(palette.tabSelected);
    root["border"] = toJson(palette.border);
    root["text"] = toJson(palette.text);
    root["textDim"] = toJson(palette.textDim);
    root["accent"] = toJson(palette.accent);
    root["accentFg"] = toJson(palette.accentFg);
    root["unsaved"] = toJson(palette.unsaved);
    root["error"] = toJson(palette.error);
    root["selectionBg"] = toJson(palette.selectionBg);
    return util::JsonValue(std::move(root));
}

Palette themeFromJson(const util::JsonValue& value, const Palette& fallback) {
    // Start from the fallback, then overwrite slot by slot. That is what makes a
    // PARTIAL theme file work: only the colours it actually names change.
    Palette result = fallback;
    if (!value.isObject()) {
        return result;
    }

    const auto readSlot = [&value, &result](const char* key, Rgba& slot) {
        if (const auto parsed = colourFromJson(value.at(key))) {
            slot = *parsed;
        }
        // No entry, wrong type, or an unusable channel: the slot keeps what it
        // already had. Never an error -- a shared theme with one bad colour
        // should load with that one colour wrong, not refuse to load at all.
    };

    readSlot("windowBg", result.windowBg);
    readSlot("panelBg", result.panelBg);
    readSlot("frameBg", result.frameBg);
    readSlot("frameHover", result.frameHover);
    readSlot("frameActive", result.frameActive);
    readSlot("header", result.header);
    readSlot("headerHover", result.headerHover);
    readSlot("headerActive", result.headerActive);
    readSlot("tabSelected", result.tabSelected);
    readSlot("border", result.border);
    readSlot("text", result.text);
    readSlot("textDim", result.textDim);
    readSlot("accent", result.accent);
    readSlot("accentFg", result.accentFg);
    readSlot("unsaved", result.unsaved);
    readSlot("error", result.error);
    readSlot("selectionBg", result.selectionBg);

    // disabledAlpha gates EVERY disabled control's contrast at once, so a bad
    // value here breaks legibility everywhere rather than in one place. Same
    // per-slot fallback: out-of-range keeps the fallback rather than clamping.
    const auto& alpha = value.at("disabledAlpha");
    if (alpha.isNumber()) {
        const double parsed = alpha.asNumber();
        if (std::isfinite(parsed) && parsed > 0.0 && parsed <= 1.0) {
            result.disabledAlpha = static_cast<float>(parsed);
        }
    }
    return result;
}

bool saveThemeFile(const std::string& path, const Palette& palette, std::string name, bool dark) {
    if (path.empty()) {
        return false;
    }
    const std::string text = util::serializeJson(themeToJson(palette, std::move(name), dark));
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    try {
        // Staged temp + rename, so an interrupted save cannot leave a truncated
        // theme file that fails to parse on the next launch.
        io::writeFile(std::filesystem::path(path), bytes);
    } catch (const std::exception&) {
        return false; // best effort, matching Settings::save()
    }
    return true;
}

bool loadThemeFile(const std::string& path, Palette& outPalette) {
    if (path.empty()) {
        return false;
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes = io::readFile(std::filesystem::path(path));
    } catch (const std::exception&) {
        return false; // missing file, permission denied, a directory: all "no"
    }
    std::string text(bytes.begin(), bytes.end());
    // A UTF-8 BOM from Notepad would make the parser reject the whole file -- the
    // same trap Settings::load() already handles.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    try {
        const auto root = util::parseJson(text);
        if (!root.isObject()) {
            return false;
        }
        // Which built-in palette to fall back on: the file says, and only when it
        // says something usable. A theme saved from light mode keeps light mode's
        // readable defaults for any colour it omits.
        const bool dark = root.at("dark").asBool(true);
        outPalette = themeFromJson(root, themePalette(dark));
    } catch (const std::exception&) {
        return false; // malformed JSON surfaces as "could not read", never throws
    }
    return true;
}

} // namespace whitehole::app
