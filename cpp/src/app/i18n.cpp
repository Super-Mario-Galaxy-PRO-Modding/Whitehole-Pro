#include "whitehole/app/i18n.hpp"

namespace whitehole::app {

const std::vector<Language>& availableLanguages() {
    // Order matters: it is the order a language picker shows them in.
    static const std::vector<Language> languages = {
        Language::English,
        Language::Spanish,
        Language::French,
        Language::German,
    };
    return languages;
}

const char* languageKey(Language language) noexcept {
    switch (language) {
    case Language::Spanish: return "es";
    case Language::French: return "fr";
    case Language::German: return "de";
    case Language::English:
    default: return "en";
    }
}

Language languageFromKey(std::string_view key) noexcept {
    if (key == "es") {
        return Language::Spanish;
    }
    if (key == "fr") {
        return Language::French;
    }
    if (key == "de") {
        return Language::German;
    }
    // Includes "en", an empty string, and anything a hand-edited file invented.
    // Falling back here is what guarantees the editor is never left without a
    // language it can render.
    return Language::English;
}

const char* languageSelfName(Language language) noexcept {
    switch (language) {
    case Language::Spanish: return "Espa\xC3\xB1ol";       // Español, UTF-8
    case Language::French: return "Fran\xC3\xA7" "ais";   // Français, UTF-8
    case Language::German: return "Deutsch";             // already ASCII
    case Language::English:
    default: return "English";                           // already ASCII
    }
}

bool isKnownLanguage(Language language) noexcept {
    switch (language) {
    case Language::English:
    case Language::Spanish:
    case Language::French:
    case Language::German:
        return true;
    default:
        return false;
    }
}

} // namespace whitehole::app