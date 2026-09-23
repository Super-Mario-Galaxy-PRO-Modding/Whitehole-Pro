#include "whitehole/app/application.hpp"
#include "whitehole/app/settings.hpp"
#include "whitehole/app/object_db_update.hpp"

#include "whitehole/db/object_db.hpp"
#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/edit/authoring.hpp"
#include "whitehole/edit/commands.hpp"
#include "whitehole/io/binary_file.hpp"
#include "whitehole/io/rarc.hpp"
#include "whitehole/io/yaz0.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/field_hashes.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/hash.hpp"
#include "whitehole/smg/stage_archive.hpp"
#include "whitehole/edit/undo.hpp"

#include "whitehole/render/model_library.hpp"
#include "whitehole/util/json.hpp"
#include "whitehole/util/text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace whitehole::app {
namespace {

bool g_json = false;
std::filesystem::path executableDirectory(const std::filesystem::path& executable) {
    auto path = executable;
    if (path.has_parent_path()) {
        return path.parent_path();
    }
    return std::filesystem::current_path();
}

void printUsage() {
        std::cout
        << "Whitehole Pro C++ 0.2.0\n"
        << "Native Super Mario Galaxy editor core\n\n"
        << "Usage:\n"
        << "  whitehole-pro-console [--json] <command> [args...]\n\n"
        << "  whitehole-pro-console gui\n"
        << "  whitehole-pro-console game list <game-directory>\n"
        << "  whitehole-pro-console models check <game-directory> <zone>\n"
        << "  whitehole-pro-console galaxy inspect <game-directory> <galaxy>\n"
        << "  whitehole-pro-console zone objects <game-directory> <zone>\n"
        << "  whitehole-pro-console map objects <archive.arc>\n"
        << "  whitehole-pro-console archive list <archive.arc>\n"
        << "  whitehole-pro-console archive extract <archive.arc> <directory>\n"
        << "  whitehole-pro-console archive replace <archive.arc> <entry> <input> <output.arc>\n"
        << "  whitehole-pro-console yaz0 compress <input> <output>\n"
        << "  whitehole-pro-console yaz0 decompress <input> <output>\n"
        << "  whitehole-pro-console bcsv inspect <table.bcsv> [--little]\n"
        << "  whitehole-pro-console bcsv roundtrip <input.bcsv> <output.bcsv> [--little]\n"
        << "  whitehole-pro-console bcsv set <input.bcsv> <row> <field> <value> [--little] [--in-place]\n"
        << "  whitehole-pro-console bcsv add <input.bcsv> [--little] [--in-place] [--field1=value1]...\n"
        << "  whitehole-pro-console bcsv remove <input.bcsv> <row> [--little] [--in-place]\n"
        << "  whitehole-pro-console hash <field-name>\n"
        << "  whitehole-pro-console objectdb check [--data <directory>] [--no-cache]\n"
        << "  whitehole-pro-console objectdb update [--data <directory>]\n"
        << "  whitehole-pro-console objectdb query <object-name> [--data <directory>]\n"
        << "  whitehole-pro-console map params <archive.arc> <object> [--game 1|2]\n"
        << "  whitehole-pro-console map set <archive.arc> <object> <field> <value> [--game 1|2] [--in-place]\n"
        << "  whitehole-pro-console map add <archive.arc> <object-name> <kind> [--game 1|2] [--in-place] [--pos x,y,z]\n"
        << "  whitehole-pro-console map remove <archive.arc> <object> [--game 1|2] [--in-place]\n"
        << "  whitehole-pro-console zone params <game-directory> <galaxy> <zone> <object> [--game 1|2]\n"
        << "  whitehole-pro-console zone set <game-directory> <galaxy> <zone> <object> <field> <value> [--game 1|2]\n"
        << "  whitehole-pro-console zone list <game-directory> <galaxy>\n"
        << "\nOptions:\n"
        << "  --json          Emit JSON on stdout instead of human-readable text\n"
        << "  --in-place      Overwrite the input file instead of writing <input>.edited\n"
        << "  --data <dir>    Override the data directory (objectdb.json, hashlookup.txt)\n"
        << "  --no-cache      Do not use the compiled objectdb cache\n"
        << "  --little        Read/write the BCSV as little-endian\n"
        << "  --game 1|2      Override the object database game type\n"
        << "  --layer <name>  Pick one placement layer when a kind has several\n"
        << "  --all           (map remove) remove every object with that name\n";
}

// Forward declarations for the new Phase 1 sub-commands.
int bcsvSetCommand(int argc, char** argv);
int bcsvAddCommand(int argc, char** argv);
int bcsvRemoveCommand(int argc, char** argv);
int zoneParamsCommand(int argc, char** argv);
int zoneSetCommand(int argc, char** argv);
int zoneListCommand(int argc, char** argv);
int mapParamsCommand(int argc, char** argv);
int mapSetCommand(int argc, char** argv);
int mapAddCommand(int argc, char** argv);
int mapRemoveCommand(int argc, char** argv);
int objectdbQueryCommand(int argc, char** argv);

int archiveCommand(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("archive requires an operation and input path");
    }
    const std::string operation = argv[2];
    auto archive = whitehole::io::RarcArchive::open(argv[3]);

    if (operation == "list") {
        std::cout << "Archive: " << archive.rootName()
                  << " (" << (archive.endian() == whitehole::io::Endian::big ? "big" : "little")
                  << "-endian, " << (archive.wasCompressed() ? "Yaz0 compressed" : "uncompressed") << ")\n";
        for (const auto& entry : archive.entries()) {
            std::cout << (entry.directory ? "d " : "f ") << std::setw(10) << entry.size << "  " << entry.path << '\n';
        }
        return 0;
    }
    if (operation == "extract") {
        if (argc != 5) {
            throw std::runtime_error("archive extract requires a destination directory");
        }
        archive.extractAll(argv[4]);
        std::cout << "Extracted " << archive.entries().size() << " entries to " << argv[4] << '\n';
        return 0;
    }
    if (operation == "replace") {
        if (argc != 7) {
            throw std::runtime_error("archive replace requires an entry, replacement file, and output archive");
        }
        archive.replace(argv[4], whitehole::io::readFile(argv[5]));
        whitehole::io::writeFile(argv[6], archive.serialize(archive.wasCompressed()));
        std::cout << "Replaced " << argv[4] << " and wrote " << argv[6] << '\n';
        return 0;
    }
    throw std::runtime_error("Unknown archive operation: " + operation);
}

