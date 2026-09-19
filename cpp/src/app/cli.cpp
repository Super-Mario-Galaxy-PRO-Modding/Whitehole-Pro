#include "whitehole/app/application.hpp"
#include "whitehole/app/settings.hpp"
#include "whitehole/app/object_db_update.hpp"

#include "whitehole/db/object_db.hpp"
#include "whitehole/io/binary_file.hpp"
#include "whitehole/io/rarc.hpp"
#include "whitehole/io/yaz0.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/field_hashes.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/hash.hpp"
#include "whitehole/smg/object_model.hpp"
#include "whitehole/smg/stage_archive.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/util/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

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
        << "  whitehole-pro-console map add <archive.arc> <object-name> <kind> [--game 1|2] [--in-place]\n"
        << "  whitehole-pro-console map remove <archive.arc> <object> [--game 1|2] [--in-place]\n"
        << "  whitehole-pro-console zone params <game-directory> <galaxy> <zone> <object> [--game 1|2]\n"
        << "  whitehole-pro-console zone set <game-directory> <galaxy> <zone> <object> <field> <value> [--game 1|2]\n"
        << "  whitehole-pro-console zone list <game-directory> <galaxy>\n";
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

// Shared helper declarations (defined further below).
struct CommonFlags {
    int gameType{2};
    bool inPlace{false};
    bool littleEndian{false};
};
void coerceAndSet(smg::BcsvTable& table, smg::BcsvRow& row, const std::string& name,
                  const std::string& text);
std::filesystem::path editedOutputPath(const std::filesystem::path& input);
std::size_t findObjectIndex(const smg::StageArchive& stage, const std::string& name);
db::ObjectDatabase loadObjectDatabase(const std::filesystem::path& executable);
CommonFlags parseCommonFlags(int argc, char** argv, int start);
void printObjectFields(const smg::ObjectModel& model, std::size_t objectIndex);

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
    if (operation == "set") {
        return bcsvSetCommand(argc, argv);
    }
    if (operation == "add") {
        return bcsvAddCommand(argc, argv);
    }
    if (operation == "remove") {
        return bcsvRemoveCommand(argc, argv);
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

namespace {

int objectdbQueryCommand(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("objectdb query requires: <object-name> [--data <directory>]");
    }
    const std::string objectName = argv[3];
    std::filesystem::path dataDir = dataDirectory(argv[0]);
    for (int index = 4; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--data" && index + 1 < argc) {
            dataDir = argv[++index];
        } else if (flag == "--json") {
            g_json = true;
        } else {
            throw std::runtime_error("Unknown objectdb query flag: " + flag);
        }
    }
    const auto jsonPath = dataDir / "objectdb.json";
    const auto cachePath = Settings::defaultConfigPath().parent_path() / "objectdb.cache";
    db::ObjectDatabase database;
    database.load(jsonPath, cachePath);
    if (database.empty()) {
        throw std::runtime_error(
            "No object database found. Run 'whitehole-pro-console objectdb update' first.");
    }
    const auto* info = database.find(objectName);
    if (info == nullptr) {
        throw std::runtime_error("Object not found in database: " + objectName);
    }
    const auto* classInfo = database.classForObject(objectName, 2);
    if (g_json) {
        util::JsonObject json;
        json["command"] = "objectdb query";
        json["name"] = info->name;
        json["internalName"] = info->internalName;
        json["category"] = info->category;
        json["description"] = info->description;
        json["file"] = info->file;
        json["list"] = info->list(2);
        json["class"] = classInfo != nullptr ? classInfo->internalName : "";
        util::JsonArray properties;
        if (classInfo != nullptr) {
            for (const auto& [identifier, prop] : classInfo->properties) {
                util::JsonObject entry;
                entry["id"] = prop.identifier;
                entry["label"] = prop.simpleName;
                entry["type"] = prop.declaredType;
                entry["description"] = prop.description;
                properties.push_back(std::move(entry));
            }
        }
        json["properties"] = std::move(properties);
        std::cout << util::serializeJson(json) << '\n';
        return 0;
    }
    std::cout << "Object:      " << info->name << '\n'
              << "Internal:    " << info->internalName << '\n'
              << "Category:    " << info->category << '\n'
              << "List:        " << info->list(2) << '\n'
              << "Class:       " << (classInfo != nullptr ? classInfo->internalName : "(none)")
              << '\n';
    if (!info->description.empty()) {
        std::cout << "Description: " << info->description << '\n';
    }
    if (classInfo != nullptr) {
        std::cout << "Properties:\n";
        for (const auto& [identifier, prop] : classInfo->properties) {
            std::cout << "  " << std::setw(16) << std::left << prop.identifier
                      << "  " << prop.declaredType << '\n';
        }
    }
    return 0;
}

