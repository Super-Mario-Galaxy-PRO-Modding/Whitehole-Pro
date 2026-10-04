#include "whitehole/app/layout.hpp"

namespace whitehole::app {

const std::vector<UiLayout>& availableUiLayouts() {
    static const std::vector<UiLayout> layouts = {UiLayout::Novice, UiLayout::Pro};
    return layouts;
}

const char* uiLayoutKey(UiLayout layout) noexcept {
    switch (layout) {
    case UiLayout::Novice: return "novice";
    case UiLayout::Pro:
    default: return "pro";
    }
}

UiLayout uiLayoutFromKey(std::string_view key) noexcept {
    if (key == "novice") {
        return UiLayout::Novice;
    }
    // Includes "pro", an empty string, and anything hand-invented. Pro is the
    // default on purpose: it is the layout the editor has always used, so a
    // corrupt file degrades to the familiar full set instead of hiding panels.
    return UiLayout::Pro;
}

const char* uiLayoutLabel(UiLayout layout) noexcept {
    switch (layout) {
    case UiLayout::Novice: return "Novice";
    case UiLayout::Pro:
    default: return "Pro";
    }
}

bool isKnownUiLayout(UiLayout layout) noexcept {
    switch (layout) {
    case UiLayout::Novice:
    case UiLayout::Pro:
        return true;
    default:
        return false;
    }
}

} // namespace whitehole::app