int yaz0Command(int argc, char** argv) {
    if (argc != 5) {
        throw std::runtime_error("yaz0 requires an operation, input path, and output path");
    }
    const std::string operation = argv[2];
    const auto input = whitehole::io::readFile(argv[3]);
    if (operation == "compress") {
        whitehole::io::writeFile(argv[4], whitehole::io::yaz0::compress(input));
    } else if (operation == "decompress") {
        whitehole::io::writeFile(argv[4], whitehole::io::yaz0::decompress(input));
    } else {
        throw std::runtime_error("Unknown Yaz0 operation: " + operation);
    }
    std::cout << operation << "ed " << argv[3] << " -> " << argv[4] << '\n';
    return 0;
}

int bcsvCommand(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("bcsv requires an operation and input path");
    }
    const std::string operation = argv[2];
    // The write sub-commands parse their own flags and accept --little anywhere
    // in the tail, so dispatch them before this opens the table.
    if (operation == "set") {
        return bcsvSetCommand(argc, argv);
    }
    if (operation == "add") {
        return bcsvAddCommand(argc, argv);
    }
    if (operation == "remove") {
        return bcsvRemoveCommand(argc, argv);
    }
    const bool littleEndian = std::string(argv[argc - 1]) == "--little";
    const auto endian = littleEndian ? whitehole::io::Endian::little : whitehole::io::Endian::big;
    const auto table = whitehole::smg::BcsvTable::open(argv[3], endian);
    smg::FieldHashes hashes;
    hashes.loadFile(dataDirectory(argv[0]) / "hashlookup.txt");

    if (operation == "inspect") {
        if (argc != 4 && !(argc == 5 && littleEndian)) {
            throw std::runtime_error("bcsv inspect accepts only an optional --little flag");
        }
        std::cout << table.rows().size() << " rows, " << table.fields().size()
                  << " fields, " << table.entrySize() << " bytes per row\n";
        for (const auto& field : table.fields()) {
            std::cout << hashes.nameOf(field.hash) << '\t';
        }
        std::cout << '\n';
        for (const auto& row : table.rows()) {
            for (const auto& value : row.values) {
                std::cout << whitehole::smg::toString(value) << '\t';
            }
            std::cout << '\n';
        }
        return 0;
    }
    if (operation == "roundtrip") {
        const auto expectedArguments = littleEndian ? 6 : 5;
        if (argc != expectedArguments) {
            throw std::runtime_error("bcsv roundtrip requires an output path and optional --little flag");
        }
        whitehole::io::writeFile(argv[4], table.serialize());
        std::cout << "Rewrote " << table.rows().size() << " rows to " << argv[4] << '\n';
        return 0;
    }
    throw std::runtime_error("Unknown BCSV operation: " + operation);
}

int gameCommand(int argc, char** argv) {
    if (argc != 4 || std::string(argv[2]) != "list") {
        throw std::runtime_error("game list requires a game directory");
    }
    smg::GameArchive game(argv[3]);
    if (game.gameType() == 0) {
        throw std::runtime_error("Directory is not an SMG1 or SMG2 workspace");
    }
    std::cout << "SMG" << game.gameType() << " workspace: " << argv[3] << '\n';
    std::cout << "Galaxies (" << game.galaxies().size() << "):\n";
    for (const auto& galaxy : game.galaxies()) {
        std::cout << "  " << galaxy << '\n';
    }
    std::cout << "Zones (" << game.zones().size() << "):\n";
    for (const auto& zone : game.zones()) {
        std::cout << "  " << zone << '\n';
    }
    return 0;
}

int galaxyCommand(int argc, char** argv) {
    if (argc != 5 || std::string(argv[2]) != "inspect") {
        throw std::runtime_error("galaxy inspect requires a game directory and galaxy name");
    }
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(argv[4]);
    std::cout << galaxy.name() << " zones:\n";
    for (const auto& zone : galaxy.zones()) {
        std::cout << "  " << zone << '\n';
    }
    return 0;
}

void printObjects(const smg::StageArchive& stage) {
    std::cout << stage.stageName() << ": " << stage.objects().size() << " objects\n";
    for (const auto& object : stage.objects()) {
        std::cout << "  [" << object.kind << "/" << object.layer << "] " << object.name
                  << "  (" << object.position.x << ", " << object.position.y << ", " << object.position.z << ")\n";
    }
}

