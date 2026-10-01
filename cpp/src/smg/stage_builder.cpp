#include "whitehole/smg/stage_builder.hpp"

#include "whitehole/io/rarc.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/scenario_model.hpp"
#include "whitehole/smg/stage_archive.hpp"
#include "whitehole/util/text.hpp"

#include <algorithm>
#include <stdexcept>

namespace whitehole::smg {
namespace {

// SMG1 archives are all-lowercase, everywhere, in every path Java builds. This is
// not cosmetic: the game's own SMG1 archives store /stage/jmp/placement/common,
// and a port that keeps the SMG2 casing produces a zone whose files the game
// does not look for.
std::string forGame(std::string_view value, int gameType) {
    std::string result(value);
    if (gameType == 1) {
        result = util::toLower(result);
    }
    return result;
}

void requireSafeName(std::string_view name, const char* what) {
    if (name.empty()) {
        throw std::runtime_error(std::string("A ") + what + " needs a name");
    }
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos ||
        name == "." || name == "..") {
        throw std::runtime_error("\"" + std::string(name) + "\" is not a usable " + what +
                                 " name (no slashes, no dots on their own)");
    }
}

// The template's table for one (folder, file), as a schema with its rows cleared.
// Nullopt when the template does not carry that file: a template for a bare zone
// legitimately lacks some, and the caller decides whether that is fatal.
std::optional<BcsvTable> schemaFromTemplate(const io::RarcArchive& tmpl, std::string_view folder,
                                            std::string_view file, int gameType) {
    const auto path = "/" + forGame("Stage", gameType) + "/jmp/" + forGame(folder, gameType) +
                      "/Common/" + forGame(file, gameType);
    if (!tmpl.fileExists(path)) {
        return std::nullopt;
    }
    BcsvTable table(tmpl.read(path), tmpl.endian());
    // Schema yes, content no: a template's objects belong to the template.
    table.rows().clear();
    return table;
}

// One spawn point at the origin, in Common/StartInfo. A zone without one cannot
// be started, so this is the single row a created zone keeps. Java does the same
// (StageHelper.createZone adds a StartObj at 0,0,0 for the Common layer).
BcsvTable spawnPointSchema(const io::RarcArchive& tmpl, int gameType) {
    auto table = schemaFromTemplate(tmpl, "Start", "StartInfo", gameType);
    if (!table.has_value()) {
        throw std::runtime_error(
            "The template has no Start/StartInfo, so a spawn point cannot be created");
    }
    const std::size_t row = table->addRow();
    auto& cells = table->rows()[row];
    table->setString(cells, "name", "StartObj");
    table->setFloat(cells, "pos_x", 0.0F);
    table->setFloat(cells, "pos_y", 0.0F);
    table->setFloat(cells, "pos_z", 0.0F);
    return std::move(*table);
}

// Creates every directory LEADING TO `filePath`, one at a time, because
// RarcArchive::createDirectory needs its parent to exist already.
//
// `filePath` is the FULL archive path of the file that is about to be inserted,
// rooted ("/Stage/jmp/Placement/Common/StageObjInfo"). The root and the FILE NAME
// are both stripped here rather than by the caller, because getting either wrong
// is silent and produces a malformed archive:
//
//   * keeping the file name creates a DIRECTORY called StageObjInfo next to the
//     file of the same name, and serialize() then fails on its missing parent;
//   * keeping the root makes createDirectory prepend the root again, giving
//     "/Stage/Stage/jmp" -- which is how this first went wrong.
//
// EVERY level is created, including the file's own parent folder. That last part
// is not optional: the layer loop only creates /Stage/jmp/<folder> before writing
// /Stage/jmp/<folder>/<layer>/<file>, so a folder's per-layer child directory was
// missing and serialize() rejected the file for having no parent.
void makeParentDirectories(io::RarcArchive& archive, std::string_view filePath) {
    // Drop the leading "/" and the root name.
    const std::string root(archive.rootName());
    std::string_view rest = filePath;
    if (!rest.empty() && rest.front() == '/') {
        rest.remove_prefix(1);
    }
    if (rest.size() >= root.size() &&
        whitehole::util::equalIgnoreCase(rest.substr(0, root.size()), root)) {
        rest.remove_prefix(root.size());
    }
    const auto lastSlash = rest.rfind('/');
    if (lastSlash == std::string_view::npos) {
        return; // a file at the archive root needs no directory
    }
    const std::string_view parent = rest.substr(0, lastSlash);
    std::string built = "/" + root;
    std::size_t start = 0;
    while (start <= parent.size()) {
        const auto slash = parent.find('/', start);
        const auto length = slash == std::string_view::npos ? std::string_view::npos
                                                            : slash - start;
        const auto component = parent.substr(start, length);
        if (!component.empty()) {
            built += "/" + std::string(component);
            archive.createDirectory(built);
        }
        if (slash == std::string_view::npos) {
            return;
        }
        start = slash + 1;
    }
}

} // namespace