int zoneListCommand(int argc, char** argv) {
    if (argc != 5) {
        throw std::runtime_error("zone list requires: <game-directory> <galaxy>");
    }
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(argv[4]);
    if (g_json) {
        util::JsonObject json;
        json["command"] = "zone list";
        json["galaxy"] = galaxy.name();
        util::JsonArray zones;
        for (const auto& zone : galaxy.zones()) zones.push_back(zone);
        json["zones"] = std::move(zones);
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << galaxy.name() << " zones:\n";
        for (const auto& zone : galaxy.zones()) {
            std::cout << "  " << zone << '\n';
        }
    }
    return 0;
}

int zoneParamsCommand(int argc, char** argv) {
    if (argc < 7) {
        throw std::runtime_error(
            "zone params requires: <game-directory> <galaxy> <zone> <object> [--game 1|2]");
    }
    const auto flags = parseCommonFlags(argc, argv, 7);
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(argv[4]);
    auto stage = smg::StageArchive::open(game.filesystem(), argv[5], flags.gameType);
    const auto database = loadObjectDatabase(argv[0]);
    smg::ObjectModel model(stage, database, flags.gameType);
    const auto objectIndex = findObjectIndex(stage, argv[6]);
    printObjectFields(model, objectIndex);
    return 0;
}

int zoneSetCommand(int argc, char** argv) {
    if (argc < 9) {
        throw std::runtime_error(
            "zone set requires: <game-directory> <galaxy> <zone> <object> <field> <value> [--game 1|2]");
    }
    const std::string galaxyName = argv[4];
    const std::string zoneName = argv[5];
    const std::string objectName = argv[6];
    const std::string fieldName = argv[7];
    const std::string valueText = argv[8];
    const auto flags = parseCommonFlags(argc, argv, 9);
    smg::GameArchive game(argv[3]);
    const auto galaxy = game.openGalaxy(galaxyName);
    auto stage = smg::StageArchive::open(game.filesystem(), zoneName, flags.gameType);
    const auto database = loadObjectDatabase(argv[0]);
    smg::ObjectModel model(stage, database, flags.gameType);
    const auto objectIndex = findObjectIndex(stage, objectName);

    edit::UndoStack undo;
    const auto* property = database.rawProperty(objectName, fieldName, flags.gameType);
    const auto kind = property != nullptr ? property->kind : db::PropertyKind::Unknown;
    bool ok = false;
    switch (kind) {
    case db::PropertyKind::Float:
        ok = model.setFloat(objectIndex, fieldName, std::stof(valueText), undo);
        break;
    case db::PropertyKind::Boolean:
        ok = model.setBool(objectIndex, fieldName,
                           valueText == "1" || valueText == "true", undo);
        break;
    case db::PropertyKind::Text:
    case db::PropertyKind::TextList:
    case db::PropertyKind::ObjectName:
        ok = model.setString(objectIndex, fieldName, valueText, undo);
        break;
    default:
        ok = model.setInt(objectIndex, fieldName, std::stol(valueText), undo);
        break;
    }
    if (!ok) {
        throw std::runtime_error("Failed to set field '" + fieldName + "' on " + objectName);
    }
    stage.save();
    if (g_json) {
        util::JsonObject json;
        json["command"] = "zone set";
        json["galaxy"] = galaxyName;
        json["zone"] = zoneName;
        json["object"] = objectName;
        json["field"] = fieldName;
        json["value"] = valueText;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Set " << objectName << "." << fieldName << " = " << valueText
                  << " in " << zoneName << '\n';
    }
    return 0;
}