int zoneCommand(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("zone requires an operation and arguments");
    }
    const std::string operation = argv[2];
    if (operation == "objects") {
        if (argc != 5) {
            throw std::runtime_error("zone objects requires a game directory and zone name");
        }
        smg::GameArchive game(argv[3]);
        printObjects(smg::StageArchive::open(game.filesystem(), argv[4], game.gameType()));
        return 0;
    }
    if (operation == "list") {
        return zoneListCommand(argc, argv);
    }
    if (operation == "params") {
        return zoneParamsCommand(argc, argv);
    }
    if (operation == "set") {
        return zoneSetCommand(argc, argv);
    }
    throw std::runtime_error("Unknown zone operation '" + operation +
                             "'. Supported: objects, list, params, set.");
}
// Diagnoses why the editor viewport shows placeholders instead of game models:
// runs the exact ModelLibrary pipeline (ObjectData listing, substitution,
// archive open, BMD/BDL entry lookup, parse, mesh conversion) for every distinct
// object name in a zone and reports the stage where each name failed.
int modelsCommand(int argc, char** argv) {
    if (argc != 5) {
        throw std::runtime_error("models check requires a game directory and a zone name");
    }
    smg::GameArchive game(argv[3]);
    if (game.gameType() == 0) {
        throw std::runtime_error("That folder is not an SMG1/SMG2 workspace: " + std::string(argv[3]));
    }
    const auto zones = game.zones();
    const auto zonePosition = std::find_if(zones.begin(), zones.end(), [&](const std::string& zone) {
        return util::equalIgnoreCase(zone, argv[4]);
    });
    if (zonePosition == zones.end()) {
        throw std::runtime_error("Zone does not exist in this workspace: " + std::string(argv[4]));
    }
    const auto stage = smg::StageArchive::open(game.filesystem(), *zonePosition, game.gameType());

    // Distinct object names in placement order, mirroring ViewportScene::rebuild.
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (const auto& object : stage.objects()) {
        if (seen.insert(object.name).second) {
            names.push_back(object.name);
        }
    }

    render::ModelLibrary library;
    // `models check` must exercise the exact same resolution pipeline as the
    // editor, so the substitution table (data/modelsubstitutions.json) is
    // applied here too. Without it the report claimed "no ObjectData archive
    // matches" for names the GUI resolves fine (LuigiIntrusively -> LuigiNPC,
    // TimerCoinBlock -> CoinBlock, ...), which made the diagnostic contradict
    // the thing it was diagnosing.
    db::ModelSubstitutions substitutions;
    // DataHolderBase resolves "<root>/data/modelsubstitutions.json", so the
    // root is the directory that *contains* data/, not data/ itself.
    substitutions.setBaseGameRoot(dataDirectory(argv[0]).parent_path());
    substitutions.initBaseGame();
    substitutions.initProject(game.filesystem());
    substitutions.load();
    library.setSubstitutions(&substitutions);
    library.bind(&game.filesystem());

    struct CheckResult {
        render::ModelProbe probe;
        std::size_t objects{0};
    };
    std::vector<CheckResult> results;
    results.reserve(names.size());
    std::size_t shown = 0;
    for (const auto& name : names) {
        CheckResult result{library.probe(name), 0};
        for (const auto& object : stage.objects()) {
            if (object.name == name) {
                ++result.objects;
            }
        }
        if (result.probe.usable()) {
            ++shown;
        }
        results.push_back(std::move(result));
    }

    if (g_json) {
        util::JsonArray items;
        for (const auto& result : results) {
            util::JsonObject item;
            item["object"] = util::JsonValue(result.probe.objectName);
            item["instances"] = util::JsonValue(static_cast<std::int64_t>(result.objects));
            item["archive"] = util::JsonValue(result.probe.archiveName);
            item["archiveFound"] = util::JsonValue(result.probe.archiveFound);
            item["modelPath"] = util::JsonValue(result.probe.modelPath);
            item["modelFound"] = util::JsonValue(result.probe.modelFound);
            item["parsed"] = util::JsonValue(result.probe.parsed);
            item["sceneNodes"] = util::JsonValue(static_cast<std::int64_t>(result.probe.sceneNodes));
            item["batches"] = util::JsonValue(static_cast<std::int64_t>(result.probe.batches));
            item["packets"] = util::JsonValue(static_cast<std::int64_t>(result.probe.packets));
            item["triangles"] = util::JsonValue(static_cast<std::int64_t>(result.probe.triangles));
            item["skippedPrimitives"] =
                util::JsonValue(static_cast<std::int64_t>(result.probe.skippedPrimitives));
            item["droppedEmptyMatrixTable"] =
                util::JsonValue(static_cast<std::int64_t>(result.probe.droppedEmptyMatrixTable));
            item["droppedBadMatrixIndex"] =
                util::JsonValue(static_cast<std::int64_t>(result.probe.droppedBadMatrixIndex));
            item["error"] = util::JsonValue(result.probe.error);
            items.emplace_back(util::JsonValue(std::move(item)));
        }
        util::JsonObject root;
        root["zone"] = util::JsonValue(*zonePosition);
        root["objects"] = util::JsonValue(static_cast<std::int64_t>(stage.objects().size()));
        root["distinct"] = util::JsonValue(static_cast<std::int64_t>(names.size()));
        root["modelsShown"] = util::JsonValue(static_cast<std::int64_t>(shown));
        root["checks"] = util::JsonValue(std::move(items));
        std::cout << util::serializeJson(util::JsonValue(std::move(root))) << "\n";
        return 0;
    }

    std::cout << "Zone " << *zonePosition << ": " << stage.objects().size() << " objects, "
              << names.size() << " distinct names, " << shown << " usable game models\n\n";
    for (const auto& result : results) {
        const auto& probe = result.probe;
        std::cout << probe.objectName << " x" << result.objects << ": ";
        if (probe.usable()) {
            std::cout << "OK " << probe.archiveName << " -> " << probe.modelPath << " ("
                      << probe.triangles << " tris";
            if (probe.skippedPrimitives != 0) {
                std::cout << ", " << probe.skippedPrimitives << " primitives skipped";
            }
            std::cout << ")";
        } else {
            std::cout << (probe.error.empty() ? "parsed but produced no geometry" : probe.error);
            if (probe.archiveFound && probe.parsed) {
                std::cout << " (packets=" << probe.packets
                          << ", emptyMatrixTable=" << probe.droppedEmptyMatrixTable
                          << ", badMatrixIndex=" << probe.droppedBadMatrixIndex << ")";
            }
        }
        std::cout << "\n";
    }
    return 0;
}

int mapCommand(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("map requires an operation and an archive path");
    }
    const std::string operation = argv[2];
    if (operation == "objects") {
        if (argc != 4) {
            throw std::runtime_error("map objects requires an archive path");
        }
        printObjects(smg::StageArchive::openMapFile(argv[3]));
        return 0;
    }
    if (operation == "params") {
        return mapParamsCommand(argc, argv);
    }
    if (operation == "set") {
        return mapSetCommand(argc, argv);
    }
    if (operation == "add") {
        return mapAddCommand(argc, argv);
    }
    if (operation == "remove") {
        return mapRemoveCommand(argc, argv);
    }
    throw std::runtime_error("Unknown map operation '" + operation +
                             "'. Supported: objects, params, set, add, remove.");
}

int objectDbCommand(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("objectdb supports: check, update");
    }
        const std::string operation = argv[2];
    if (operation == "query") {
        return objectdbQueryCommand(argc, argv);
    }
    if (operation != "check" && operation != "update") {
        throw std::runtime_error("unknown objectdb operation: " + operation);
    }
    std::filesystem::path dataDir = dataDirectory(argv[0]);
    bool useCache = true;
    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--no-cache") {
            useCache = false;
        } else if (flag == "--data" && index + 1 < argc) {
            dataDir = argv[++index];
        }
    }
    const std::filesystem::path jsonPath = dataDir / "objectdb.json";

    if (operation == "update") {
        std::cout << "Downloading " << kObjectDatabaseUrl << '\n';
        const std::string failure = downloadObjectDatabase(jsonPath);
        if (!failure.empty()) {
            std::cerr << "Download failed: " << failure << '\n';
            return 1;
        }
        std::cout << "Saved " << jsonPath.string() << '\n'
                  << "Run 'whitehole-pro-console objectdb check' to verify it.\n";
        return 0;
    }

    const std::filesystem::path cachePath =
        useCache ? Settings::defaultConfigPath().parent_path() / "objectdb.cache"
                 : std::filesystem::path{};

    const auto started = std::chrono::steady_clock::now();
    db::ObjectDatabase database;
    database.load(jsonPath, cachePath);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    if (database.empty()) {
        std::cout << "No object database found at " << jsonPath.string() << '\n'
                  << "Run 'whitehole-pro-console objectdb update' to download the community database.\n";
        return 1;
    }

    std::size_t withClass = 0;
    std::size_t withParameters = 0;
    for (const auto& name : database.names()) {
        const auto* info = database.classForObject(name, 2);
        if (info == nullptr) continue;
        ++withClass;
        if (!info->properties.empty()) ++withParameters;
    }

    std::cout << "Object database: " << jsonPath.string() << '\n'
              << "  objects:       " << database.size() << '\n'
              << "  classes:       " << database.classCount() << '\n'
              << "  categories:    " << database.categoryCount() << '\n'
              << "  timestamp:     " << database.timestamp() << '\n'
              << "  loaded from:   "
              << (database.cacheLoaded() ? "compiled cache" : "objectdb.json") << '\n'
              << "  load time:     " << elapsed << " ms\n"
              << "  SMG2 coverage: " << withClass << " objects resolve a class, "
              << withParameters << " expose parameters\n";
    return 0;
}

} // namespace