std::vector<std::string> allStageLayerNames() {
    std::vector<std::string> layers{"Common"};
    for (const auto& name : scenarioLayerNames()) {
        layers.push_back(name);
    }
    return layers;
}

std::vector<std::string> normalizeStageLayers(const std::vector<std::string>& layers) {
    // "Common" is not optional and not optional twice: it owns no bit and every
    // zone has it, so asking for it twice (or not at all) is the same zone.
    std::vector<std::string> wanted{"Common"};
    for (const auto& layer : layers) {
        if (layer == "Common") {
            continue;
        }
        if (scenarioLayerBit(layer) < 0) {
            throw std::runtime_error("\"" + layer + "\" is not a layer (LayerA .. LayerP)");
        }
        if (std::find(wanted.begin(), wanted.end(), layer) == wanted.end()) {
            wanted.push_back(layer);
        }
    }
    return wanted;
}

namespace {

// One file the create will put inside the map archive: where it goes and the
// table that goes there (already reduced to a schema -- rows cleared).
//
// This is the single source of BOTH halves of the split. planStageZone keeps only
// the paths; createStageZone keeps paths and tables. Neither recomputes the list,
// so a dry run cannot describe a different zone from the one that gets written.
struct PlannedZoneFile {
    std::string path;
    BcsvTable table;
};

std::vector<PlannedZoneFile> planZoneFiles(const io::RarcArchive& tmpl,
                                           std::string_view root,
                                           const std::vector<std::string>& layers,
                                           int gameType) {
    std::vector<PlannedZoneFile> files;
    for (const auto& layer : layers) {
        for (const auto& spec : stageLayeredTables()) {
            if (spec.gameType != 0 && spec.gameType != gameType) {
                continue; // SoundInfo / ChildObjInfo are SMG1-only
            }
            auto table = schemaFromTemplate(tmpl, spec.folder, spec.file, gameType);
            if (!table.has_value()) {
                continue; // the template does not carry this file
            }
            // Common's StartInfo is the exception: it is the one file a created
            // zone must not leave empty.
            if (layer == "Common" && spec.file == "StartInfo") {
                table = spawnPointSchema(tmpl, gameType);
            }
            files.push_back({"/" + std::string(root) + "/jmp/" +
                                 forGame(spec.folder, gameType) + "/" +
                                 forGame(layer, gameType) + "/" + forGame(spec.file, gameType),
                             std::move(*table)});
        }
    }
    // A zone with no path table loads its rails as an empty list either way, but
    // the game's own zones all have one, and a missing file is a difference
    // nothing else would explain. Copied from the template when it has one.
    if (const auto pathSchema = schemaFromTemplate(tmpl, "Path", "CommonPathInfo", gameType);
        pathSchema.has_value()) {
        files.push_back({"/" + std::string(root) + "/jmp/" + forGame("Path", gameType) +
                             "/" + forGame("CommonPathInfo", gameType),
                         std::move(*pathSchema)});
    }
    return files;
}

// Builds the map archive a plan describes. Pure: no filesystem, no writes.
io::RarcArchive buildZoneArchive(const std::vector<PlannedZoneFile>& files, int gameType) {
    io::RarcArchive archive = io::RarcArchive::create(forGame("Stage", gameType));
    for (const auto& file : files) {
        makeParentDirectories(archive, file.path);
        archive.insert(file.path, file.table.serialize());
    }
    return archive;
}

} // namespace

std::string scenarioArchivePath(std::string_view galaxyName) {
    // Java's createGalaxy always uses the SMG2 folder shape for the scenario
    // archive; only the ROOT name is lowercased for SMG1 (StageHelper.java:293).
    // There is deliberately no gameType parameter: this path does not vary, and
    // a parameter that is accepted and ignored invites callers to believe it
    // does. See the header comment.
    return "/StageData/" + std::string(galaxyName) + "/" + std::string(galaxyName) +
           "Scenario.arc";
}

