#include "whitehole/smg/stage_templates.hpp"

#include "whitehole/io/binary_file.hpp"
#include "whitehole/smg/scenario_model.hpp"
#include "whitehole/util/json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace whitehole::smg {
namespace {

// The two shipped archives whose tables are a correct bare zone for their game.
// Chosen because they are real game files, not because their names look right:
// the builder copies the SCHEMA from whichever it is given.
constexpr const char* kBareZoneMapSmg2 = "SMG2StandardZoneMap.arc";
constexpr const char* kBareZoneMapSmg1 = "SMG1OneStarGalaxy.arc";

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not open template: " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// A template file's own stem, which is what the UI shows when Name is absent.
std::string stemOf(const std::filesystem::path& path) {
    return path.stem().string();
}

} // namespace

std::optional<std::vector<std::uint8_t>> StageTemplate::mapArchive(
    const std::filesystem::path& templatesDirectory) const {
    if (mapFile.empty()) {
        return std::nullopt;
    }
    const auto path = templatesDirectory / mapFile;
    if (!std::filesystem::is_regular_file(path)) {
        // Named but absent is a BROKEN template, not a bare one: the user asked
        // for content we cannot deliver, and quietly creating an empty zone
        // instead would be the worst possible outcome.
        throw std::runtime_error("Template \"" + name + "\" names a map file that does not " +
                                 "exist: " + path.string());
    }
    return io::readFile(path);
}

std::vector<StageTemplate> loadStageTemplates(const std::filesystem::path& templatesDirectory,
                                              int gameType, bool forGalaxy) {
    std::vector<StageTemplate> result;
    if (!std::filesystem::is_directory(templatesDirectory)) {
        return result;
    }
    for (const auto& entry : std::filesystem::directory_iterator(templatesDirectory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto file = entry.path();
        // parseJson throws on malformed input; the file name goes in the message so
        // a user with a hand-edited template knows WHICH one to fix.
        util::JsonValue root;
        try {
            root = util::parseJson(readTextFile(file));
        } catch (const std::exception& error) {
            throw std::runtime_error("Template " + file.filename().string() +
                                     " is not valid JSON: " + error.what());
        }
        if (!root.isObject()) {
            throw std::runtime_error("Template " + file.filename().string() +
                                     " must contain a JSON object");
        }

        StageTemplate tmpl;
        tmpl.name = root.stringAt("Name", stemOf(file));
        tmpl.mapFile = root.stringAt("MapFile");
        tmpl.scenarioFile = root.stringAt("ScenarioFile");
        tmpl.originalName = root.stringAt("OriginalName");
        tmpl.game = static_cast<int>(root.at("Game").asNumber(0));
        // Java's default is TRUE (CreateGalaxyForm.isApplicableTemplate uses
        // optBoolean("ForGalaxy", true)), so a template with no ForGalaxy is a
        // galaxy template.
        tmpl.forGalaxy = root.at("ForGalaxy").asBool(true);
        for (const auto& layer : root.at("UsedLayers").asArray()) {
            const auto text = layer.asString();
            if (!text.empty()) {
                tmpl.usedLayers.push_back(text);
            }
        }

        // The same filter the Java applies: the game must match, and the kind of
        // thing being created must match too.
        if (tmpl.game != 0 && tmpl.game != gameType) {
            continue;
        }
        if (tmpl.forGalaxy != forGalaxy) {
            continue;
        }
        result.push_back(std::move(tmpl));
    }
    std::sort(result.begin(), result.end(),
              [](const StageTemplate& left, const StageTemplate& right) {
                  return left.name < right.name;
              });
    return result;
}

StageTemplate bareMinimumTemplate(int gameType) {
    StageTemplate tmpl;
    tmpl.name = "Bare minimum (no objects)";
    tmpl.mapFile = gameType == 1 ? kBareZoneMapSmg1 : kBareZoneMapSmg2;
    // A bare zone's Common layer, and nothing else: the whole point is an empty
    // starting point. Java offers this as the first entry too.
    tmpl.usedLayers = {"Common"};
    tmpl.game = gameType;
    tmpl.forGalaxy = false;
    return tmpl;
}

std::vector<std::uint8_t> bareMinimumMapArchive(const std::filesystem::path& templatesDirectory,
                                                int gameType) {
    const auto path = templatesDirectory / (gameType == 1 ? kBareZoneMapSmg1 : kBareZoneMapSmg2);
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(
            "The bundled bare-zone template is missing, so a zone cannot be created: " +
            path.string());
    }
    return io::readFile(path);
}

} // namespace whitehole::smg