// ---- Phase 1 sub-command implementations ----------------------------------
// Each command owns its flags, prints human text by default and JSON when the
// global --json flag is set, and writes to "<input>.edited" unless --in-place
// is passed, so a scripted edit never destroys the source file by accident.

namespace {

// Shared option tail for the Phase 1 sub-commands: --game, --in-place, --all,
// --layer and --json. Unknown flags are rejected so a typo cannot silently
// change which file an edit lands in.
struct EditOptions {
    int gameType{0}; // 0 means "use the workspace type" (zone commands)
    bool inPlace{false};
    bool all{false};
    std::optional<std::string> layer;
    std::optional<math::Vec3f> position; // spawn point for map add (--pos x,y,z)
};

int parseGameType(std::string_view value) {
    if (value == "1") {
        return 1;
    }
    if (value == "2") {
        return 2;
    }
    throw std::runtime_error("--game expects 1 (SMG1) or 2 (SMG2), got: " + std::string(value));
}

// "x,y,z" -> a world position for map add. Commas are required so a typo in the
// argument cannot silently become a different coordinate.
math::Vec3f parseVec3(std::string_view text) {
    std::array<float, 3> parts{};
    std::size_t component = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',') {
            if (component >= parts.size()) {
                throw std::runtime_error("--pos takes exactly three values (x,y,z), got: " +
                                         std::string(text));
            }
            parts[component++] =
                std::stof(std::string(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (component != parts.size()) {
        throw std::runtime_error("--pos takes exactly three values (x,y,z), got: " +
                                 std::string(text));
    }
    return {parts[0], parts[1], parts[2]};
}

EditOptions parseEditOptions(int argc, char** argv, int start, int defaultGameType) {
    EditOptions options;
    options.gameType = defaultGameType;
    for (int index = start; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--in-place") {
            options.inPlace = true;
        } else if (flag == "--all") {
            options.all = true;
        } else if (flag == "--json") {
            g_json = true;
        } else if (flag == "--game") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--game needs a value (1 or 2)");
            }
            options.gameType = parseGameType(argv[++index]);
        } else if (flag.rfind("--game=", 0) == 0) {
            options.gameType = parseGameType(flag.substr(7));
        } else if (flag == "--layer") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--layer needs a layer name");
            }
            options.layer = argv[++index];
        } else if (flag.rfind("--layer=", 0) == 0) {
            options.layer = flag.substr(8);
        } else if (flag == "--pos") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--pos needs a position (x,y,z)");
            }
            options.position = parseVec3(argv[++index]);
        } else if (flag.rfind("--pos=", 0) == 0) {
            options.position = parseVec3(flag.substr(6));
        } else {
            throw std::runtime_error("Unknown flag: " + flag);
        }
    }
    return options;
}

// Non-in-place edits write "<input>.edited" so the source archive survives.
std::filesystem::path editedPathFor(const std::filesystem::path& input) {
    auto path = input;
    path += ".edited";
    return path;
}

// Object lookup: exact name first, then case-insensitive.
std::size_t requireObject(const smg::StageArchive& stage, std::string_view name) {
    for (std::size_t index = 0; index < stage.objects().size(); ++index) {
        if (stage.objects()[index].name == name) {
            return index;
        }
    }
    for (std::size_t index = 0; index < stage.objects().size(); ++index) {
        if (util::equalIgnoreCase(stage.objects()[index].name, name)) {
            return index;
        }
    }
    throw std::runtime_error("No object named '" + std::string(name) + "' in " + stage.stageName());
}

// BCSV fields are hash-addressed, so a miss lists the fields the row really has
// instead of leaving the caller guessing.
std::size_t requireField(const smg::BcsvTable& table, const smg::FieldHashes& hashes,
                         std::string_view field) {
    const auto index = table.fieldIndex(field);
    if (index) {
        return *index;
    }
    std::string available;
    for (const auto& candidate : table.fields()) {
        if (!available.empty()) {
            available += ", ";
        }
        available += hashes.nameOf(candidate.hash);
    }
    throw std::runtime_error("Unknown field '" + std::string(field) + "'. Available: " + available);
}

// Writes one text value according to the field's declared BCSV type.
void assignFieldValue(smg::BcsvTable& table, smg::BcsvRow& row, const smg::BcsvField& field,
                      std::string_view fieldName, const std::string& text) {
    try {
        switch (field.type) {
        case smg::BcsvType::floatingPoint:
            table.setFloat(row, fieldName, std::stof(text));
            break;
        case smg::BcsvType::fixedString:
        case smg::BcsvType::stringOffset:
            table.setString(row, fieldName, text);
            break;
        case smg::BcsvType::byte:
        case smg::BcsvType::shortInteger:
        case smg::BcsvType::integer:
        case smg::BcsvType::integer2:
            table.setInt(row, fieldName, static_cast<std::int32_t>(std::stol(text)));
            break;
        }
    } catch (const std::exception&) {
        throw std::runtime_error("Cannot read '" + text + "' as a value for field '" +
                                 std::string(fieldName) + "'");
    }
}

smg::FieldHashes loadFieldHashes(const std::filesystem::path& executable) {
    smg::FieldHashes hashes;
    hashes.loadFile(dataDirectory(executable) / "hashlookup.txt");
    return hashes;
}

// Optional community database: params/query output gains labels and
// descriptions when data/objectdb.json is present, and works without it.
std::unique_ptr<db::ObjectDatabase> openObjectDatabase(const std::filesystem::path& executable,
                                                       bool useCache) {
    auto database = std::make_unique<db::ObjectDatabase>();
    const auto path = dataDirectory(executable) / "objectdb.json";
    if (!std::filesystem::exists(path)) {
        return database;
    }
    const auto cachePath = useCache ? Settings::defaultConfigPath().parent_path() / "objectdb.cache"
                                    : std::filesystem::path{};
    database->load(path, cachePath);
    return database;
}