std::vector<std::string> StageCreatePlan::filesWritten() const {
    // The map archive first, then -- for a galaxy -- the scenario archive. This is
    // the order create* writes them in, so a dry run reads as a sequence rather
    // than an alphabetical jumble.
    std::vector<std::string> files{mapPath};
    if (forGalaxy()) {
        files.push_back(scenarioPath);
    }
    return files;
}

StageCreatePlan planStageZone(std::string_view name, const std::vector<std::string>& layers,
                              const std::vector<std::uint8_t>& schemaTemplate, int gameType) {
    requireSafeName(name, "zone");
    if (schemaTemplate.empty()) {
        throw std::runtime_error(
            "A zone needs a template: it is where the JMap schemas come from "
            "(use \"Bare minimum\", which is the bundled standard-zone template)");
    }
    if (layers.empty()) {
        throw std::runtime_error("A zone needs at least the Common layer");
    }

    StageCreatePlan plan;
    plan.name = std::string(name);
    plan.mapPath = stageMapFilesystemPath(name, gameType);
    plan.layers = normalizeStageLayers(layers);

    // The template is read here to learn WHICH tables it carries -- a template
    // that lacks a file contributes nothing, and that is a property of the
    // template, not of the destination, so it belongs in the plan.
    const io::RarcArchive tmpl{schemaTemplate};
    const std::string root = forGame("Stage", gameType);
    for (const auto& file : planZoneFiles(tmpl, root, plan.layers, gameType)) {
        plan.layerFiles.push_back(file.path);
    }
    return plan;
}

StageCreatePlan planGalaxy(std::string_view name, const std::vector<std::string>& extraZones,
                           const std::vector<std::string>& layers,
                           const std::vector<std::uint8_t>& schemaTemplate, int gameType) {
    requireSafeName(name, "galaxy");
    StageCreatePlan plan = planStageZone(name, layers, schemaTemplate, gameType);
    for (const auto& zoneName : extraZones) {
        requireSafeName(zoneName, "zone");
    }
    plan.extraZones = extraZones;
    plan.scenarioPath = scenarioArchivePath(name);
    return plan;
}

void createStageZone(io::DirectoryFilesystem& filesystem, std::string_view name,
                     const std::vector<std::string>& layers,
                     const std::vector<std::uint8_t>& schemaTemplate, int gameType,
                     CreatedZone* report) {
    // Every validation the old code did inline now happens in the plan, so a dry
    // run rejects exactly what the real create rejects -- including the case that
    // matters most, an already-existing zone, which is a question about the
    // DESTINATION and so cannot be answered by the plan alone.
    const StageCreatePlan plan = planStageZone(name, layers, schemaTemplate, gameType);
    if (filesystem.fileExists(plan.mapPath)) {
        throw std::runtime_error("A zone named " + std::string(name) + " already exists: " +
                                 plan.mapPath);
    }

    const io::RarcArchive tmpl{schemaTemplate};
    const io::RarcArchive archive =
        buildZoneArchive(planZoneFiles(tmpl, forGame("Stage", gameType), plan.layers, gameType),
                         gameType);

    CreatedZone created;
    created.name = plan.name;
    created.mapPath = plan.mapPath;
    created.layers = plan.layers;
    created.layerFiles = plan.layerFiles;

    // Not compressed: retail archives are Yaz0, but a fresh uncompressed one is
    // valid and the editor reads it. Compression is the game's packaging, not a
    // requirement of the format.
    // DirectoryFilesystem::write refuses to create the parent directory on
    // purpose (a typo'd path should not scatter files), so an SMG2 zone's own
    // folder is created HERE, deliberately. SMG1 zones are flat and need nothing.
    if (gameType != 1) {
        filesystem.createDirectory("/StageData/" + std::string(name));
    }
    filesystem.write(plan.mapPath, archive.serialize(false));
    if (report != nullptr) {
        *report = std::move(created);
    }
}

