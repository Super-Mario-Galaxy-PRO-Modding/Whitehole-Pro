#pragma once

// The creation templates shipped in data/templates/*.json. Java's
// CreateGalaxyForm reads these at runtime to offer "start from this galaxy";
// the C++ port had the files on disk and nothing reading them.
//
// A template is metadata only -- WHICH archive to copy the JMap schemas from,
// which layers it already uses, which game it is for. It is not itself a zone.
// Every field is validated on load, because these are user-editable files sitting
// in the installation directory: a malformed one must produce a clear message,
// never a crash and never a half-created galaxy.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// One template from data/templates.
struct StageTemplate {
    // The file's own stem, used to name it in the UI ("SMG2 Big Galaxy").
    std::string name;
    // The MAP archive the schemas come from, relative to data/templates.
    // Empty means "no template": build from the bundled bare-zone template.
    std::string mapFile;
    // The scenario archive to copy wholesale, if the template ships one.
    std::string scenarioFile;
    // The galaxy whose ScenarioData columns this template defines, when the map
    // was taken from a real galaxy and its per-zone columns must be renamed.
    std::string originalName;
    // Layers the template already uses. These are shown as forced-and-disabled in
    // the layer picker: the archive carries them, so offering to drop one would
    // promise something the create cannot deliver.
    std::vector<std::string> usedLayers;
    // 0 = both games, otherwise the game this template is for.
    int game{0};
    // true for a galaxy template, false for a bare zone.
    bool forGalaxy{true};

    // Reads a template's template archive. Nullopt when the template names no map
    // file, which is the "bare minimum" case.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> mapArchive(
        const std::filesystem::path& templatesDirectory) const;
};

// Every template in `templatesDirectory` that applies to `gameType` and to
// galaxy/zone creation, sorted by name. Throws std::runtime_error naming the
// offending FILE if one is malformed -- a broken template should be skipped
// loudly, not silently half-applied.
[[nodiscard]] std::vector<StageTemplate> loadStageTemplates(
    const std::filesystem::path& templatesDirectory, int gameType, bool forGalaxy);

// The "Bare minimum" template, which is not a file: the bundled standard-zone
// archive, used for a zone with no objects. Always available.
[[nodiscard]] StageTemplate bareMinimumTemplate(int gameType);

// The byte source for a bare zone's schemas, per game: the standard-zone map for
// SMG2 and the one-star galaxy for SMG1, which are the two shipped archives whose
// tables are the game's own.
[[nodiscard]] std::vector<std::uint8_t> bareMinimumMapArchive(
    const std::filesystem::path& templatesDirectory, int gameType);

} // namespace whitehole::smg