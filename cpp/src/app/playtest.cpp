#include "whitehole/app/playtest.hpp"

#include "whitehole/io/binary_file.hpp"
#include "whitehole/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace whitehole::app {
namespace {

// The four retail discs of each game. Read off the public disc-id lists (these
// are what Riivolution's <id game="..."> has to match); gameType follows the
// rest of the codebase, where 1 is SMG1 and anything else is SMG2.
struct DiscRegion {
    char region;
    const char* id;
    const char* label;
};

const DiscRegion kSmg1Discs[] = {
    {'E', "RMGE01", "USA (NTSC-U)"},
    {'P', "RMGP01", "Europe (PAL)"},
    {'J', "RMGJ01", "Japan (NTSC-J)"},
    {'K', "RMGK01", "Korea (NTSC-K)"},
};

const DiscRegion kSmg2Discs[] = {
    {'E', "SB4E01", "USA (NTSC-U)"},
    {'P', "SB4P01", "Europe (PAL)"},
    {'J', "SB4J01", "Japan (NTSC-J)"},
    {'K', "SB4K01", "Korea (NTSC-K)"},
};

[[nodiscard]] const DiscRegion* discTable(int gameType) noexcept {
    return gameType == 1 ? kSmg1Discs : kSmg2Discs;
}

[[nodiscard]] constexpr std::size_t discTableSize() noexcept {
    return sizeof(kSmg1Discs) / sizeof(kSmg1Discs[0]);
}

// XML escaping. Paths from a game are ASCII and boring, but the mod folder name
// is typed by a human, and a bare '&' in an attribute value is not a parse
// error -- pugixml stops reading there, so the patch silently loses its name.
// Escaping is five lines; debugging that is an evening.
[[nodiscard]] std::string xmlEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char raw : value) {
        switch (raw) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result.push_back(raw); break;
        }
    }
    return result;
}

// A name that is safe as a folder name on Windows AND as an XML attribute. The
// rejected set is exactly the characters that would either break the path (a
// separator) or be refused by the filesystem, so the message can say which.
void requireUsableName(std::string_view name) {
    if (name.empty()) {
        throw std::runtime_error("The playtest name cannot be empty");
    }
    if (name == "." || name == "..") {
        throw std::runtime_error("The playtest name cannot be \"" + std::string(name) + "\"");
    }
    for (const char raw : name) {
        const unsigned char character = static_cast<unsigned char>(raw);
        if (character < 0x20) {
            throw std::runtime_error("The playtest name cannot contain control characters");
        }
        if (std::string_view("\\/:*?\"<>|").find(raw) != std::string_view::npos) {
            throw std::runtime_error(std::string("The playtest name cannot contain '") + raw +
                                     "' (it has to be usable as a folder name)");
        }
    }
    if (name.back() == ' ' || name.back() == '.') {
        throw std::runtime_error("The playtest name cannot end with a space or a full stop");
    }
}

// Workspace-relative slash path, from either form a caller may hand us: a
// workspace path ("/StageData/Foo/FooMap.arc", which is NOT absolute on Windows
// -- it has no root name) or a real host path inside the workspace.
[[nodiscard]] std::string normalizeRelativePath(const std::filesystem::path& workspaceRoot,
                                                const std::string& input) {
    if (input.empty()) {
        throw std::runtime_error("An exported file needs a path");
    }
    const std::filesystem::path path = util::replaceSlashes(input);
    if (path.is_absolute()) {
        // Inside the workspace only. A file from somewhere else has no disc path
        // to speak of, and guessing one would export a patch that patches
        // nothing. Both sides are lexically_normal()'d so "ws/./StageData" and
        // "ws/StageData" agree.
        const std::filesystem::path root = workspaceRoot.lexically_normal();
        const auto relative = path.lexically_normal().lexically_relative(root);
        if (relative.empty() || relative.generic_string().rfind("..", 0) == 0) {
            throw std::runtime_error("Not inside the workspace: " + input);
        }
        return "/" + util::replaceSlashes(relative.generic_string());
    }
    const std::string trimmed = util::trimSlashes(path.generic_string());
    if (trimmed.empty()) {
        throw std::runtime_error("An exported file needs a path");
    }
    return "/" + trimmed;
}

// Where a planned file lands. ONE function for both the dry run (what
// filesWritten() prints) and the write (what applyPlaytest() does): a second
// spelling of this concatenation is how a preview starts lying. The leading
// slash has to come off before appending -- `modFolder / "/StageData/..."` is a
// root-relative path, so operator/ discards modFolder and yields "\StageData".
[[nodiscard]] std::filesystem::path exportDestination(const std::filesystem::path& modFolder,
                                                      std::string_view relativePath) {
    return modFolder / "files" /
           std::filesystem::path(util::replaceSlashes(util::trimSlashes(relativePath)));
}

} // namespace