// Prints every BCSV field of one placement object. Database metadata is layered
// on top when available: the raw field list alone is already exact, so the
// command stays useful without objectdb.json.
void printObjectParams(const smg::StageArchive& stage, std::size_t objectIndex,
                       const smg::FieldHashes& hashes, const db::ObjectDatabase* database,
                       int gameType, const char* commandName) {
    const auto& object = stage.objects()[objectIndex];
    const auto& table = stage.tables()[object.tableIndex].table;
    const auto& row = table.rows()[object.rowIndex];
    const bool haveDatabase = database != nullptr && !database->empty();

    if (g_json) {
        const auto vectorJson = [](const math::Vec3f& value) {
            util::JsonArray array;
            array.emplace_back(static_cast<double>(value.x));
            array.emplace_back(static_cast<double>(value.y));
            array.emplace_back(static_cast<double>(value.z));
            return util::JsonValue(std::move(array));
        };
        util::JsonArray fields;
        for (const auto& field : table.fields()) {
            const std::string name = hashes.nameOf(field.hash);
            const auto* value = table.rawValue(row, field.hash);
            util::JsonObject entry;
            entry["name"] = name;
            entry["value"] = value != nullptr ? smg::toString(*value) : std::string{};
            if (haveDatabase) {
                entry["label"] = database->propertyLabel(object.name, name, gameType);
                entry["description"] = database->propertyDescription(object.name, name, gameType);
            }
            fields.emplace_back(std::move(entry));
        }
        util::JsonObject root;
        root["command"] = commandName;
        root["object"] = object.name;
        root["kind"] = object.kind;
        root["layer"] = object.layer;
        root["position"] = vectorJson(object.position);
        root["rotation"] = vectorJson(object.rotation);
        root["scale"] = vectorJson(object.scale);
        root["fields"] = std::move(fields);
        std::cout << util::serializeJson(root) << '\n';
        return;
    }

    std::cout << object.name << "  [" << object.kind << '/' << object.layer << "]\n"
              << "  position: " << object.position.x << ", " << object.position.y << ", "
              << object.position.z << '\n';
    for (const auto& field : table.fields()) {
        const std::string name = hashes.nameOf(field.hash);
        const auto* value = table.rawValue(row, field.hash);
        std::cout << "  " << name << " = "
                  << (value != nullptr ? smg::toString(*value) : std::string("?"));
        if (haveDatabase) {
            const auto description = database->propertyDescription(object.name, name, gameType);
            if (!description.empty()) {
                std::cout << "  // " << description;
            }
        }
        std::cout << '\n';
    }
}

// ---- bcsv set / add / remove ----------------------------------------------

struct BcsvWriteOptions {
    bool littleEndian{false};
    bool inPlace{false};
};

std::size_t parseRowIndex(std::string_view text) {
    try {
        const auto value = std::stoll(std::string(text));
        if (value < 0) {
            throw std::runtime_error("Row index cannot be negative: " + std::string(text));
        }
        return static_cast<std::size_t>(value);
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("Cannot read '" + std::string(text) + "' as a row index");
    } catch (const std::out_of_range&) {
        throw std::runtime_error("Row index is too large: " + std::string(text));
    }
}

BcsvWriteOptions parseBcsvWriteOptions(int argc, char** argv, int start) {
    BcsvWriteOptions options;
    for (int index = start; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--little") {
            options.littleEndian = true;
        } else if (flag == "--in-place") {
            options.inPlace = true;
        } else if (flag == "--json") {
            g_json = true;
        } else {
            throw std::runtime_error("Unknown flag: " + flag);
        }
    }
    return options;
}

void reportBcsvOutput(const std::filesystem::path& inputPath, const std::filesystem::path& outPath,
                      bool inPlace) {
    std::cout << "Input:  " << inputPath.string() << '\n';
    if (!inPlace) {
        std::cout << "Output: " << outPath.string() << '\n';
    }
}