int mapAddCommand(int argc, char** argv) {
    if (argc < 6) {
        throw std::runtime_error(
            "map add requires: <archive.arc> <object-name> <kind> [--game 1|2] [--in-place]");
    }
    const std::filesystem::path archivePath = argv[3];
    const std::string objectName = argv[4];
    const std::string kind = argv[5];
    const auto flags = parseCommonFlags(argc, argv, 6);
    auto stage = smg::StageArchive::openMapFile(archivePath, flags.gameType);

    std::size_t tableIndex = stage.tables().size();
    for (std::size_t index = 0; index < stage.tables().size(); ++index) {
        if (stage.tables()[index].kind == kind) {
            tableIndex = index;
            break;
        }
    }
    if (tableIndex == stage.tables().size()) {
        throw std::runtime_error("Unknown object kind: " + kind);
    }
    auto& table = stage.tables()[tableIndex].table;
    const auto nameIndex = table.fieldIndex("name");
    if (!nameIndex) {
        throw std::runtime_error("Table for kind '" + kind + "' has no name field");
    }
    std::vector<smg::BcsvValue> values;
    values.reserve(table.fields().size());
    for (const auto& field : table.fields()) {
        values.push_back(smg::defaultValueFor(field.type));
    }
    values[*nameIndex] = objectName;
    const std::size_t rowIndex = table.insertRow(0, values);
    stage.rebuildObjects();
    if (flags.inPlace) {
        stage.save();
    } else {
        stage.saveTo(editedOutputPath(archivePath));
    }
    if (g_json) {
        util::JsonObject json;
        json["command"] = "map add";
        json["archive"] = archivePath.string();
        json["object"] = objectName;
        json["kind"] = kind;
        json["row"] = static_cast<int>(rowIndex);
        json["table"] = stage.tables()[tableIndex].path;
        json["inPlace"] = flags.inPlace;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Added " << objectName << " (" << kind << ") at row " << rowIndex << '\n';
        if (!flags.inPlace) std::cout << "Output: " << editedOutputPath(archivePath).string() << '\n';
    }
    return 0;
}

int mapRemoveCommand(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error(
            "map remove requires: <archive.arc> <object> [--game 1|2] [--in-place]");
    }
    const std::filesystem::path archivePath = argv[3];
    const std::string objectName = argv[4];
    const auto flags = parseCommonFlags(argc, argv, 5);
    auto stage = smg::StageArchive::openMapFile(archivePath, flags.gameType);
    const auto objectIndex = findObjectIndex(stage, objectName);
    // Copy the placement info first: rebuildObjects() below invalidates the
    // objects vector, so references into it must not outlive that call.
    const std::string resolvedName = stage.objects()[objectIndex].name;
    const std::size_t tableIndex = stage.objects()[objectIndex].tableIndex;
    const std::size_t row = stage.objects()[objectIndex].rowIndex;
    auto& table = stage.tables()[tableIndex].table;
    table.removeRow(row);
    stage.rebuildObjects();
    if (flags.inPlace) {
        stage.save();
    } else {
        stage.saveTo(editedOutputPath(archivePath));
    }
    if (g_json) {
        util::JsonObject json;
        json["command"] = "map remove";
        json["archive"] = archivePath.string();
        json["object"] = resolvedName;
        json["row"] = static_cast<int>(row);
        json["inPlace"] = flags.inPlace;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Removed " << resolvedName << '\n';
        if (!flags.inPlace) std::cout << "Output: " << editedOutputPath(archivePath).string() << '\n';
    }
    return 0;
}