std::vector<std::string> knownDiscIds(int gameType) {
    const DiscRegion* table = discTable(gameType);
    std::vector<std::string> result;
    result.reserve(discTableSize());
    for (std::size_t index = 0; index < discTableSize(); ++index) {
        result.emplace_back(table[index].id);
    }
    return result;
}

std::string discIdFor(int gameType, char region) {
    const char wanted = static_cast<char>(std::toupper(static_cast<unsigned char>(region)));
    const DiscRegion* table = discTable(gameType);
    for (std::size_t index = 0; index < discTableSize(); ++index) {
        if (table[index].region == wanted) {
            return table[index].id;
        }
    }
    throw std::runtime_error(std::string("No retail SMG") + (gameType == 1 ? "1" : "2") +
                             " disc has region '" + wanted + "' (expected E, P, J or K)");
}

std::string regionLabel(char region) {
    const char wanted = static_cast<char>(std::toupper(static_cast<unsigned char>(region)));
    for (const DiscRegion* table : {kSmg1Discs, kSmg2Discs}) {
        for (std::size_t index = 0; index < discTableSize(); ++index) {
            if (table[index].region == wanted) {
                return table[index].label;
            }
        }
    }
    return {};
}

char regionOfDiscId(std::string_view discId) noexcept {
    for (const DiscRegion* table : {kSmg1Discs, kSmg2Discs}) {
        for (std::size_t index = 0; index < discTableSize(); ++index) {
            if (discId == table[index].id) {
                return table[index].region;
            }
        }
    }
    return '\0';
}

std::vector<std::string> PlaytestPlan::filesWritten() const {
    std::vector<std::string> result;
    result.reserve(files.size() + 1);
    for (const auto& file : files) {
        result.push_back(
            exportDestination(modFolder, file.relativePath).lexically_normal().string());
    }
    result.push_back(xmlPath.string());
    return result;
}

PlaytestPlan planPlaytest(const std::filesystem::path& sdRoot, std::string_view name,
                          std::string_view gameId, const std::filesystem::path& workspaceRoot,
                          const std::vector<std::string>& relativePaths, std::string_view discRoot) {
    requireUsableName(name);
    if (gameId.empty()) {
        throw std::runtime_error("A playtest patch needs the disc id it is for (e.g. SB4E01)");
    }
    if (relativePaths.empty()) {
        throw std::runtime_error(
            "Nothing to export: save a zone (or edit the galaxy's missions) first, then test");
    }
    if (sdRoot.empty()) {
        throw std::runtime_error("A playtest patch needs Dolphin's Riivolution folder");
    }

    PlaytestPlan plan;
    plan.name = std::string(name);
    plan.gameId = std::string(gameId);
    plan.sdRoot = sdRoot;
    // The XML goes where Dolphin scans for patches: <root>/riivolution/*.xml.
    // The copied files go in a SIBLING folder of that one, because the XML
    // references them root-relative ("/<name>/files"), which is the form
    // Dolphin resolves against the Riivolution root rather than the XML's own
    // folder -- see the header for why that matters.
    plan.xmlPath = sdRoot / "riivolution" / (plan.name + ".xml");
    plan.modFolder = sdRoot / plan.name;

    const std::string root = std::string(util::trimSlashes(discRoot));
    for (const auto& requested : relativePaths) {
        const std::string relative = normalizeRelativePath(workspaceRoot, requested);
        const std::filesystem::path source =
            (workspaceRoot / std::filesystem::path(util::replaceSlashes(util::trimSlashes(relative))))
                .lexically_normal();
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error("Nothing to export at " + relative +
                                     " (save the zone first, then test)");
        }
        PlaytestFile file;
        file.source = source;
        file.relativePath = relative;
        file.discPath = "/" + root + relative;
        file.targetPath = "/" + plan.name + "/files" + relative;
        plan.files.push_back(std::move(file));
    }
    return plan;
}