int bcsvSetCommand(int argc, char** argv) {
    if (argc < 7) {
        throw std::runtime_error(
            "bcsv set requires: <input.bcsv> <row> <field> <value> [--little] [--in-place]");
    }
    const std::filesystem::path inputPath = argv[3];
    const std::size_t rowIndex = parseRowIndex(argv[4]);
    const std::string fieldName = argv[5];
    const std::string valueText = argv[6];
    const auto options = parseBcsvWriteOptions(argc, argv, 7);
    const auto endian = options.littleEndian ? io::Endian::little : io::Endian::big;
    auto table = smg::BcsvTable::open(inputPath, endian);
    if (rowIndex >= table.rows().size()) {
        throw std::runtime_error("Row " + std::to_string(rowIndex) + " is out of range (" +
                                 std::to_string(table.rows().size()) + " rows)");
    }
    const auto fieldIndex = requireField(table, loadFieldHashes(argv[0]), fieldName);
    const smg::BcsvField field = table.fields()[fieldIndex];
    assignFieldValue(table, table.rows()[rowIndex], field, fieldName, valueText);
    const auto outPath = options.inPlace ? inputPath : editedPathFor(inputPath);
    io::writeFile(outPath, table.serialize());

    if (g_json) {
        util::JsonObject root;
        root["command"] = "bcsv set";
        root["input"] = inputPath.string();
        root["row"] = static_cast<int>(rowIndex);
        root["field"] = fieldName;
        root["value"] = valueText;
        root["rows"] = static_cast<int>(table.rows().size());
        root["inPlace"] = options.inPlace;
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << "Set " << fieldName << " = " << valueText << " on row " << rowIndex << " of "
              << inputPath.string() << '\n';
    reportBcsvOutput(inputPath, outPath, options.inPlace);
    return 0;
}

int bcsvAddCommand(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error(
            "bcsv add requires: <input.bcsv> [--little] [--in-place] [--field1=value1]...");
    }
    const std::filesystem::path inputPath = argv[3];
    std::vector<std::pair<std::string, std::string>> fieldValues;
    BcsvWriteOptions options;
    for (int index = 4; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--little") {
            options.littleEndian = true;
        } else if (argument == "--in-place") {
            options.inPlace = true;
        } else if (argument == "--json") {
            g_json = true;
        } else if (argument.rfind("--", 0) == 0) {
            const auto equals = argument.find('=');
            if (equals == std::string::npos || equals <= 2) {
                throw std::runtime_error("Field assignments use --field=value, got: " + argument);
            }
            fieldValues.emplace_back(argument.substr(2, equals - 2), argument.substr(equals + 1));
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }
    const auto endian = options.littleEndian ? io::Endian::little : io::Endian::big;
    auto table = smg::BcsvTable::open(inputPath, endian);
    if (table.fields().empty()) {
        throw std::runtime_error("Cannot add a row to a BCSV table with no fields");
    }
    const auto hashes = loadFieldHashes(argv[0]);
    const std::size_t rowIndex = table.addRow();
    for (const auto& [name, value] : fieldValues) {
        const auto fieldIndex = requireField(table, hashes, name);
        const smg::BcsvField field = table.fields()[fieldIndex];
        assignFieldValue(table, table.rows()[rowIndex], field, name, value);
    }
    const auto outPath = options.inPlace ? inputPath : editedPathFor(inputPath);
    io::writeFile(outPath, table.serialize());

    if (g_json) {
        util::JsonObject root;
        root["command"] = "bcsv add";
        root["input"] = inputPath.string();
        root["row"] = static_cast<int>(rowIndex);
        root["rows"] = static_cast<int>(table.rows().size());
        root["inPlace"] = options.inPlace;
        util::JsonObject fields;
        for (const auto& [name, value] : fieldValues) {
            fields[name] = value;
        }
        root["fields"] = std::move(fields);
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << "Added row " << rowIndex << " with " << fieldValues.size() << " field(s) to "
              << inputPath.string() << '\n';
    reportBcsvOutput(inputPath, outPath, options.inPlace);
    return 0;
}

int bcsvRemoveCommand(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error(
            "bcsv remove requires: <input.bcsv> <row> [--little] [--in-place]");
    }
    const std::filesystem::path inputPath = argv[3];
    const std::size_t rowIndex = parseRowIndex(argv[4]);
    const auto options = parseBcsvWriteOptions(argc, argv, 5);
    const auto endian = options.littleEndian ? io::Endian::little : io::Endian::big;
    auto table = smg::BcsvTable::open(inputPath, endian);
    const auto rowCount = table.rows().size();
    if (!table.removeRow(rowIndex)) {
        throw std::runtime_error("Row " + std::to_string(rowIndex) + " is out of range (" +
                                 std::to_string(rowCount) + " rows)");
    }
    const auto outPath = options.inPlace ? inputPath : editedPathFor(inputPath);
    io::writeFile(outPath, table.serialize());

    if (g_json) {
        util::JsonObject root;
        root["command"] = "bcsv remove";
        root["input"] = inputPath.string();
        root["row"] = static_cast<int>(rowIndex);
        root["removed"] = static_cast<int>(rowCount - table.rows().size());
        root["rows"] = static_cast<int>(table.rows().size());
        root["inPlace"] = options.inPlace;
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << "Removed row " << rowIndex << " from " << inputPath.string() << " ("
              << table.rows().size() << " rows remain)\n";
    reportBcsvOutput(inputPath, outPath, options.inPlace);
    return 0;
}

// ---- map params / set / add / remove ---------------------------------------

int mapParamsCommand(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error("map params requires: <archive.arc> <object> [--game 1|2]");
    }
    const auto options = parseEditOptions(argc, argv, 5, 2);
    const auto stage = smg::StageArchive::openMapFile(argv[3], options.gameType);
    const auto objectIndex = requireObject(stage, argv[4]);
    const auto database = openObjectDatabase(argv[0], true);
    printObjectParams(stage, objectIndex, loadFieldHashes(argv[0]), database.get(), options.gameType,
                      "map params");
    return 0;
}