void createGalaxy(io::DirectoryFilesystem& filesystem, std::string_view name,
                  const std::vector<std::string>& extraZones,
                  const std::vector<std::string>& layers,
                  const std::vector<std::uint8_t>& schemaTemplate, int gameType) {
    // The plan carries every name check the old code did inline, plus the layer
    // and template checks, so `galaxy create --dry-run` refuses exactly what the
    // real command refuses.
    const StageCreatePlan plan = planGalaxy(name, extraZones, layers, schemaTemplate, gameType);
    if (filesystem.fileExists(plan.scenarioPath)) {
        throw std::runtime_error("A galaxy named " + std::string(name) + " already exists");
    }

    // The galaxy's own map zone first, exactly as Java does (createGalaxy calls
    // createZone(galaxyName, ...)).
    CreatedZone zone;
    createStageZone(filesystem, name, layers, schemaTemplate, gameType, &zone);

    // The zone links the galaxy map uses to travel between zones.
    io::RarcArchive map{filesystem.read(zone.mapPath)};
    const auto stageObjPath = "/" + std::string(map.rootName()) + "/jmp/" +
                              forGame("Placement", gameType) + "/" + forGame("Common", gameType) +
                              "/" + forGame("StageObjInfo", gameType);
    BcsvTable links(map.read(stageObjPath), map.endian());
    links.rows().clear();
    std::int32_t nextLinkId = 0;
    for (const auto& zoneName : extraZones) {
        const std::size_t row = links.addRow();
        auto& cells = links.rows()[row];
        links.setString(cells, "name", zoneName);
        links.setInt(cells, "l_id", nextLinkId++);
    }
    map.replace(stageObjPath, links.serialize());
    filesystem.write(zone.mapPath, map.serialize(map.wasCompressed()));

    // ---- the scenario archive ----------------------------------------------
    // SMG1 lowercases the ROOT but not the file name (StageHelper.java:290-294).
    std::string root = std::string(name) + "Scenario";
    if (gameType != 2) {
        root = util::toLower(root);
    }
    io::RarcArchive scenario = io::RarcArchive::create(root);

    // Every zone the ScenarioData knows: the galaxy's own, plus the extras.
    std::vector<std::string> allZones{std::string(name)};
    for (const auto& zoneName : extraZones) {
        allZones.push_back(zoneName);
    }

    // ScenarioModel owns the columns, so the schema is ASKED FOR rather than
    // restated: addScenario writes the row and its columns using the game's own
    // defaults and the SMG1/SMG2 split (PowerStarType + CometLimitTimer for SMG2,
    // IsHidden for SMG1), and one integer column per zone.
    BcsvTable scenarioData;
    BcsvTable zoneList;
    if (gameType == 1) {
        // The SMG1 marker, written BEFORE the model is built, because
        // ensureScenarioColumns() decides the game's column set from it: a table
        // with neither IsHidden nor PowerStarType reads as SMG2 and would grow
        // columns SMG1 has never had. SMG1's pair is IsHidden and nothing else
        // (populateScenarioFieldsScenarioData).
        (void)scenarioData.ensureField("IsHidden", BcsvType::integer);
    }
    {
        ScenarioModel model(scenarioData, zoneList, allZones, 0);
        const std::size_t row = model.addScenario("New Scenario");
        // Java's starter row awards a star equal to its own id
        // (StageHelper.getNewScenarioDataEntry: PowerStarId = scenarioNo).
        // addScenario() deliberately leaves a fresh mission awarding NOTHING --
        // which is right for "add a mission" in the panel, and wrong for a
        // galaxy's first mission, which would otherwise open with a galaxy the
        // game treats as handing out no stars at all.
        model.setPowerStar(row, model.scenarios()[row].number);
        // ...and records its type, which addScenario() also leaves empty for a
        // fresh row. The game's own column default is "Normal"
        // (populateScenarioFieldsScenarioData), so say it rather than leave a
        // blank the panel would have to render as "not recorded".
        if (scenarioData.hasField("PowerStarType")) {
            model.setPowerStarType(row, "Normal");
        }
        // Only the galaxy's own zone goes in the ZoneList. See section 15.
        (void)model.addZone(name);
    }
    scenario.insert("/" + root + "/ScenarioData.bcsv", scenarioData.serialize());
    scenario.insert("/" + root + "/ZoneList.bcsv", zoneList.serialize());
    if (gameType == 2) {
        // GalaxyInfo is SMG2-only: writing one for SMG1 adds a file the game has
        // never seen, and the panel treats its absence as meaningful.
        BcsvTable info;
        (void)info.ensureField("WorldNo", BcsvType::integer);
        const std::size_t row = info.addRow();
        info.setInt(info.rows()[row], "WorldNo", 0);
        scenario.insert("/" + root + "/GalaxyInfo.bcsv", info.serialize());
    }

    // The scenario archive lives in the galaxy's own folder, which for an SMG2
    // zone was already created above -- but an SMG1 zone is a flat file, so its
    // folder has never been made. DirectoryFilesystem::write will not make it.
    filesystem.createDirectory("/StageData/" + std::string(name));
    filesystem.write(plan.scenarioPath, scenario.serialize(false));
}

} // namespace whitehole::smg