std::string riivolutionXml(const PlaytestPlan& plan) {
    const std::string name = xmlEscape(plan.name);
    std::string xml;
    xml.reserve(1024);
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<!-- Written by Whitehole Pro (Test in Dolphin). Everything it refers to\n";
    xml += "     lives in a folder named after this patch, beside the riivolution folder\n";
    xml += "     this file is in. Re-exporting overwrites this file. -->\n";
    // NOTE on escaping: `name` is the ESCAPED spelling, used only inside
    // attribute values. Paths are built from the RAW name and escaped exactly
    // once at the point of writing -- escaping twice turns "&" into "&amp;amp;"
    // and the patch then points at a folder that does not exist.
    // version="1" is not decoration: Dolphin's parser returns "no patch" for any
    // other value, and a patch that is silently not a patch is the failure mode
    // this whole module exists to remove.
    xml += "<wiidisc version=\"1\">\n";
    // The disc id is what Riivolution matches the patch to. 6 characters, or it
    // never applies.
    xml += "  <id game=\"" + xmlEscape(plan.gameId) + "\" />\n";
    xml += "  <options>\n";
    xml += "    <section name=\"Whitehole Pro\">\n";
    // default="1" selects the single choice below: Riivolution's option index is
    // 1-based, where 0 means "no choice, option disabled".
    xml += "      <option name=\"" + name + "\" id=\"" + name + "\" default=\"1\">\n";
    xml += "        <choice name=\"Enabled\">\n";
    xml += "          <patch id=\"" + name + "\" />\n";
    xml += "        </choice>\n";
    xml += "      </option>\n";
    xml += "    </section>\n";
    xml += "  </options>\n";
    xml += "  <patch id=\"" + name + "\">\n";
    // A folder patch, not one <file> per entry, for one reason: create="true"
    // adds files the disc does not have, which is the only way a zone created
    // from scratch in Whitehole is reachable. An external path starting with '/'
    // is resolved against the Riivolution root, so this works however the
    // author's Dolphin is configured.
    const std::string external = "/" + plan.name + "/files";
    xml += "    <folder disc=\"" + xmlEscape(std::string(kWorkspaceDiscRoot)) +
           "\" external=\"" + xmlEscape(external) +
           "\" recursive=\"true\" create=\"true\" />\n";
    xml += "  </patch>\n";
    xml += "</wiidisc>\n";
    return xml;
}

void applyPlaytest(const PlaytestPlan& plan) {
    std::error_code error;
    // ---- the files, first, so the XML never points at a half-written tree ----
    for (const auto& file : plan.files) {
        const std::filesystem::path destination =
            exportDestination(plan.modFolder, file.relativePath);
        std::filesystem::create_directories(destination.parent_path(), error);
        // Through writeFile(), like every other writer here: it stages into a
        // per-writer temporary and renames, so an interrupted export cannot
        // leave a truncated zone archive that the game would then load.
        io::writeFile(destination, io::readFile(file.source));
    }

    // ---- drop what an earlier export left behind -----------------------------
    // A folder patch applies EVERY file under the external folder, so a zone
    // exported last week and since removed from the export list would keep being
    // patched in, and the modder would have no way to see why. Only files under
    // this plan's own mod folder are ever touched.
    std::vector<std::filesystem::path> keep;
    keep.reserve(plan.files.size());
    for (const auto& file : plan.files) {
        keep.push_back(exportDestination(plan.modFolder, file.relativePath).lexically_normal());
    }
    const std::filesystem::path filesRoot = plan.modFolder / "files";
    if (std::filesystem::is_directory(filesRoot, error)) {
        std::vector<std::filesystem::path> directories;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(filesRoot)) {
            if (entry.is_directory()) {
                directories.push_back(entry.path());
                continue;
            }
            const auto normalized = entry.path().lexically_normal();
            if (std::ranges::find(keep, normalized) == keep.end()) {
                std::filesystem::remove(normalized, error);
            }
        }
        // Deepest first, so a folder outlives its contents.
        std::ranges::sort(directories, [](const auto& left, const auto& right) {
            return left.native().size() > right.native().size();
        });
        for (const auto& directory : directories) {
            // remove() on a non-empty directory fails, which is exactly right:
            // anything still in it is still part of the export.
            std::filesystem::remove(directory, error);
        }
    }

    // ---- the patch itself, last ---------------------------------------------
    std::filesystem::create_directories(plan.xmlPath.parent_path(), error);
    const std::string text = riivolutionXml(plan);
    io::writeFile(plan.xmlPath, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::filesystem::path defaultDolphinSdRoot() {
    const char* appData = std::getenv("APPDATA");
    if (appData == nullptr || *appData == '\0') {
        return {};
    }
    return std::filesystem::path(appData) / "Dolphin Emulator" / "Load" / "Riivolution";
}

std::filesystem::path detectDolphinSdRoot() {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path standard = defaultDolphinSdRoot();
    if (!standard.empty()) {
        candidates.push_back(standard);
    }
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData != nullptr && *localAppData != '\0') {
        candidates.push_back(std::filesystem::path(localAppData) / "Dolphin Emulator" / "Load" /
                             "Riivolution");
    }
    // A portable Dolphin keeps everything beside the executable, so nothing
    // under %APPDATA% exists to find. detect() returns the standard path in that
    // case (empty-less), and the dialog lets the author point at the real one --
    // which is the same "disabled WITH A STATED REASON" rule the create dialog
    // follows, rather than a button that quietly writes somewhere useless.
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
    }
    return standard;
}

std::string dolphinLaunchSteps() {
    return "In Dolphin:\n"
           "  1. Right-click Super Mario Galaxy (or Galaxy 2) in the game list.\n"
           "  2. Choose \"Start With Riivolution Patches...\".\n"
           "  3. Enable the \"Whitehole Pro\" entry, then press Start.\n"
           "Not listed? Set Dolphin's Riivolution \"Root\" to the folder shown above.";
}

} // namespace whitehole::app