int mapSetCommand(int argc, char** argv) {
    if (argc < 7) {
        throw std::runtime_error("map set requires: <archive.arc> <object> <field> <value> "
                                 "[--game 1|2] [--in-place]");
    }
    const auto options = parseEditOptions(argc, argv, 7, 2);
    auto stage = smg::StageArchive::openMapFile(argv[3], options.gameType);
    const auto objectIndex = requireObject(stage, argv[4]);
    // Copy the location out of the placement list: rebuildObjects() below
    // re-creates it, which would dangle a reference into the old vector.
    const std::size_t tableIndex = stage.objects()[objectIndex].tableIndex;
    const std::size_t rowIndex = stage.objects()[objectIndex].rowIndex;
    const std::string objectName = stage.objects()[objectIndex].name;
    auto& table = stage.tables()[tableIndex].table;
    const auto fieldIndex = requireField(table, loadFieldHashes(argv[0]), argv[5]);
    const smg::BcsvField field = table.fields()[fieldIndex];
    assignFieldValue(table, table.rows()[rowIndex], field, argv[5], argv[6]);
    // Re-derive the placement list so a change to "name" is not undone on save.
    stage.rebuildObjects();
    const auto outPath = options.inPlace ? stage.sourcePath() : editedPathFor(stage.sourcePath());
    stage.saveTo(outPath);

    if (g_json) {
        util::JsonObject root;
        root["command"] = "map set";
        root["object"] = objectName;
        root["field"] = argv[5];
        root["value"] = argv[6];
        root["output"] = outPath.string();
        root["inPlace"] = options.inPlace;
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << "Set " << objectName << '.' << argv[5] << " = " << argv[6] << '\n'
              << "Output: " << outPath.string() << '\n';
    return 0;
}

int mapAddCommand(int argc, char** argv) {
    if (argc < 6) {
        throw std::runtime_error("map add requires: <archive.arc> <object-name> <kind> "
                                 "[--game 1|2] [--in-place] [--layer <layer>] [--pos x,y,z]");
    }
    const auto options = parseEditOptions(argc, argv, 6, 2);
    const std::string objectName = argv[4];
    auto stage = smg::StageArchive::openMapFile(argv[3], options.gameType);

    std::optional<std::size_t> tableIndex;
    std::string availableLayers;
    for (std::size_t index = 0; index < stage.tables().size(); ++index) {
        const auto& candidate = stage.tables()[index];
        if (!availableLayers.empty()) {
            availableLayers += ", ";
        }
        availableLayers += candidate.kind + " (" + candidate.layer + ")";
        if (!util::equalIgnoreCase(candidate.kind, argv[5])) {
            continue;
        }
        if (options.layer && !util::equalIgnoreCase(candidate.layer, *options.layer)) {
            continue;
        }
        tableIndex = index;
        break;
    }
    if (!tableIndex) {
        throw std::runtime_error("No placement table of kind '" + std::string(argv[5]) + "' in " +
                                 argv[3] + ". Available: " + availableLayers);
    }

    // The row goes through the same authoring path as the desktop editor: a unique
    // l_id, the -1 sentinels for the unset ids and switches, and a 1.0 scale, so
    // a scripted add writes the same archive a click in the editor would.
    edit::NewObject request;
    request.name = objectName;
    request.position = options.position.value_or(math::Vec3f{});
    edit::UndoStack stack;
    const auto created =
        edit::createObject(stage, stack, *tableIndex, request, "Add " + objectName);
    if (!created.has_value()) {
        throw std::runtime_error("Table '" + stage.tables()[*tableIndex].path +
                                 "' has no 'name' field to place the object in");
    }
    const auto outPath = options.inPlace ? stage.sourcePath() : editedPathFor(stage.sourcePath());
    stage.saveTo(outPath);

    if (g_json) {
        util::JsonObject root;
        root["command"] = "map add";
        root["object"] = objectName;
        root["kind"] = stage.tables()[*tableIndex].kind;
        root["layer"] = stage.tables()[*tableIndex].layer;
        root["row"] = static_cast<int>(created->rowIndex);
        root["output"] = outPath.string();
        root["inPlace"] = options.inPlace;
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << "Added " << objectName << " to " << stage.tables()[*tableIndex].kind << '/'
              << stage.tables()[*tableIndex].layer << " as row " << created->rowIndex << '\n'
              << "Output: " << outPath.string() << '\n';
    return 0;
}

int mapRemoveCommand(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error("map remove requires: <archive.arc> <object> "
                                 "[--game 1|2] [--in-place] [--all]");
    }
    const auto options = parseEditOptions(argc, argv, 5, 2);
    const std::string target = argv[4];
    auto stage = smg::StageArchive::openMapFile(argv[3], options.gameType);

    // Snapshot the matches before touching anything: removing a row re-numbers
    // the rows after it, which would invalidate later lookups.
    std::vector<std::pair<std::size_t, std::size_t>> matches;
    for (const auto& object : stage.objects()) {
        if (util::equalIgnoreCase(object.name, target)) {
            matches.emplace_back(object.tableIndex, object.rowIndex);
            if (!options.all) {
                break;
            }
        }
    }
    if (matches.empty()) {
        throw std::runtime_error("No object named '" + target + "' in " + argv[3]);
    }
    // Remove from the highest index down so the remaining row indices stay valid.
    std::sort(matches.begin(), matches.end());
    edit::UndoStack stack;
    std::size_t removed = 0;
    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
        if (edit::removeObject(stage, stack, it->first, it->second, "Remove " + target)) {
            ++removed;
        }
    }
    const auto outPath = options.inPlace ? stage.sourcePath() : editedPathFor(stage.sourcePath());
    stage.saveTo(outPath);

    if (g_json) {
        util::JsonObject root;
        root["command"] = "map remove";
        root["object"] = target;
        root["removed"] = static_cast<int>(removed);
        root["output"] = outPath.string();
        root["inPlace"] = options.inPlace;
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << "Removed " << removed << " object(s) named " << target << '\n'
              << "Output: " << outPath.string() << '\n';
    return 0;
}

// ---- zone list / params / set ----------------------------------------------

// Zone commands read the game workspace, so the default game type comes from
// the workspace itself; --game only overrides it.
int zoneGameType(const smg::GameArchive& game, const EditOptions& options) {
    return options.gameType != 0 ? options.gameType : game.gameType();
}

int zoneListCommand(int argc, char** argv) {
    if (argc != 5) {
        throw std::runtime_error("zone list requires: <game-directory> <galaxy>");
    }
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(argv[4]);
    if (g_json) {
        util::JsonArray zones;
        for (const auto& zone : galaxy.zones()) {
            zones.emplace_back(zone);
        }
        util::JsonObject root;
        root["command"] = "zone list";
        root["galaxy"] = galaxy.name();
        root["zones"] = std::move(zones);
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }
    std::cout << galaxy.name() << " zones (" << galaxy.zones().size() << "):\n";
    for (const auto& zone : galaxy.zones()) {
        std::cout << "  " << zone << '\n';
    }
    return 0;
}

int zoneParamsCommand(int argc, char** argv) {
    if (argc < 7) {
        throw std::runtime_error(
            "zone params requires: <game-directory> <galaxy> <zone> <object> [--game 1|2]");
    }
    const auto options = parseEditOptions(argc, argv, 7, 0);
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(argv[4]);
    const auto stage = galaxy.openZone(argv[5]);
    const auto objectIndex = requireObject(stage, argv[6]);
    const auto database = openObjectDatabase(argv[0], true);
    printObjectParams(stage, objectIndex, loadFieldHashes(argv[0]), database.get(),
                      zoneGameType(game, options), "zone params");
    return 0;
}

int zoneSetCommand(int argc, char** argv) {
    if (argc < 9) {
        throw std::runtime_error("zone set requires: <game-directory> <galaxy> <zone> <object> "
                                 "<field> <value> [--game 1|2]");
    }
    // Flags are validated here even though zone set needs no game type.
    parseEditOptions(argc, argv, 9, 0);
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(argv[4]);
    auto stage = galaxy.openZone(argv[5]);
    const auto objectIndex = requireObject(stage, argv[6]);
    const std::size_t tableIndex = stage.objects()[objectIndex].tableIndex;
    const std::size_t rowIndex = stage.objects()[objectIndex].rowIndex;
    const std::string objectName = stage.objects()[objectIndex].name;
    auto& table = stage.tables()[tableIndex].table;
    const auto fieldIndex = requireField(table, loadFieldHashes(argv[0]), argv[7]);
    const smg::BcsvField field = table.fields()[fieldIndex];
    assignFieldValue(table, table.rows()[rowIndex], field, argv[7], argv[8]);
    stage.rebuildObjects();
    stage.save();
    std::cout << "Set " << objectName << '.' << argv[7] << " = " << argv[8] << " in "
              << stage.sourcePath().string() << '\n';
    return 0;
}

// ---- objectdb query --------------------------------------------------------

int objectdbQueryCommand(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("objectdb query requires an object name");
    }
    std::filesystem::path dataDir = dataDirectory(argv[0]);
    bool useCache = true;
    int gameType = 2;
    for (int index = 4; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--no-cache") {
            useCache = false;
        } else if (flag == "--json") {
            g_json = true;
        } else if (flag == "--data" && index + 1 < argc) {
            dataDir = argv[++index];
        } else if (flag == "--game" && index + 1 < argc) {
            gameType = parseGameType(argv[++index]);
        } else if (flag.rfind("--game=", 0) == 0) {
            gameType = parseGameType(flag.substr(7));
        } else {
            throw std::runtime_error("Unknown flag: " + flag);
        }
    }

    const auto jsonPath = dataDir / "objectdb.json";
    db::ObjectDatabase database;
    const auto cachePath = useCache ? Settings::defaultConfigPath().parent_path() / "objectdb.cache"
                                    : std::filesystem::path{};
    database.load(jsonPath, cachePath);
    if (database.empty()) {
        std::cout << "No object database found at " << jsonPath.string() << '\n'
                  << "Run 'whitehole-pro-console objectdb update' to download the community "
                     "database.\n";
        return 1;
    }

    const db::ObjectInfo* info = database.find(argv[3]);
    if (info == nullptr) {
        const auto matches = database.search(argv[3], gameType);
        if (matches.size() == 1) {
            info = matches.front();
        }
    }
    if (info == nullptr) {
        throw std::runtime_error("Object not found in the database: " + std::string(argv[3]));
    }
    const db::ClassInfo* classInfo = database.classForObject(info->internalName, gameType);

    if (g_json) {
        util::JsonObject root;
        root["command"] = "objectdb query";
        root["internalName"] = info->internalName;
        root["name"] = info->name;
        root["category"] = info->category;
        root["description"] = info->description;
        root["games"] = info->games;
        root["class"] = std::string(info->className(gameType));
        util::JsonArray parameters;
        if (classInfo != nullptr) {
            for (const auto& [identifier, property] : classInfo->properties) {
                if (!property.appliesTo(gameType, info->internalName)) {
                    continue;
                }
                util::JsonObject entry;
                entry["identifier"] = identifier;
                entry["label"] = property.simpleName;
                entry["type"] = std::string(db::toString(property.kind));
                entry["description"] = property.description;
                entry["needed"] = property.needed;
                util::JsonArray values;
                for (const auto& value : property.values) {
                    values.emplace_back(value);
                }
                entry["values"] = std::move(values);
                parameters.emplace_back(std::move(entry));
            }
        }
        root["parameters"] = std::move(parameters);
        std::cout << util::serializeJson(root) << '\n';
        return 0;
    }

    std::cout << (info->name.empty() ? info->internalName : info->name);
    if (!info->name.empty() && info->name != info->internalName) {
        std::cout << "  (" << info->internalName << ')';
    }
    std::cout << '\n';
    if (!info->category.empty()) {
        std::cout << "  category: " << info->category << '\n';
    }
    std::cout << "  games:    "
              << (info->games == 1 ? "SMG1" : info->games == 2 ? "SMG2" : "SMG1 + SMG2") << '\n';
    if (info->unused) {
        std::cout << "  unused in the shipped game\n";
    }
    if (info->leftover) {
        std::cout << "  leftover data\n";
    }
    const auto className = info->className(gameType);
    std::cout << "  class:    "
              << (className.empty() ? std::string("(none)") : std::string(className)) << '\n';
    const auto listName = info->list(gameType);
    if (!listName.empty()) {
        std::cout << "  list:     " << listName << '\n';
    }
    if (!info->description.empty()) {
        std::cout << "  notes:    " << info->description << '\n';
    }
    if (classInfo == nullptr) {
        std::cout << "  (no parameter data for SMG" << gameType << ")\n";
        return 0;
    }
    std::cout << "  parameters (" << classInfo->properties.size() << "):\n";
    for (const auto& [identifier, property] : classInfo->properties) {
        if (!property.appliesTo(gameType, info->internalName)) {
            continue;
        }
        std::cout << "    " << identifier << "  [" << db::toString(property.kind) << ']';
        if (!property.simpleName.empty() && property.simpleName != identifier) {
            std::cout << "  \"" << property.simpleName << '"';
        }
        if (property.needed) {
            std::cout << "  (needed)";
        }
        std::cout << '\n';
        if (!property.description.empty()) {
            std::cout << "        " << property.description << '\n';
        }
        for (const auto& value : property.values) {
            std::cout << "        - " << value << '\n';
        }
    }
    return 0;
}


} // namespace