// Sets one field through ObjectModel, recording an undo entry (discarded on
// exit; CLI runs are one-shot edits).
int mapSetField(int argc, char** argv) {
    if (argc < 7) {
        throw std::runtime_error(
            "map set requires: <archive.arc> <object> <field> <value> [--game 1|2] [--in-place]");
    }
    const std::filesystem::path archivePath = argv[3];
    const std::string objectName = argv[4];
    const std::string fieldName = argv[5];
    const std::string valueText = argv[6];
    const auto flags = parseCommonFlags(argc, argv, 7);
    auto stage = smg::StageArchive::openMapFile(archivePath, flags.gameType);
    const auto database = loadObjectDatabase(argv[0]);
    smg::ObjectModel model(stage, database, flags.gameType);
    const auto objectIndex = findObjectIndex(stage, objectName);

    edit::UndoStack undo;
    const auto* property = database.rawProperty(objectName, fieldName, flags.gameType);
    const auto kind = property != nullptr ? property->kind : db::PropertyKind::Unknown;
    bool ok = false;
    switch (kind) {
    case db::PropertyKind::Float:
        ok = model.setFloat(objectIndex, fieldName, std::stof(valueText), undo);
        break;
    case db::PropertyKind::Boolean:
        ok = model.setBool(objectIndex, fieldName,
                           valueText == "1" || valueText == "true", undo);
        break;
    case db::PropertyKind::Text:
    case db::PropertyKind::TextList:
    case db::PropertyKind::ObjectName:
        ok = model.setString(objectIndex, fieldName, valueText, undo);
        break;
    default:
        ok = model.setInt(objectIndex, fieldName, std::stol(valueText), undo);
        break;
    }
    if (!ok) {
        throw std::runtime_error("Failed to set field '" + fieldName + "' on " + objectName +
                                 " (field may be absent from this table row)");
    }
    if (flags.inPlace) {
        stage.save();
    } else {
        stage.saveTo(editedOutputPath(archivePath));
    }
    if (g_json) {
        util::JsonObject json;
        json["command"] = "map set";
        json["archive"] = archivePath.string();
        json["object"] = objectName;
        json["field"] = fieldName;
        json["value"] = valueText;
        json["inPlace"] = flags.inPlace;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Set " << objectName << "." << fieldName << " = " << valueText << '\n';
        if (!flags.inPlace) std::cout << "Output: " << editedOutputPath(archivePath).string() << '\n';
    }
    return 0;
}

int mapSetCommand(int argc, char** argv) {
    return mapSetField(argc, argv);
}

// Finds a placement object by (case-insensitive) name; returns object index.
std::size_t findObjectIndex(const smg::StageArchive& stage, const std::string& name) {
    const auto& objects = stage.objects();
    for (std::size_t index = 0; index < objects.size(); ++index) {
        std::string candidate = objects[index].name;
        std::string needle = name;
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (candidate == needle) return index;
    }
    throw std::runtime_error("Object not found: " + name);
}

db::ObjectDatabase loadObjectDatabase(const std::filesystem::path& executable) {
    db::ObjectDatabase database;
    const auto jsonPath = dataDirectory(executable) / "objectdb.json";
    const auto cachePath =
        Settings::defaultConfigPath().parent_path() / "objectdb.cache";
    database.load(jsonPath, cachePath);
    if (database.empty()) {
        throw std::runtime_error(
            "No object database found. Run 'whitehole-pro-console objectdb update' first.");
    }
    return database;
}

// Parses trailing --game/--in-place/--json options starting at argv[start].
CommonFlags parseCommonFlags(int argc, char** argv, int start) {
    CommonFlags flags;
    for (int index = start; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--game" && index + 1 < argc) {
            flags.gameType = std::stoi(argv[++index]);
            if (flags.gameType != 1 && flags.gameType != 2) {
                throw std::runtime_error("--game must be 1 or 2");
            }
        } else if (arg == "--in-place") {
            flags.inPlace = true;
        } else if (arg == "--json") {
            g_json = true;
        } else if (arg == "--little") {
            flags.littleEndian = true;
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }
    return flags;
}

