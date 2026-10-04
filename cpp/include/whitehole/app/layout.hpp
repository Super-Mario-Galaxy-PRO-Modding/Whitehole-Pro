#pragma once

// Pre-configured panel layouts. Like render::NavPreset, this is a CLOSED SET with
// an explicit fallback rather than a free-form string, so a hand-edited settings
// file can never name a layout the dock builder has no case for.
//
// Why layouts exist at all: the default layout docks eleven panels, which is
// right for someone who works in this editor all day and overwhelming for
// someone opening it for the first time. Novice keeps the three that matter for
// placing objects; Pro is today's full set, unchanged, so nothing an existing
// user relies on moves.

#include <cstdint>
#include <string_view>
#include <vector>

namespace whitehole::app {

enum class UiLayout : std::uint8_t {
    // Objects, Viewport, Properties. Nothing else.
    Novice = 0,
    // Every panel the editor ships today.
    Pro = 1,
};

// Every layout, in menu order.
[[nodiscard]] const std::vector<UiLayout>& availableUiLayouts();

// Machine key for the settings file: "novice" | "pro". Never localised.
[[nodiscard]] const char* uiLayoutKey(UiLayout layout) noexcept;

// The inverse. Anything unrecognised resolves to Pro, because Pro is the layout
// the editor has always used: a corrupt settings file should degrade to the
// familiar full set rather than silently hiding panels someone relies on.
[[nodiscard]] UiLayout uiLayoutFromKey(std::string_view key) noexcept;

// Short label for a layout picker: "Novice", "Pro". English only; a localised
// name belongs with the translation tables, not here.
[[nodiscard]] const char* uiLayoutLabel(UiLayout layout) noexcept;

[[nodiscard]] bool isKnownUiLayout(UiLayout layout) noexcept;

} // namespace whitehole::app