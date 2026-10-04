#pragma once

// UI language selection. This header holds only the LANGUAGE ENUM and its key
// round-trip -- the translation tables themselves live alongside it, but they are
// pure data and are tested separately, so a language can be added without
// touching a switch that has to stay exhaustive.
//
// WHY AN ENUM AND NOT A STRING: the settings file has to survive being
// hand-edited, and a raw string can hold a value no switch statement handles.
// Language is therefore a closed set with an explicit fallback, matching how
// render::NavPreset handles the control preset (camera_controller.hpp). The
// invariant, copied from there: a hand-edited settings file must never leave the
// editor in a state the code cannot handle.

#include <cstdint>
#include <string_view>
#include <vector>

namespace whitehole::app {

// English is FIRST and is the default for two separate reasons: it is the
// fallback for an unknown or missing key, and it is the language every string in
// the codebase is currently written in, so an untranslated key must render as
// English rather than as an empty label.
enum class Language : std::uint8_t { English = 0, Spanish = 1, French = 2, German = 3 };

// Every language the editor ships, in menu order. Kept as data rather than a
// hard-coded array so a language picker can enumerate them without repeating the
// list, which is how a picker ends up offering one language and switching four.
[[nodiscard]] const std::vector<Language>& availableLanguages();

// Machine key for the settings file and the on-disk locale folder: "en", "es",
// "fr", "de". Never localised -- it is an identifier, not a label.
[[nodiscard]] const char* languageKey(Language language) noexcept;

// The inverse. Anything unrecognised -- including an empty string from a
// hand-edited file -- resolves to English rather than failing, so the editor
// always has a usable language.
[[nodiscard]] Language languageFromKey(std::string_view key) noexcept;

// The language's name IN THAT LANGUAGE: "Español", "Français", "Deutsch".
//
// This is deliberately not a translation of the English name. A language picker
// has to be readable by the person who cannot read the current UI language -- an
// author who set the editor to Deutsch has to be able to find "Deutsch" in the
// list, and "German" would defeat the point.
[[nodiscard]] const char* languageSelfName(Language language) noexcept;

// True when `language` is a value the enum actually defines. Guards against a
// cast or an out-of-range read reaching a switch that has no case for it.
[[nodiscard]] bool isKnownLanguage(Language language) noexcept;

} // namespace whitehole::app