void printObjectFields(const smg::ObjectModel& model, std::size_t objectIndex) {
    const auto* objectClass = model.objectClass(objectIndex);
    if (g_json) {
        util::JsonObject json;
        json["stage"] = model.stage().stageName();
        json["object"] = model.stage().objects()[objectIndex].name;
        json["class"] = objectClass ? objectClass->internalName : "";
        util::JsonArray fields;
        for (const auto& field : model.fields(objectIndex)) {
            util::JsonObject entry;
            entry["id"] = field.identifier;
            entry["label"] = field.label;
            entry["kind"] = std::string(smg::propertyKindLabel(field.kind));
            entry["present"] = field.present;
            entry["used"] = field.used;
            entry["value"] = smg::toString(field.value);
            fields.push_back(std::move(entry));
        }
        json["fields"] = std::move(fields);
        std::cout << util::serializeJson(json) << '\n';
        return;
    }
    std::cout << model.stage().objects()[objectIndex].name;
    if (objectClass != nullptr) {
        std::cout << "  [" << objectClass->internalName << "]";
    }
    std::cout << '\n';
    for (const auto& field : model.fields(objectIndex)) {
        std::cout << "  " << std::setw(24) << std::left << field.identifier << "  "
                  << std::setw(8) << smg::propertyKindLabel(field.kind)
                  << (field.present ? "  " : "! ")
                  << smg::toString(field.value) << '\n';
    }
}

int mapParamsCommand(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error("map params requires: <archive.arc> <object> [--game 1|2]");
    }
    const auto flags = parseCommonFlags(argc, argv, 5);
    auto stage = smg::StageArchive::openMapFile(argv[3], flags.gameType);
    const auto database = loadObjectDatabase(argv[0]);
    smg::ObjectModel model(stage, database, flags.gameType);
    const auto objectIndex = findObjectIndex(stage, argv[4]);
    printObjectFields(model, objectIndex);
    return 0;
}

int bcsvAddCommand(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("bcsv add requires an input path");
    }
    const std::filesystem::path inputPath = argv[3];
    bool littleEndian = false;
    bool inPlace = false;
    std::vector<std::pair<std::string, std::string>> fieldValues;
    for (int index = 4; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--little") littleEndian = true;
        else if (arg == "--in-place") inPlace = true;
        else if (arg == "--json") g_json = true;
        else if (arg.rfind("--", 0) == 0 && arg.find('=') != std::string::npos) {
            const auto eq = arg.find('=');
            fieldValues.emplace_back(arg.substr(2, eq - 2), arg.substr(eq + 1));
        } else throw std::runtime_error("Unknown bcsv add argument: " + arg);
    }
    const auto endian = littleEndian ? whitehole::io::Endian::little : whitehole::io::Endian::big;
    auto table = smg::BcsvTable::open(inputPath, endian);
    if (table.fields().empty()) {
        throw std::runtime_error("Cannot add a row to a table with no fields");
    }
    const std::size_t newRow = table.addRow();
    for (const auto& [name, value] : fieldValues) {
        coerceAndSet(table, table.rows()[newRow], name, value);
    }
    const auto outputPath = inPlace ? inputPath : editedOutputPath(inputPath);
    whitehole::io::writeFile(outputPath, table.serialize());
    if (g_json) {
        util::JsonObject json;
        json["command"] = "bcsv add";
        json["input"] = inputPath.string();
        util::JsonObject fields;
        for (const auto& [name, value] : fieldValues) fields[name] = value;
        json["fields"] = std::move(fields);
        json["newRow"] = static_cast<int>(newRow);
        json["inPlace"] = inPlace;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Added row " << newRow << " with " << fieldValues.size() << " field(s)\n";
        if (!inPlace) std::cout << "Output: " << outputPath.string() << '\n';
    }
    return 0;
}