std::filesystem::path dataDirectory(const std::filesystem::path& executable) {
    const auto current = std::filesystem::current_path() / "data";
    if (std::filesystem::exists(current)) {
        return current;
    }
    const auto beside = executableDirectory(executable) / "data";
    if (std::filesystem::exists(beside)) {
        return beside;
    }
#ifdef WHITEHOLE_SOURCE_DIR
    return std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data";
#else
    return current;
#endif
}

int runCli(int argc, char** argv) {
    try {
        // Parse a global --json flag that can appear before the command. When
        // set, sub-commands emit JSON on stdout instead of human-readable text.
        int argvOffset = 0;
        if (argc > 1 && std::string(argv[1]) == "--json") {
            g_json = true;
            argvOffset = 1;
        }
        int effectiveArgc = argc - argvOffset;
        if (effectiveArgc <= 1) {
#ifdef _WIN32
            return runGui(argv[0]);
#else
            printUsage();
            return 0;
#endif
        }
        // Build a sub-argv: argv[0] is the program name, argv[1] is the command.
        char* subArgv[256];
        subArgv[0] = argv[0];
        for (int index = 1; index < effectiveArgc; ++index) {
            subArgv[index] = argv[index + argvOffset];
        }
        const std::string command = subArgv[1];
        if (command == "--help" || command == "-h") {
            printUsage();
            return 0;
        }
        if (command == "gui") {
            return runGui(subArgv[0]);
        }
        if (command == "archive") {
            return archiveCommand(effectiveArgc, subArgv);
        }
        if (command == "yaz0") {
            return yaz0Command(effectiveArgc, subArgv);
        }
        if (command == "bcsv") {
            return bcsvCommand(effectiveArgc, subArgv);
        }
        if (command == "game") {
            return gameCommand(effectiveArgc, subArgv);
        }
        if (command == "models") {
            return modelsCommand(effectiveArgc, subArgv);
        }
        if (command == "galaxy") {
            return galaxyCommand(effectiveArgc, subArgv);
        }
        if (command == "zone") {
            return zoneCommand(effectiveArgc, subArgv);
        }
        if (command == "map") {
            return mapCommand(effectiveArgc, subArgv);
        }
        if (command == "objectdb") {
            return objectDbCommand(effectiveArgc, subArgv);
        }
        if (command == "hash") {
            if (effectiveArgc != 3) {
                throw std::runtime_error("hash requires one field name");
            }
            std::cout << "JMap:      0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                      << whitehole::smg::jmapHash(subArgv[2]) << '\n'
                      << "SuperFast: 0x" << std::setw(8) << whitehole::smg::superFastHash(subArgv[2]) << '\n';
            return 0;
        }
        throw std::runtime_error("Unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "whitehole-pro-console: " << error.what() << '\n';
        return 1;
    }
}

} // namespace whitehole::app