int bcsvRemoveCommand(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error("bcsv remove requires: <input> <row> [--little] [--in-place]");
    }
    const std::filesystem::path inputPath = argv[3];
    const std::size_t rowIndex = static_cast<std::size_t>(std::stoul(argv[4]));
    bool littleEndian = false;
    bool inPlace = false;
    for (int index = 5; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--little") littleEndian = true;
        else if (flag == "--in-place") inPlace = true;
        else if (flag == "--json") g_json = true;
        else throw std::runtime_error("Unknown bcsv remove flag: " + flag);
    }
    const auto endian = littleEndian ? whitehole::io::Endian::little : whitehole::io::Endian::big;
    auto table = smg::BcsvTable::open(inputPath, endian);
    if (rowIndex >= table.rows().size()) {
        throw std::runtime_error("Row index out of range: " + std::to_string(rowIndex) +
                                 " (" + std::to_string(table.rows().size()) + " rows)");
    }
    table.removeRow(rowIndex);
    const auto outputPath = inPlace ? inputPath : editedOutputPath(inputPath);
    whitehole::io::writeFile(outputPath, table.serialize());
    if (g_json) {
        util::JsonObject json;
        json["command"] = "bcsv remove";
        json["input"] = inputPath.string();
        json["row"] = static_cast<int>(rowIndex);
        json["rows"] = static_cast<int>(table.rows().size());
        json["inPlace"] = inPlace;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Removed row " << rowIndex << " (" << table.rows().size() << " rows remain)\n";
        if (!inPlace) std::cout << "Output: " << outputPath.string() << '\n';
    }
    return 0;
}

// ---- shared helpers --------------------------------------------------------

// Coerces a text argument to the field's declared type and stores it.
void coerceAndSet(smg::BcsvTable& table, smg::BcsvRow& row, const std::string& name,
                  const std::string& text) {
    const auto index = table.fieldIndex(name);
    if (!index) {
        throw std::runtime_error("Field not found: " + name);
    }
    switch (table.fields()[*index].type) {
    case smg::BcsvType::floatingPoint:
        table.setFloat(row, name, std::stof(text));
        break;
    case smg::BcsvType::integer:
    case smg::BcsvType::integer2:
        table.setInt(row, name, std::stol(text));
        break;
    case smg::BcsvType::shortInteger:
        table.setInt(row, name, static_cast<std::int16_t>(std::stol(text)));
        break;
    case smg::BcsvType::byte:
        table.setInt(row, name, static_cast<std::int8_t>(std::stol(text)));
        break;
    case smg::BcsvType::fixedString:
    case smg::BcsvType::stringOffset:
        table.setString(row, name, text);
        break;
    }
}

std::filesystem::path editedOutputPath(const std::filesystem::path& input) {
    std::filesystem::path output = input;
    output += ".edited";
    return output;
}

int bcsvSetCommand(int argc, char** argv) {
    if (argc < 7) {
        throw std::runtime_error("bcsv set requires: <input> <row> <field> <value> [--little] [--in-place]");
    }
    const std::filesystem::path inputPath = argv[3];
    const std::size_t rowIndex = static_cast<std::size_t>(std::stoul(argv[4]));
    const std::string fieldName = argv[5];
    const std::string valueText = argv[6];
    bool littleEndian = false;
    bool inPlace = false;
    for (int index = 7; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--little") littleEndian = true;
        else if (flag == "--in-place") inPlace = true;
        else if (flag == "--json") g_json = true;
        else throw std::runtime_error("Unknown bcsv set flag: " + flag);
    }
    const auto endian = littleEndian ? whitehole::io::Endian::little : whitehole::io::Endian::big;
    auto table = smg::BcsvTable::open(inputPath, endian);
    if (rowIndex >= table.rows().size()) {
        throw std::runtime_error("Row index out of range: " + std::to_string(rowIndex) +
                                 " (" + std::to_string(table.rows().size()) + " rows)");
    }
    coerceAndSet(table, table.rows()[rowIndex], fieldName, valueText);
    const auto outputPath = inPlace ? inputPath : editedOutputPath(inputPath);
    whitehole::io::writeFile(outputPath, table.serialize());
    if (g_json) {
        util::JsonObject json;
        json["command"] = "bcsv set";
        json["input"] = inputPath.string();
        json["row"] = static_cast<int>(rowIndex);
        json["field"] = fieldName;
        json["value"] = valueText;
        json["inPlace"] = inPlace;
        std::cout << util::serializeJson(json) << '\n';
    } else {
        std::cout << "Set " << fieldName << " = " << valueText << " on row " << rowIndex << '\n';
        if (!inPlace) std::cout << "Output: " << outputPath.string() << '\n';
    }
    return 0;
}

} // namespace

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
