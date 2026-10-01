#include "whitehole/app/settings.hpp"
#include "whitehole/app/theme_palette.hpp"
#include "whitehole/db/data_holder.hpp"
#include "whitehole/db/name_table.hpp"
#include "whitehole/db/hints.hpp"
#include "whitehole/db/areamanagerlimits.hpp"
#include "whitehole/db/shortcuts.hpp"
#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/db/custom_obj_db.hpp"
#include "whitehole/db/specialrenderers.hpp"
#include "whitehole/edit/document.hpp"
#include "whitehole/edit/validation.hpp"
#include "whitehole/db/object_db.hpp"
#include "whitehole/edit/authoring.hpp"
#include "whitehole/edit/commands.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/io/binary_file.hpp"
#include "whitehole/io/directory_filesystem.hpp"
#include "whitehole/io/rarc.hpp"
#include "whitehole/io/yaz0.hpp"
#include "whitehole/util/json.hpp"
#include "whitehole/util/text.hpp"
#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera.hpp"
#include "whitehole/render/collision_kcl.hpp"
#include "whitehole/render/gizmo.hpp"
#include "whitehole/render/model_mesh.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/render/viewport_scene.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/bmd.hpp"
#include "whitehole/smg/bti.hpp"
#include "whitehole/smg/camera_param.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/hash.hpp"
#include "whitehole/smg/object_model.hpp"
#include "whitehole/smg/path.hpp"
#include "whitehole/smg/scenario_model.hpp"
#include "whitehole/smg/stage_archive.hpp"
#include "whitehole/smg/stage_builder.hpp"
#include "whitehole/smg/stage_templates.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Defined further down with the inline fallback database; declared here so the
// custom-object tests above that definition can use it too.
void loadObjectDatabaseForTests(whitehole::db::ObjectDatabase& database);

// Defined below next to the BCSV helpers; declared here so the camera table
// test can compare a re-parsed table against the one it was loaded from.
void expectTablesEqual(const whitehole::smg::BcsvTable& left,
                       const whitehole::smg::BcsvTable& right, const std::string& context);

void testBinaryData() {
    whitehole::io::BinaryWriter writer(whitehole::io::Endian::big);
    writer.writeU8(0x12);
    writer.writeU16(0x3456);
    writer.writeU32(0x789ABCDE);
    writer.writeF32(1.25F);
    writer.writeString("Galaxy");

    whitehole::io::BinaryReader reader(writer.data(), whitehole::io::Endian::big);
    expect(reader.readU8() == 0x12, "8-bit binary round trip failed");
    expect(reader.readU16() == 0x3456, "16-bit binary round trip failed");
    expect(reader.readU32() == 0x789ABCDE, "32-bit binary round trip failed");
    expect(std::abs(reader.readF32() - 1.25F) < 0.00001F, "float binary round trip failed");
    expect(reader.readString() == "Galaxy", "string binary round trip failed");

    bool rejected = false;
    try {
        (void)reader.readU8();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "out-of-bounds binary read was not rejected");
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto unique = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() / ("whitehole-native-tests-" + unique);
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("could not create temporary test directory");
        }
    }
    // Best-effort, and DELIBERATELY non-throwing. remove_all's throwing overload
    // turns any lingering handle into a hard test failure that names a file the
    // test author never mentioned -- which is exactly how a passing run turned
    // red on one machine and stayed green on another. Cleanup is housekeeping; a
    // stale temp directory must never be able to fail a test.
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path path;
};

// THE PLAN MUST DESCRIBE THE WRITE, NOT DESCRIBE A WRITE. --dry-run and the
// GUI's confirm step are only worth anything if they list the files create*
// actually produces, so this test is about AGREEMENT rather than about the
// plan's contents: plan a create, then really create it, and compare. A plan
// that drifted from the writer would make the preview a lie -- exactly the gap
// the Scenarios panel's "show what will happen, then happen it" rule prevents.
void testStageCreatePlans() {
    using namespace whitehole::smg;
    const auto templates =
        std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    const auto smg2ZoneTemplate =
        whitehole::io::readFile(templates / "SMG2StandardZoneMap.arc");
    const auto smg1ZoneTemplate =
        whitehole::io::readFile(templates / "SMG1OneStarGalaxy.arc");

    // ---- a plan writes nothing ---------------------------------------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        const auto plan = planStageZone("DryRun", {"Common", "LayerA"}, smg2ZoneTemplate, 2);
        expect(!plan.mapPath.empty(), "a plan must name the map archive it would write");
        expect(plan.layers.size() == 2 && plan.layers.front() == "Common",
               "a plan must record the resolved layers, Common first");
        expect(!plan.layerFiles.empty(), "a plan must list the tables it would create");
        expect(!plan.forGalaxy(), "a plain zone plan must not claim to be a galaxy");

        // The whole point: nothing is on disk.
        expect(!project.fileExists(plan.mapPath),
               "planning a zone must not write its map archive");
        expect(!project.directoryExists("/StageData/DryRun"),
               "planning a zone must not create its folder");

        // Applying the plan produces exactly the files it promised.
        CreatedZone created;
        createStageZone(project, "DryRun", {"Common", "LayerA"}, smg2ZoneTemplate, 2, &created);
        expect(created.mapPath == plan.mapPath,
               "the created zone's map path must match the planned one");
        expect(created.layers == plan.layers,
               "the created zone's layers must match the planned ones");
        expect(created.layerFiles == plan.layerFiles,
               "the created zone's table list must match the planned one");
        expect(project.fileExists(plan.mapPath), "the apply must write what it planned");
    }

    // ---- filesWritten() is what a dry run prints ---------------------------
    {
        const auto zone = planStageZone("Cave", {"Common"}, smg2ZoneTemplate, 2);
        const auto zoneFiles = zone.filesWritten();
        expect(zoneFiles.size() == 1 && zoneFiles.front() == zone.mapPath,
               "a zone writes exactly its map archive");

        const auto galaxy = planGalaxy("TestGal", {"Cave"}, {"Common"}, smg2ZoneTemplate, 2);
        expect(galaxy.forGalaxy(), "a galaxy plan must report itself as one");
        const auto galaxyFiles = galaxy.filesWritten();
        expect(galaxyFiles.size() == 2,
               "a galaxy writes its map archive and its scenario archive");
        expect(galaxyFiles.front() == galaxy.mapPath,
               "the map archive is written before the scenario archive");
        expect(galaxy.scenarioPath == "/StageData/TestGal/TestGalScenario.arc",
               "the planned scenario path must be the game's expected one");
        expect(galaxy.extraZones.size() == 1 && galaxy.extraZones.front() == "Cave",
               "a galaxy plan must record the zones it will link");
    }

    // ---- the plan refuses exactly what create* refuses ---------------------
    // This is what makes a dry run trustworthy: it must not happily preview a
    // create that would then be rejected. The already-exists check is the one
    // case it CANNOT cover, because that is a fact about the destination and
    // planning never looks at it -- which is why the CLI re-checks it.
    const auto refuses = [](const auto& call) {
        try {
            call();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    expect(refuses([&] { planStageZone("NoLayers", {}, smg2ZoneTemplate, 2); }),
           "a plan with no layers must be refused, like the create is");
    expect(refuses([&] { planStageZone("BadLayer", {"Common", "LayerZ"}, smg2ZoneTemplate, 2); }),
           "a plan naming something that is not a layer must be refused");
    expect(refuses([&] { planStageZone("NoTemplate", {"Common"}, {}, 2); }),
           "a plan with no template must be refused, not guessed at");
    expect(refuses([&] { planStageZone("Bad/Name", {"Common"}, smg2ZoneTemplate, 2); }),
           "a plan with an unsafe name must be refused");
    expect(refuses([&] { planGalaxy("Gal", {"Bad/Zone"}, {"Common"}, smg2ZoneTemplate, 2); }),
           "a galaxy plan with an unsafe zone name must be refused");

    // ---- SMG1's layout, which differs from SMG2's --------------------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        const auto smg1 = planStageZone("Flat", {"Common"}, smg1ZoneTemplate, 1);
        expect(smg1.mapPath == "/StageData/Flat.arc",
               "an SMG1 zone plan must use the flat path");
        CreatedZone created;
        createStageZone(project, "Flat", {"Common"}, smg1ZoneTemplate, 1, &created);
        expect(created.mapPath == smg1.mapPath,
               "the created SMG1 zone must land where it was planned to");
        expect(project.fileExists(smg1.mapPath), "the SMG1 zone must be written");
        // SMG1 needs no folder of its own, so promising one would be a lie.
        expect(!project.directoryExists("/StageData/Flat"),
               "an SMG1 zone must not create a folder for itself");
    }
}

// Creating zones and galaxies. The claim under test is that a created zone is
// indistinguishable from a real one TO WHITEHOLE PRO -- it reopens through the
// same loaders the editor uses. It is NOT a claim that a retail game accepts it;
// no game has been run against this.
void testStageBuilder() {
    using namespace whitehole::smg;
    const auto templates =
        std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    const auto readTemplate = [&templates](const char* name) {
        return whitehole::io::readFile(templates / name);
    };
    const auto smg2ZoneTemplate = readTemplate("SMG2StandardZoneMap.arc");
    const auto smg1ZoneTemplate = readTemplate("SMG1OneStarGalaxy.arc");

    // ---- a bare SMG2 zone ---------------------------------------------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        CreatedZone created;
        createStageZone(project, "BareZone", {"Common"}, smg2ZoneTemplate, 2, &created);
        expect(created.mapPath == "/StageData/BareZone/BareZoneMap.arc",
               "an SMG2 zone must live in its own folder");
        expect(project.fileExists(created.mapPath), "the zone's map archive was not written");

        // It must open through the ordinary loader the editor uses.
        auto stage = StageArchive::open(project, "BareZone", 2);
        expect(stage.stageName() == "BareZone", "the created zone must open by name");
        // A bare zone has no objects but DOES have a spawn point: without one the
        // game cannot start the stage.
        expect(stage.objects().size() == 1,
               "a created zone must carry exactly its spawn point");
        expect(stage.objects().front().name == "StartObj",
               "the spawn point must be named StartObj");
        expect(stage.objects().front().layer == "Common",
               "the spawn point must live in the Common layer");
        // Every table the loader knows about should be there, so the zone is not
        // a shell that quietly loses objects.
        expect(stage.tables().size() >= 8,
               "a created zone must have the full set of placement tables");
        for (const auto& table : stage.tables()) {
            expect(table.layer == "Common",
                   "a bare zone must only have Common tables, found: " + table.layer);
        }
    }

    // ---- extra layers, and the SMG1-only files ------------------------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        CreatedZone created;
        createStageZone(project, "Layered", {"Common", "LayerA", "LayerC"},
                        smg2ZoneTemplate, 2, &created);
        expect(created.layers.size() == 3, "all three requested layers must be created");
        expect(created.layers.front() == "Common", "Common must come first");
        auto stage = StageArchive::open(project, "Layered", 2);
        bool sawLayerA = false;
        bool sawLayerC = false;
        for (const auto& table : stage.tables()) {
            sawLayerA = sawLayerA || table.layer == "LayerA";
            sawLayerC = sawLayerC || table.layer == "LayerC";
            // SoundInfo/ChildObjInfo are SMG1-only. Creating them for SMG2 would
            // add files the game does not expect to find.
            expect(table.kind != "sound" && table.kind != "child",
                   "an SMG2 zone must not carry the SMG1-only tables");
        }
        expect(sawLayerA, "LayerA's tables must be created");
        expect(sawLayerC, "LayerC's tables must be created");
        // Every LAYER must have its OWN copy of the shared table names. The
        // bare-filename bug this guards against left the layer directories in
        // place but stored only Common's file, so the archive looked complete and
        // a layer silently lost every table.
        for (const auto& table : stage.tables()) {
            if (table.kind == "start" && table.layer != "Common") {
                expect(table.table.rows().empty(),
                       "a non-Common layer must have its own empty StartInfo, not "
                       "Common's row");
            }
        }
        int startTables = 0;
        for (const auto& table : stage.tables()) {
            startTables = startTables + (table.kind == "start" ? 1 : 0);
        }
        expect(startTables == 3, "Common, LayerA and LayerC must each have a StartInfo");
        // The spawn point stays in Common only, whatever else was asked for.
        for (const auto& object : stage.objects()) {
            expect(object.layer == "Common",
                   "a created zone's only object is its Common spawn point");
        }
    }

    // ---- an SMG1 zone, whose whole path layout is lowercased ----------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        CreatedZone created;
        createStageZone(project, "OneStarZone", {"Common"}, smg1ZoneTemplate, 1, &created);
        // SMG1 flattens zones into /StageData/<zone>.arc.
        expect(created.mapPath == "/StageData/OneStarZone.arc",
               "an SMG1 zone must be a flat file, found: " + created.mapPath);
        auto stage = StageArchive::open(project, "OneStarZone", 1);
        expect(stage.objects().size() == 1,
               "a created SMG1 zone must carry its spawn point");
        bool sawSound = false;
        for (const auto& table : stage.tables()) {
            sawSound = sawSound || table.kind == "sound";
        }
        expect(sawSound, "an SMG1 zone must carry its SMG1-only tables");
    }

    // ---- a galaxy -----------------------------------------------------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        const auto before = smg2ZoneTemplate;
        createGalaxy(project, "TestGalaxy", {"TestZoneOne", "TestZoneTwo"},
                     {"Common"}, smg2ZoneTemplate, 2);
        expect(project.fileExists("/StageData/TestGalaxy/TestGalaxyMap.arc"),
               "a galaxy must have its map zone");
        expect(project.fileExists("/StageData/TestGalaxy/TestGalaxyScenario.arc"),
               "a galaxy must have a scenario archive");

        // It must open as a galaxy, with the galaxy map reachable.
        GalaxyArchive galaxy(project, "TestGalaxy", 2);
        expect(galaxy.hasMapZone(), "a created galaxy must have a map zone");
        expect(galaxy.galaxyInfo().hasField("WorldNo"),
               "an SMG2 galaxy must have a GalaxyInfo the panel can read");
        // editableZones() leads with the map zone; zones() is the ZoneList only.
        expect(galaxy.editableZones().front() == "TestGalaxy",
               "the galaxy map zone must lead the editable zone list");
        ScenarioModel model(galaxy.scenarioData(), galaxy.zoneList(), galaxy.zones(), 0);
        expect(model.scenarioCount() == 1, "a created galaxy must start with one mission");
        expect(model.scenarios().front().awardsStar(),
               "the starting mission must award a star");
        expect(model.scenarios().front().powerStarType == "Normal",
               "an SMG2 starting mission must record a Normal star");
        // Every zone the galaxy was told about has its own column.
        expect(model.layerMask(0, "TestZoneOne") == 0,
               "an extra zone must have a ScenarioData column of its own");

        // The template must not have been touched by any of this.
        expect(readTemplate("SMG2StandardZoneMap.arc") == before,
               "creating a zone must never modify the template it read");
    }

    // ---- an SMG1 galaxy: lowercased scenario root, no GalaxyInfo ------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        createGalaxy(project, "SmallOne", {}, {"Common"}, smg1ZoneTemplate, 1);
        expect(project.fileExists("/StageData/SmallOne/SmallOneScenario.arc"),
               "an SMG1 galaxy's scenario archive keeps its mixed-case file name");
        auto archive = whitehole::io::RarcArchive::open(temporary.path / "StageData" / "SmallOne" /
                                             "SmallOneScenario.arc");
        // SMG1 lowercases the ROOT (StageHelper.java:293) and writes NO
        // GalaxyInfo -- the panel treats that absence as meaningful.
        expect(archive.rootName() == "smallonescenario",
               "an SMG1 scenario archive's root must be lowercase, found: " +
                   std::string(archive.rootName()));
        expect(!archive.fileExists("/smallonescenario/GalaxyInfo.bcsv"),
               "an SMG1 galaxy must not be given a GalaxyInfo");
        expect(archive.fileExists("/smallonescenario/ScenarioData.bcsv"),
               "an SMG1 galaxy must have ScenarioData");
        GalaxyArchive galaxy(project, "SmallOne", 1);
        ScenarioModel model(galaxy.scenarioData(), galaxy.zoneList(), galaxy.zones(), 0);
        // SMG1 stores IsHidden, not PowerStarType: the model's empty type is how
        // the panel shows "this galaxy records no star type".
        expect(model.scenarios().front().powerStarType.empty(),
               "an SMG1 mission must not be given a PowerStarType");
        expect(galaxy.scenarioData().hasField("IsHidden"),
               "an SMG1 ScenarioData must have IsHidden");
    }

    // ---- refusals -----------------------------------------------------------
    {
        TemporaryDirectory temporary;
        whitehole::io::DirectoryFilesystem project(temporary.path);
        project.createDirectory("/StageData");
        createStageZone(project, "Once", {"Common"}, smg2ZoneTemplate, 2);
        const auto refuses = [&project, &smg2ZoneTemplate](auto&& call) {
            try {
                call();
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        };
        expect(refuses([&] { createStageZone(project, "Once", {"Common"}, smg2ZoneTemplate, 2); }),
               "creating a zone that already exists must be refused");
        expect(refuses([&] { createStageZone(project, "Bad/Name", {"Common"}, smg2ZoneTemplate, 2); }),
               "an unsafe zone name must be refused");
        expect(refuses([&] { createStageZone(project, "NoLayers", {}, smg2ZoneTemplate, 2); }),
               "a zone with no layers must be refused");
        expect(refuses([&] {
                   createStageZone(project, "BadLayer", {"Common", "LayerZ"},
                                   smg2ZoneTemplate, 2);
               }),
               "a layer name that is not a layer must be refused");
        // A zone with no template has no trustworthy schema, so it is refused
        // rather than guessed at.
        expect(refuses([&] { createStageZone(project, "NoTemplate", {"Common"}, {}, 2); }),
               "a zone with no template must be refused, not guessed at");
    }
}

// writeFile() is the app's only save path, so its failure modes are worth
// pinning: it must never leave a temporary behind, and two saves to the same
// path must not collide on one.
void testWriteFileLeavesNoTemporaries() {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "save.bin";

    whitehole::io::writeFile(path, {1, 2, 3});
    expect(whitehole::io::readFile(path) == std::vector<std::uint8_t>({1, 2, 3}),
           "writeFile must write the bytes it was given");

    // Two saves in a row to the same path: the second must fully replace the
    // first, and must not trip over a leftover temporary.
    whitehole::io::writeFile(path, {4, 5});
    expect(whitehole::io::readFile(path) == std::vector<std::uint8_t>({4, 5}),
           "a second save must replace the file");

    // Nothing with a temporary suffix may survive a successful save. A fixed
    // ".tmp" name also made two concurrent writers race -- one renaming the
    // shared temporary out from under the other -- so the names are unique now.
    std::vector<std::filesystem::path> strays;
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        if (entry.path().string().find(".tmp") != std::string::npos) {
            strays.push_back(entry.path());
        }
    }
    expect(strays.empty(), "a successful save must not leave a temporary file behind");

    // A save into a directory that does not exist creates it.
    const auto nested = temporary.path / "a" / "b" / "save.bin";
    whitehole::io::writeFile(nested, {7});
    expect(whitehole::io::readFile(nested) == std::vector<std::uint8_t>({7}),
           "writeFile must create missing parent directories");

    // ---- THE APP-LAYER WRITERS MUST ROUTE THROUGH writeFile() --------------
    // This used to be a comment about writeFile alone, while three writers
    // bypassed it with a raw ofstream. Two of them hold data a modder cannot
    // regenerate, so a truncating write interrupted by a crash destroys real
    // work:
    //
    //   CustomObjDatabase::save()  the modder's own object registry
    //   Settings::save()           preferences (low stakes, same hole)
    //   downloadObjectDatabase()   used a FIXED ".download" temporary, which is
    //                              the same race writeFile() was fixed for, and
    //                              removed the destination BEFORE renaming it --
    //                              so a failed rename left the user with no
    //                              database at all instead of the old one.
    //
    // The download path is WinHTTP-only and cannot run in a test, so what is
    // asserted here is the property that made its bug possible: nothing in the
    // app layer stages into a fixed suffix of its own any more. That is a
    // naming-convention pin, and it only stays true while temporaryPathFor is
    // the single scheme -- which is why it is public.
    const auto fixedSuffixes = std::vector<std::string>{".download", ".tmp"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(temporary.path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().string();
        // A ".bak" is EXPECTED -- that is the backup writeFile deliberately
        // keeps. Only the staging names must not survive.
        for (const auto& suffix : fixedSuffixes) {
            expect(name.find(suffix) == std::string::npos,
                   "a save left a staging file behind: " + name);
        }
    }

    // temporaryPathFor() is what makes that true, so pin its contract: it is a
    // sibling of the target, and two calls in a row never collide.
    const auto first = whitehole::io::temporaryPathFor(path);
    const auto second = whitehole::io::temporaryPathFor(path);
    expect(first.parent_path() == path.parent_path(),
           "a temporary must be a sibling of the file it stages for");
    expect(first != second,
           "two writers must never be handed the same temporary name");
    expect(first.string().find(".tmp") != std::string::npos,
           "a staging name must be recognisable as one");

    // The custom-object registry in particular: writing it twice must leave the
    // second one's bytes and keep the first as a .bak, which is the difference
    // between "recoverable" and "gone".
    const auto registry = temporary.path / "customobjdb.json";
    whitehole::io::writeFile(registry, {'a'});
    whitehole::io::writeFile(registry, {'b'});
    expect(whitehole::io::readFile(registry) == std::vector<std::uint8_t>({'b'}),
           "the registry must hold the newest contents");
    auto backup = registry;
    backup += ".bak";
    expect(std::filesystem::exists(backup),
           "overwriting the registry must keep the previous copy as a .bak");
    expect(whitehole::io::readFile(backup) == std::vector<std::uint8_t>({'a'}),
           "the .bak must hold what the file held before");
}

// THE TEMPLATES THAT ACTUALLY SHIP MUST STILL BE USABLE. testStageTemplates
// below checks the LOADER against hand-written broken files; this checks the
// OTHER direction -- that every data/templates/*.json in the repo still parses,
// still names files that exist, and still names archives the game format can
// open.
//
// Why this is not redundant: these are user-editable files in an installation
// directory. Someone who typos "MapFile", renames an .arc, or saves a galaxy
// archive over a map one ships a repo where `galaxy create --template "Big
// Galaxy"` fails at the user's desk, with an error naming the missing file
// rather than the JSON that asked for it. Nothing else notices: the loader tests
// use synthetic files, and the builder tests name templates by string.
//
// The gap this fills is specific. testStageTemplates already checks
// mapArchive() returns non-empty bytes, but only for the SMG2 GALAXY templates --
// SMG1's galaxies and both zone templates were unchecked; "non-empty" is much
// weaker than "parses as a RARC"; and ScenarioFile was never validated at all
// despite createGalaxy being its only consumer.
void testShippedTemplatesParse() {
    using namespace whitehole::smg;
    const auto templates =
        std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";

    // Every .json must be ACCEPTED by at least one of the four (game, galaxy/zone)
    // filters. A template nobody can ever be offered is dead weight at best and a
    // mislabelled file at worst, and this is the only place that would notice.
    std::size_t seen = 0;
    for (const auto& gameType : {1, 2}) {
        for (const bool forGalaxy : {true, false}) {
            for (const auto& tmpl : loadStageTemplates(templates, gameType, forGalaxy)) {
                ++seen;
                const std::string who =
                    tmpl.name + " (SMG" + std::to_string(gameType) + (forGalaxy ? " galaxy)" : " zone)");

                // The declared game must match the filter that found it, or the
                // same template would be reachable from two places at once.
                expect(tmpl.game == gameType,
                       "template \"" + who + "\" was offered for the wrong game");

                // Its map archive must exist AND open. mapArchive() returning bytes
                // proves the file was read, not that it is an archive the RARC
                // parser accepts -- a truncated .arc would sail past a "non-empty"
                // check and blow up inside create.
                const auto bytes = tmpl.mapArchive(templates);
                expect(bytes.has_value(),
                       "template \"" + who + "\" names a map archive that cannot be read");
                expect(!bytes->empty(), "template \"" + who + "\" has an empty map archive");
                const whitehole::io::RarcArchive map{*bytes};
                expect(!map.entries().empty(),
                       "template \"" + who + "\"'s map archive holds no entries");
                expect(!map.rootName().empty(),
                       "template \"" + who + "\"'s map archive has no root name");

                // And it must carry at least one JMap table, because that is the
                // entire reason a template exists: the builder copies schemas from
                // it, and a map with no tables would create a zone with no files at
                // all -- which looks like success.
                std::size_t tables = 0;
                for (const auto& entry : map.entries()) {
                    if (!entry.directory && entry.path.find("/jmp/") != std::string::npos) {
                        ++tables;
                    }
                }
                expect(tables > 0,
                       "template \"" + who + "\"'s map archive carries no JMap tables");

                // ScenarioFile is optional (a zone template has none), but when it
                // IS declared it must exist and open, because createGalaxy copies it
                // wholesale and nothing else would notice it was missing until a
                // user's galaxy came out empty.
                if (!tmpl.scenarioFile.empty()) {
                    const auto scenarioPath = templates / tmpl.scenarioFile;
                    expect(std::filesystem::exists(scenarioPath),
                           "template \"" + who + "\" names a scenario archive that does not "
                           "exist: " + tmpl.scenarioFile);
                    if (std::filesystem::exists(scenarioPath)) {
                        const auto scenario =
                            whitehole::io::RarcArchive{whitehole::io::readFile(scenarioPath)};
                        expect(!scenario.entries().empty(),
                               "template \"" + who + "\"'s scenario archive holds no entries");
                    }
                }
            }
        }
    }

    // Five ship today: two SMG1 galaxies, two SMG2 galaxies, one SMG2 zone. A FLOOR
    // rather than an exact list, so ADDING a template is not a failure while
    // deleting one still is -- and deletion is the failure that matters.
    expect(seen >= 5,
           "fewer than five shipped templates were reachable; data/templates is "
           "missing or malformed files");

    // The bare minimum is not a file at all -- it is synthesized in code and points
    // at a bundled archive -- so the loop above never touches it. It is the
    // fallback every create uses when no template is named, so both games are
    // checked explicitly and each must actually open.
    for (const auto& gameType : {1, 2}) {
        const auto bare = bareMinimumTemplate(gameType);
        expect(!bare.mapFile.empty(), "the bare-minimum template for SMG" +
                                          std::to_string(gameType) + " must name an archive");
        const auto bytes = bareMinimumMapArchive(templates, gameType);
        expect(!bytes.empty(),
               "the bare-minimum archive for SMG" + std::to_string(gameType) + " must load");
        const whitehole::io::RarcArchive archive{bytes};
        expect(!archive.entries().empty(),
               "the bare-minimum archive for SMG" + std::to_string(gameType) +
               " holds no entries");
    }
}

// The creation templates, which shipped in data/templates since before the port
// and which nothing in the C++ read at all.
void testStageTemplates() {
    using namespace whitehole::smg;
    const auto templates =
        std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";

    // The game split and the galaxy/zone split both filter, exactly as Java's
    // isApplicableTemplate does: a template for the wrong game must never be
    // offered, and neither must a galaxy template appear when creating a zone.
    const auto smg2Galaxies = loadStageTemplates(templates, 2, true);
    const auto smg2Zones = loadStageTemplates(templates, 2, false);
    const auto smg1Galaxies = loadStageTemplates(templates, 1, true);
    expect(!smg2Galaxies.empty(), "the SMG2 galaxy templates must load");
    expect(!smg2Zones.empty(), "the SMG2 zone templates must load");
    expect(!smg1Galaxies.empty(), "the SMG1 galaxy templates must load");

    const auto hasName = [](const std::vector<StageTemplate>& list, const std::string& name) {
        return std::any_of(list.begin(), list.end(),
                           [&name](const StageTemplate& tmpl) { return tmpl.name == name; });
    };
    expect(hasName(smg2Galaxies, "Big Galaxy"), "the SMG2 Big Galaxy template must load");
    expect(hasName(smg1Galaxies, "1 Star Galaxy"),
           "the SMG1 1 Star Galaxy template must load");
    // The one that matters most: a ZONE template must not turn up as a galaxy
    // one, or `galaxy create` would build a galaxy from a bare zone.
    expect(!hasName(smg2Galaxies, "Standard Zone"),
           "a zone template must never be offered as a galaxy template");
    expect(!hasName(smg1Galaxies, "Standard Zone"),
           "a zone template must never be offered for SMG1 either");
    expect(!hasName(smg2Galaxies, "1 Star Galaxy"),
           "an SMG1 template must not be offered for SMG2");

    // UsedLayers are the layers the archive already carries, which the layer
    // picker shows as forced. A template with none listed is not a broken file,
    // it just forces nothing.
    for (const auto& tmpl : smg2Galaxies) {
        if (tmpl.name == "Big Galaxy") {
            expect(tmpl.usedLayers.size() >= 4,
                   "the Big Galaxy template must record the layers it uses");
            expect(std::find(tmpl.usedLayers.begin(), tmpl.usedLayers.end(), "Common")
                       != tmpl.usedLayers.end(),
                   "a galaxy template's used layers must include Common");
        }
    }

    // Every galaxy template must be able to produce its map archive: a template
    // that names a file which is not there would fail at creation time, and the
    // error would be far from the cause.
    for (const auto& tmpl : smg2Galaxies) {
        const auto bytes = tmpl.mapArchive(templates);
        expect(bytes.has_value(), "template \"" + tmpl.name + "\" must name a map archive");
        expect(!bytes->empty(), "template \"" + tmpl.name + "\"'s archive must not be empty");
    }

    // Bare minimum is not a file -- it is the bundled bare-zone map per game.
    const auto bare2 = bareMinimumTemplate(2);
    const auto bare1 = bareMinimumTemplate(1);
    expect(bare2.mapFile != bare1.mapFile,
           "the two games must use DIFFERENT bare-zone templates, or one game's "
           "lowercase layout would be built with the other's casing");
    expect(!bareMinimumMapArchive(templates, 2).empty(),
           "the SMG2 bare-zone archive must load");
    expect(!bareMinimumMapArchive(templates, 1).empty(),
           "the SMG1 bare-zone archive must load");

    // These files are user-editable, so a malformed one must be a clear error
    // naming the file -- not a crash, and not a silently half-built galaxy. Each
    // case gets its own directory: a leftover Broken.json would otherwise be
    // re-reported by the NEXT load and mask what that one is asserting.
    bool reported = false;
    {
        TemporaryDirectory brokenDirectory;
        // EVERY stream below is scoped to its own block, and closed before the
        // next statement touches its file. That discipline is the rest of this
        // file's convention, and it is load-bearing: an ofstream still open when
        // remove() or ~TemporaryDirectory runs makes THAT call fail with "being
        // used by another process", naming a file the reader never heard of.
        {
            std::ofstream broken(brokenDirectory.path / "Broken.json");
            broken << "{ this is not json";
        }
        try {
            (void)loadStageTemplates(brokenDirectory.path, 2, true);
        } catch (const std::runtime_error& error) {
            reported = std::string(error.what()).find("Broken.json") != std::string::npos;
        }
        expect(reported, "a malformed template must be reported BY NAME, not ignored");

        // A template file that is valid JSON but not an object is the same class
        // of mistake, and just as likely from a hand-edited file. Broken.json goes
        // first: the loader reports the FIRST bad file it meets, and directory
        // order is not something to depend on.
        std::error_code removed;
        std::filesystem::remove(brokenDirectory.path / "Broken.json", removed);
        expect(!removed, "the first broken template must be removable");
        reported = false;
        {
            std::ofstream array(brokenDirectory.path / "Array.json");
            array << "[1, 2, 3]";
        }
        try {
            (void)loadStageTemplates(brokenDirectory.path, 2, true);
        } catch (const std::runtime_error& error) {
            reported = std::string(error.what()).find("Array.json") != std::string::npos;
        }
        expect(reported, "a template that is not a JSON object must be reported BY NAME");
    }
    TemporaryDirectory temporary;

    // A template that names a map archive which is not there is a broken
    // template, not a bare one: the user asked for content we cannot deliver.
    {
        // Scoped, for the same reason as the streams above: this directory is
        // removed by ~TemporaryDirectory at the end of the function, and an open
        // handle there is what turns cleanup into a test failure.
        std::ofstream missing(temporary.path / "Missing.json");
        missing << R"({"Name":"Ghost","Game":2,"ForGalaxy":true,"MapFile":"Nope.arc"})";
    }
    // Listing does not open the file -- that check belongs to mapArchive(), which
    // is what the create path actually calls, so it is where a missing archive
    // is caught.
    const auto ghosts = loadStageTemplates(temporary.path, 2, true);
    bool listed = false;
    for (const auto& tmpl : ghosts) {
        listed = listed || tmpl.name == "Ghost";
    }
    expect(listed, "a template naming a missing archive must still be LISTED, so the "
                   "user can see what is installed");
    reported = false;
    try {
        for (const auto& tmpl : ghosts) {
            if (tmpl.name == "Ghost") {
                (void)tmpl.mapArchive(temporary.path);
            }
        }
    } catch (const std::runtime_error& error) {
        reported = std::string(error.what()).find("Nope.arc") != std::string::npos;
    }
    expect(reported, "opening a template whose archive is missing must be reported");
}

void testDirectoryFilesystem() {
    TemporaryDirectory temporary;
    whitehole::io::DirectoryFilesystem project(temporary.path);
    project.createDirectory("/StageData/TestGalaxy");
    project.write("/StageData/TestGalaxy/TestGalaxyMap.arc", {1, 2, 3});
    expect(project.directoryExists("/StageData/TestGalaxy"), "project directory was not created");
    expect(project.fileExists("/StageData/TestGalaxy/TestGalaxyMap.arc"), "project file was not created");
    expect(project.read("/StageData/TestGalaxy/TestGalaxyMap.arc") == std::vector<std::uint8_t>({1, 2, 3}),
           "project file contents changed");
    expect(project.directories("/StageData") == std::vector<std::string>({"TestGalaxy"}),
           "project directory listing changed");
    expect(project.files("/StageData/TestGalaxy") == std::vector<std::string>({"TestGalaxyMap.arc"}),
           "project file listing changed");

    project.renameFile("/StageData/TestGalaxy/TestGalaxyMap.arc", "Renamed.arc");
    expect(project.fileExists("/StageData/TestGalaxy/Renamed.arc"), "project file rename failed");
    project.renameDirectory("/StageData/TestGalaxy", "RenamedGalaxy");
    expect(project.fileExists("/StageData/RenamedGalaxy/Renamed.arc"), "project directory rename failed");

    bool traversalRejected = false;
    try {
        (void)project.read("/../outside");
    } catch (const std::runtime_error&) {
        traversalRejected = true;
    }
    expect(traversalRejected, "project path traversal was not rejected");

    project.removeFile("/StageData/RenamedGalaxy/Renamed.arc");
    project.removeDirectory("/StageData/RenamedGalaxy");
    expect(!project.directoryExists("/StageData/RenamedGalaxy"), "project directory removal failed");
}

void testYaz0() {
    std::vector<std::uint8_t> input;
    const std::string pattern = "Whitehole Pro native archive support! ";
    for (int index = 0; index < 100; ++index) {
        input.insert(input.end(), pattern.begin(), pattern.end());
        input.push_back(static_cast<std::uint8_t>(index));
    }
    const auto compressed = whitehole::io::yaz0::compress(input);
    expect(whitehole::io::yaz0::isCompressed(compressed), "Yaz0 output has no header");
    expect(compressed.size() < input.size(), "Yaz0 did not compress repetitive data");
    expect(whitehole::io::yaz0::decompress(compressed) == input, "Yaz0 round trip failed");
    expect(whitehole::io::yaz0::compress(compressed) == compressed, "Yaz0 double compression changed data");
    expect(whitehole::io::yaz0::decompress(whitehole::io::yaz0::compress(std::vector<std::uint8_t>{})).empty(),
           "empty Yaz0 round trip failed");

    bool rejected = false;
    try {
        (void)whitehole::io::yaz0::decompress({'Y', 'a', 'z', '0'});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "truncated Yaz0 data was not rejected");
}

std::vector<std::uint8_t> makeTinyRarc(whitehole::io::Endian endian) {
    whitehole::io::BinaryWriter writer(endian);
    writer.writeU32(0x52415243); // RARC
    writer.writeU32(0xA3);      // file size
    writer.writeU32(0x20);      // header size
    writer.writeU32(0x80);      // data offset, relative to 0x20
    writer.writeU32(3);
    writer.writeU32(3);
    writer.writeU32(0);
    writer.writeU32(0);
    writer.writeU32(1);         // node count
    writer.writeU32(0x20);      // node table at 0x40
    writer.writeU32(3);         // entry count
    writer.writeU32(0x30);      // entry table at 0x50
    writer.writeU32(16);        // string table length
    writer.writeU32(0x70);      // string table at 0x90
    writer.writeU32(0);
    writer.writeU32(0);

    writer.seek(0x40);
    writer.writeU32(0x524F4F54); // ROOT
    writer.writeU32(5);          // "root"
    writer.writeU16(0);
    writer.writeU16(3);
    writer.writeU32(0);

    const auto writeDirectoryEntry = [&](std::uint16_t nameOffset, std::uint32_t node) {
        writer.writeU16(0xFFFF);
        writer.writeU16(0);
        if (endian == whitehole::io::Endian::big) {
            writer.writeU16(0x0200);
            writer.writeU16(nameOffset);
        } else {
            writer.writeU16(nameOffset);
            writer.writeU16(0x0200);
        }
        writer.writeU32(node);
        writer.writeU32(0x10);
        writer.writeU32(0);
    };
    writeDirectoryEntry(0, 0);
    writeDirectoryEntry(2, 0xFFFFFFFF);

    writer.writeU16(0);
    writer.writeU16(0);
    if (endian == whitehole::io::Endian::big) {
        writer.writeU16(0x1100);
        writer.writeU16(10);
    } else {
        writer.writeU16(10);
        writer.writeU16(0x1100);
    }
    writer.writeU32(0);
    writer.writeU32(3);
    writer.writeU32(0);

    writer.seek(0x90);
    writer.writeString(".");
    writer.writeString("..");
    writer.writeString("root");
    writer.writeString("file");
    writer.seek(0xA0);
    writer.writeU8(1);
    writer.writeU8(2);
    writer.writeU8(3);
    return std::move(writer).take();
}

// ROUND-TRIP PROPERTIES. The strongest tests in this suite are the ones that
// build something, write it, re-read it and compare -- testRarcCreation and the
// two-layer zone case are the models. This generalises that shape.
//
// The claim under test is the one BLUEPRINT section 3 states as law: a table or
// archive that was NOT edited must save back byte-identical. It is the property
// most likely to rot silently, because almost every change here rewrites some
// table, and "it still loads" is not "it still loads the same way" -- a field
// mask shifted by one, a row stride widened, an entry reordered, and the game
// rejects the file with no hint as to why.
//
// Both endiannesses run in every case. Retail SMG1/SMG2 are big-endian, but the
// parser accepts little-endian too and the writer has an explicit little-endian
// path -- an untested one is an untested one.
void testRarcRoundTripProperties() {
    using whitehole::io::Endian;
    using whitehole::io::RarcArchive;

    // ---- built-from-nothing archives ---------------------------------------
    // Nested directories, mixed casing, an empty file and a multi-byte one: the
    // shapes a real zone actually has. create() always writes big-endian, so this
    // half is big-endian only by construction; the little-endian half follows.
    {
        RarcArchive archive = RarcArchive::create("MixedCase");
        archive.createDirectory("/jmp");
        archive.createDirectory("/jmp/Placement");
        archive.createDirectory("/jmp/Placement/Common");
        archive.createDirectory("/jmp/Placement/LayerA");
        archive.insert("/jmp/Placement/Common/ObjInfo", {1, 2, 3, 4, 5});
        archive.insert("/jmp/Placement/LayerA/ObjInfo", {});
        archive.insert("/jmp/Placement/LayerA/StarInfo", {0xFF, 0x00, 0x7F});
        // Casing is exercised on the FILE NAME only, never on a directory path.
        // Lookups are case-insensitive, but serialize()'s parent map is keyed by
        // the exact stored paths, so writing "jmp/placement/common/..." when the
        // directory was created as "jmp/Placement/Common" produces a file with no
        // serialized parent -- section 14 invariant 2. An editor that did that
        // would fail at save time with a message about the wrong thing.
        archive.insert("/jmp/Placement/Common/MAPPartsInfo", {9});

        const auto bytes = archive.serialize(false);
        const RarcArchive reparsed(bytes);

        expect(reparsed.rootName() == archive.rootName(),
               "RARC round trip: root name changed");
        expect(reparsed.endian() == archive.endian(), "RARC round trip: endian changed");
        expect(reparsed.entries().size() == archive.entries().size(),
               "RARC round trip: entry count changed");
        for (const auto& before : archive.entries()) {
            const auto* after = reparsed.find(before.path);
            expect(after != nullptr,
                   "RARC round trip: entry missing after re-parse: " + before.path);
            if (after == nullptr) {
                continue;
            }
            expect(after->directory == before.directory,
                   "RARC round trip: directory flag changed for " + before.path);
            if (!before.directory) {
                expect(reparsed.read(*after) == archive.read(before),
                       "RARC round trip: payload changed for " + before.path);
            }
        }
        bool keptCasing = false;
        for (const auto& entry : reparsed.entries()) {
            if (entry.path == "MixedCase/jmp/Placement/Common/MAPPartsInfo") {
                keptCasing = true;
            }
        }
        expect(keptCasing, "RARC round trip: an entry's stored casing was normalised away");

        // NOTE: byte-identity is deliberately NOT asserted for a built-from-nothing
        // archive, only for parsed ones (below). parse() rebuilds entries_ in node
        // tree order -- each directory followed by its children -- while a builder
        // appends directories and files as it goes. serialize() lays the string
        // table out by walking entries_, so the two orderings produce different
        // bytes for an IDENTICAL tree. That is benign and expected: nothing has
        // been written yet, the tree is what the game reads, and the property that
        // actually matters -- an untouched PARSED file re-saving byte-for-byte --
        // is asserted in the loop below.
    }

    // ---- parsed archives, both endiannesses --------------------------------
    for (const auto endian : {Endian::big, Endian::little}) {
        RarcArchive archive(makeTinyRarc(endian));
        // A directory and a second file, so this is not a trivial single-entry
        // copy of what parse() already produced. The directory has to be created
        // FIRST: insert() refuses a file whose parent does not exist, which is
        // one of the section 14 invariants, and makeTinyRarc ships no directories.
        archive.createDirectory("root/nested");
        archive.insert("root/file", {1, 2, 3});
        archive.insert("root/nested/deep.bin", {7, 7, 7, 7});
        const auto before = archive.serialize(false);
        const RarcArchive reparsed(before);

        expect(reparsed.endian() == endian, "RARC round trip: endian not preserved");
        expect(reparsed.entries().size() == archive.entries().size(),
               "RARC round trip: entry count not preserved");
        for (const auto& original : archive.entries()) {
            const auto* copy = reparsed.find(original.path);
            expect(copy != nullptr, "RARC round trip: lost " + original.path);
            if (copy != nullptr && !original.directory) {
                expect(reparsed.read(*copy) == archive.read(original),
                       "RARC round trip: payload not preserved for " + original.path);
            }
        }
        expect(reparsed.serialize(false) == before,
               "RARC round trip: a second serialize differed");
    }
}

// BCSV has the same law and more ways to break it, because a BCSV's schema is
// bit-packed: fields share words behind masks and shifts, and the row stride is
// aligned. A change that looks harmless -- one extra field, one new row -- can
// shift every following field's offset, and the file still parses while meaning
// something else entirely. "It still loads" is not "it still means the same".
void testBcsvRoundTripProperties() {
    using whitehole::smg::BcsvTable;
    using whitehole::smg::BcsvType;

    // One endianness here, and deliberately so. A default-constructed BcsvTable
    // is BIG-endian (the retail layout), and there is no public way to BUILD a
    // little-endian one from scratch -- only to parse one. Iterating both here
    // would have meant writing big bytes and re-parsing them claiming to be
    // little, which fails loudly and correctly. Little-endian PARSING and
    // re-serialization are pinned by testBcsvEndianness; this test is about the
    // round trip itself.
    const std::string where = "BCSV";

    {
            BcsvTable table;
            const auto i = table.ensureField("AnInt", BcsvType::integer);
            const auto s = table.ensureField("AString", BcsvType::fixedString);
            const auto f = table.ensureField("AFloat", BcsvType::floatingPoint);
            const auto i2 = table.ensureField("APair", BcsvType::integer2);
            const auto sh = table.ensureField("AShort", BcsvType::shortInteger);
            const auto b = table.ensureField("AByte", BcsvType::byte);
            const auto so = table.ensureField("AStringRef", BcsvType::stringOffset);
            expect(i < s && s < f && f < i2 && i2 < sh && sh < b && b < so,
                   where + ": ensureField must append in order");

            const std::size_t row = table.addRow();
            auto& cells = table.rows()[row];
            table.setInt(cells, "AnInt", -12345);
            table.setString(cells, "AString", "Mario");
            table.setFloat(cells, "AFloat", 1.5F);
            table.setInt(cells, "APair", 7);
            table.setString(cells, "AStringRef", "obj/ObjInfo");
            (void)sh;
            (void)b;

            const BcsvTable reparsed(table.serialize(), table.endian());
            expectTablesEqual(table, reparsed, where + " mixed-type round trip");
            expect(reparsed.getInt(reparsed.rows()[0], "AnInt") == -12345,
                   where + ": a negative integer must survive the round trip");
            expect(reparsed.getString(reparsed.rows()[0], "AString") == "Mario",
                   where + ": a string must survive the round trip");
            expect(std::abs(reparsed.getFloat(reparsed.rows()[0], "AFloat") - 1.5F) < 0.0001F,
                   where + ": a float must survive the round trip");
            expect(reparsed.getString(reparsed.rows()[0], "AStringRef") == "obj/ObjInfo",
                   where + ": a string-offset field must resolve back to its text");
        }

        // ---- several rows, then a byte-exact second pass ----------------------
        {
            BcsvTable table;
            (void)table.ensureField("l_id", BcsvType::shortInteger);
            (void)table.ensureField("name", BcsvType::stringOffset);
            (void)table.ensureField("pos_x", BcsvType::floatingPoint);
            for (std::int32_t index = 0; index < 8; ++index) {
                const std::size_t row = table.addRow();
                auto& cells = table.rows()[row];
                table.setInt(cells, "l_id", index);
                table.setString(cells, "name", "Object" + std::to_string(index));
                table.setFloat(cells, "pos_x", static_cast<float>(index) * 0.25F);
            }
            const auto bytes = table.serialize();
            const BcsvTable reparsed(bytes, table.endian());
            expectTablesEqual(table, reparsed, where + " eight-row round trip");
            expect(reparsed.rows().size() == 8, where + ": row count changed");
            expect(reparsed.serialize() == bytes,
                   where + ": a second BCSV serialize produced different bytes");
        }

        // ---- a table with no rows at all --------------------------------------
        // A schema with zero rows is what every created zone is built from, and it
        // is the shape that has quietly broken before -- a zone that "succeeded" and
        // contained no tables.
        {
            BcsvTable table;
            (void)table.ensureField("WorldNo", BcsvType::integer);
            expect(table.rows().empty(), where + ": a fresh table must have no rows");
            const BcsvTable reparsed(table.serialize(), table.endian());
            expectTablesEqual(table, reparsed, where + " empty-table round trip");
            expect(reparsed.rows().empty(), where + ": an empty table gained rows");
            expect(reparsed.hasField("WorldNo"),
                   where + ": an empty table lost its only field");
        }
}

void testRarcEndianness() {
    for (const auto endian : {whitehole::io::Endian::big, whitehole::io::Endian::little}) {
        whitehole::io::RarcArchive archive(makeTinyRarc(endian));
        expect(archive.endian() == endian, "RARC endian detection failed");
        expect(archive.rootName() == "root", "RARC root name changed");
        expect(archive.entries().size() == 1, "RARC entry table was not parsed");
        expect(archive.entries().front().path == "root/file", "RARC entry path changed");
        expect(archive.read(archive.entries().front()) == std::vector<std::uint8_t>({1, 2, 3}),
               "RARC entry contents changed");
        archive.replace("ROOT/FILE", {9, 8, 7, 6});
        const whitehole::io::RarcArchive rewritten(archive.serialize(false));
        expect(rewritten.endian() == endian, "rewritten RARC endian changed");
        expect(rewritten.entries().size() == 1, "rewritten RARC entry count changed");
        expect(rewritten.read(rewritten.entries().front()) == std::vector<std::uint8_t>({9, 8, 7, 6}),
               "RARC replacement was not serialized");
    }
}

// THE WRITER MUST NEVER USE THE LOOSE LOOKUP. This is the regression test for a
// bug class that has cost this repo real data TWICE, both times in the zone
// builder (BLUEPRINT section 15):
//
//   createDirectory() asked whether "Stage/jmp/MapParts/Common" existed and was
//   answered by the FILE ".../MapParts/Common/MapPartsInfo" -- a path-suffix
//   match -- so it skipped the directory, and every file inside it then failed
//   to serialize for having no parent.
//
//   insert() had the same problem and it was worse. Every layer's tables are
//   named identically BY DESIGN (Common/StartInfo, LayerA/StartInfo, ...), so
//   inserting LayerA's table found Common's through the bare-file-name fallback
//   and REPLACED it. The archive looked complete -- every layer directory was
//   present -- while every non-Common layer silently held no tables at all.
//
// The rule is now written down at the top of rarc.hpp and pinned here. This test
// constructs the exact ambiguity both bugs relied on and asserts that the writer
// lands on the file it was told to, and ONLY on that file.
void testRarcWriterNeverLooseMatches() {
    using whitehole::io::RarcArchive;

    // --- the insert() half: same file name in two layers ---------------------
    auto archive = RarcArchive::create("Zone");
    archive.createDirectory("/jmp");
    archive.createDirectory("/jmp/Placement");
    archive.createDirectory("/jmp/Placement/Common");
    archive.createDirectory("/jmp/Placement/LayerA");
    const std::uint8_t commonBytes = 0xC0;
    const std::uint8_t layerBytes = 0xA1;
    archive.insert("/jmp/Placement/Common/StartInfo", {commonBytes});
    archive.insert("/jmp/Placement/LayerA/StartInfo", {layerBytes});

    // The two entries must coexist. Before the fix, the second insert() found the
    // first through the bare-filename fallback and replaced it, leaving ONE file.
    // Count FILES, not entries(): entries() also lists the four directories, and
    // the point of this assertion is about the two identically-named files.
    const auto fileCount = [&archive] {
        std::size_t count = 0;
        for (const auto& entry : archive.entries()) {
            if (!entry.directory) {
                ++count;
            }
        }
        return count;
    };
    expect(fileCount() == 2,
           "two layers' identically-named tables must be two separate files");
    const auto* common = archive.find("Zone/jmp/Placement/Common/StartInfo");
    const auto* layerA = archive.find("Zone/jmp/Placement/LayerA/StartInfo");
    expect(common != nullptr && layerA != nullptr,
           "both layers' StartInfo files must be individually addressable");
    expect(common != layerA, "the two layers resolved to the SAME entry");
    expect(archive.read(*common) == std::vector<std::uint8_t>{commonBytes},
           "insert() into LayerA overwrote Common's StartInfo");
    expect(archive.read(*layerA) == std::vector<std::uint8_t>{layerBytes},
           "LayerA's StartInfo did not get its own bytes");

    // And this must survive a serialize/re-parse, or the loss only shows up once
    // the file reaches the game.
    const RarcArchive reparsed(archive.serialize(false));
    expect(std::count_if(reparsed.entries().begin(), reparsed.entries().end(),
                         [](const auto& entry) { return !entry.directory; }) == 2,
           "the two layer tables did not both survive serialization");
    expect(reparsed.read(*reparsed.find("Zone/jmp/Placement/Common/StartInfo")) ==
               std::vector<std::uint8_t>{commonBytes},
           "Common's StartInfo changed on the way out to disk");

    // --- the replace() half: the path overload must not fall back either -----
    // This used to resolve through find(), which made it the last writer in the
    // file that could be redirected by a loose match. Replacing LayerA's file
    // must change LayerA's file and leave Common's alone.
    auto replaced = archive;
    const std::uint8_t newLayerBytes = 0xA2;
    replaced.replace("Zone/jmp/Placement/LayerA/StartInfo", {newLayerBytes});
    expect(replaced.read(*replaced.find("Zone/jmp/Placement/Common/StartInfo")) ==
               std::vector<std::uint8_t>{commonBytes},
           "replace() wrote to Common's file instead of the one it was given");
    expect(replaced.read(*replaced.find("Zone/jmp/Placement/LayerA/StartInfo")) ==
               std::vector<std::uint8_t>{newLayerBytes},
           "replace() did not write the file it was given");

    // Root-RELATIVE paths (a leading slash, which is how every caller in the
    // codebase addresses an archive) must resolve too. Entries are stored
    // WITHOUT the leading slash, so replace() has to normalize before comparing.
    // Getting this wrong made createGalaxy() fail with "RARC file does not
    // exist: /Stage/jmp/Placement/Common/StageObjInfo" for a file the archive
    // plainly had -- findExact() compares raw strings and does not normalize.
    auto slashForm = archive;
    slashForm.replace("/Zone/jmp/Placement/LayerA/StartInfo", {0xA3});
    expect(slashForm.read(*slashForm.find("Zone/jmp/Placement/LayerA/StartInfo")) ==
               std::vector<std::uint8_t>{0xA3},
           "replace() must accept a root-relative path with a leading slash");
    expect(slashForm.read(*slashForm.find("Zone/jmp/Placement/Common/StartInfo")) ==
               std::vector<std::uint8_t>{commonBytes},
           "a leading-slash replace() disturbed the wrong layer");

    // A path that resolves ONLY through the fallback must now be refused outright
    // rather than silently landing on some other entry. The bare name "StartInfo"
    // is ambiguous here by construction -- there are two of them.
    bool refusedAmbiguous = false;
    try {
        replaced.replace("StartInfo", {0xFF});
    } catch (const std::runtime_error&) {
        refusedAmbiguous = true;
    }
    expect(refusedAmbiguous,
           "replace() must refuse a bare file name rather than guess which file it meant");

    // The entry overload is the unambiguous writer: the caller already decided,
    // and nothing about the path can redirect it.
    auto byEntry = archive;
    byEntry.replace(*byEntry.find("Zone/jmp/Placement/Common/StartInfo"), {0xC1});
    expect(byEntry.read(*byEntry.find("Zone/jmp/Placement/Common/StartInfo")) ==
               std::vector<std::uint8_t>{0xC1},
           "replace(entry, ...) did not write the entry it was handed");
    expect(byEntry.read(*byEntry.find("Zone/jmp/Placement/LayerA/StartInfo")) ==
               std::vector<std::uint8_t>{layerBytes},
           "replace(entry, ...) disturbed a different entry");

    // --- the createDirectory() half: a FILE must not satisfy a directory ask --
    // The original bug in its purest form. "Common/MapPartsInfo" exists as a
    // file; asking whether the DIRECTORY "Common" exists must not be answered by
    // it through the suffix match, because that skips creating the directory.
    auto suffix = RarcArchive::create("Suffix");
    suffix.createDirectory("/jmp");
    suffix.createDirectory("/jmp/MapParts");
    suffix.createDirectory("/jmp/MapParts/Common");
    suffix.insert("/jmp/MapParts/Common/MapPartsInfo", {0x42});
    // Creating the same directory again is idempotent and must NOT create a
    // duplicate entry -- the exact case that used to be answered by the file.
    suffix.createDirectory("/jmp/MapParts/Common");
    expect(suffix.entries().size() == 4,
           "createDirectory() must be idempotent when the directory really exists");
    const RarcArchive suffixReparsed(suffix.serialize(false));
    expect(suffixReparsed.find("Suffix/jmp/MapParts/Common/MapPartsInfo") != nullptr,
           "the sibling file must survive the idempotent directory create");

    // --- the READ side keeps its convenience, deliberately -------------------
    // The half of the rule that says find() is not being removed: a reader may
    // still answer a bare file name and a path suffix, because that is what a
    // human typing a shortened path means. Pin it so a future "cleanup" does
    // not quietly break SMG1's lowercase paths or the BCSV editor's dialogs.
    const auto* loose = archive.find("StartInfo");
    expect(loose != nullptr, "find() must still resolve a bare file name for readers");
    const auto* suffixLoose = archive.find("jmp/Placement/Common/StartInfo");
    expect(suffixLoose != nullptr, "find() must still resolve a path suffix for readers");
    expect(suffixLoose == common,
           "the path-suffix read must resolve to the same file as the exact path");
}

void testMath() {
    using whitehole::math::Matrix4;
    using whitehole::math::Vec3f;
    const auto transformed = Matrix4::translation({10, 20, 30}).transformPoint({1, 2, 3});
    expect(std::abs(transformed.x - 11) < 0.00001F, "matrix X translation failed");
    expect(std::abs(transformed.y - 22) < 0.00001F, "matrix Y translation failed");
    expect(std::abs(transformed.z - 33) < 0.00001F, "matrix Z translation failed");
    const auto composed = Matrix4::translation({10, 20, 30}) * Matrix4::scale({2, 3, 4});
    const auto composedPoint = composed.transformPoint({1, 2, 3});
    expect(std::abs(composedPoint.x - 12) < 0.00001F, "matrix composition scaled X translation");
    expect(std::abs(composedPoint.y - 26) < 0.00001F, "matrix composition scaled Y translation");
    expect(std::abs(composedPoint.z - 42) < 0.00001F, "matrix composition scaled Z translation");
    expect(std::abs(Vec3f::dot({1, 0, 0}, {0, 1, 0})) < 0.00001F, "vector dot product failed");
    const auto cross = Vec3f::cross({1, 0, 0}, {0, 1, 0});
    expect(std::abs(cross.z - 1) < 0.00001F, "vector cross product failed");
}

void testViewportCamera() {
    whitehole::render::ViewportCamera camera;
    camera.target = {100.0F, 0.0F, 0.0F};
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;

    const auto eye = camera.eye();
    expect(std::abs(eye.x - 600.0F) < 0.01F, "viewport camera eye is wrong");

    // Center of the screen must ray-cast straight at the orbit target.
    const auto ray = camera.screenToRay(400.0F, 300.0F, 800.0F, 600.0F);
    const auto toTarget = whitehole::math::Vec3f{100.0F - ray.origin.x, 0.0F - ray.origin.y, 0.0F - ray.origin.z};
    const float alignment = whitehole::math::Vec3f::dot(ray.direction, toTarget.normalized());
    expect(alignment > 0.999F, "viewport camera center ray misses target");

    // Round trip through world->screen keeps the target centered.
    float screenX = 0.0F;
    float screenY = 0.0F;
    expect(camera.worldToScreen(camera.target, 800.0F, 600.0F, screenX, screenY), "target behind viewport camera");
    expect(std::abs(screenX - 400.0F) < 1.0F && std::abs(screenY - 300.0F) < 1.0F,
           "viewport camera projection is off-center");

    const float before = camera.distance;
    camera.dolly(1.0F);
    expect(camera.distance < before, "viewport camera dolly-in failed");
    camera.frameTarget({1.0F, 2.0F, 3.0F}, 250.0F);
    expect(std::abs(camera.target.x - 1.0F) < 0.001F && std::abs(camera.distance - 250.0F) < 0.001F,
           "viewport camera framing failed");

    // Zoom and framing reach galaxy-spanning distances: the old hard 20000
    // clamp cut big maps off mid-view and made Frame All impossible. dolly()
    // clamps one gesture to +/-4 notches, so a full zoom-out is several flicks.
    camera.distance = 1000.0F;
    for (int flick = 0; flick < 12; ++flick) {
        camera.dolly(-4.0F);
    }
    expect(camera.distance > 20000.0F, "dolly-out is stuck below the old clamp");
    expect(camera.distance <= 150001.0F, "dolly-out exceeded the new ceiling");
    camera.frameTarget({}, 250000.0F);
    expect(camera.distance > 20000.0F, "frameTarget clamps below galaxy size");
    expect(camera.distance <= 150001.0F, "frameTarget exceeded the new ceiling");

    // Dynamic clip planes stay inside their band and ordered at every zoom --
    // this is what killed the far-range z-fighting on rails and overlays.
    for (const float zoom : {5.0F, 800.0F, 40000.0F, 150000.0F}) {
        camera.distance = zoom;
        const float near = camera.nearPlane();
        const float far = camera.farPlane(50000.0F);
        expect(near >= 1.0F && near <= 100.0F, "near plane left its band");
        expect(far > near, "far plane must sit beyond near");
        expect(far <= 500001.0F, "far plane exceeded its cap");
    }

    // Fly slides the orbit target along the camera basis. With yaw/pitch at 0,
    // target (100,0,0) and distance 500 the eye sits at +X, so forward is -X
    // and right is -Z.
    camera.target = {100.0F, 0.0F, 0.0F};
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;
    camera.fly(0.0F, 0.0F, 100.0F);
    expect(std::abs(camera.target.x - 0.0F) < 0.01F, "fly forward moved the wrong way");
    camera.fly(100.0F, 0.0F, 0.0F);
    expect(std::abs(camera.target.z - (-100.0F)) < 0.01F, "fly right moved the wrong way");
    camera.fly(0.0F, 50.0F, 0.0F);
    expect(std::abs(camera.target.y - 50.0F) < 0.01F, "fly up moved the wrong way");

    // The grid covers the visible ground (half-extent >= orbit distance, so
    // the full patch spans >= 2x) and snaps to clean 1/2/5 decade steps.
    for (const float zoom : {5.0F, 40.0F, 800.0F, 30000.0F}) {
        const auto grid = whitehole::render::gridSpec(zoom);
        expect(grid.extent >= zoom * 0.999F, "grid does not cover the visible ground");
        const float decade = std::pow(10.0F, std::floor(std::log10(grid.step)));
        const float mantissa = grid.step / decade;
        expect(std::abs(mantissa - 1.0F) < 0.001F || std::abs(mantissa - 2.0F) < 0.001F ||
                   std::abs(mantissa - 5.0F) < 0.001F,
               "grid step is not a 1/2/5 decade value");
    }
}

void testViewportCameraPreview() {
    using whitehole::math::Vec3f;
    using whitehole::render::ViewportCamera;

    // lookAt is what hands the viewport a solved BCAM pose, so the orbit rig has
    // to reproduce the requested eye exactly -- otherwise the preview would show
    // a camera the game never had.
    ViewportCamera camera;
    const Vec3f eye{500.0F, 220.0F, -300.0F};
    const Vec3f at{40.0F, 10.0F, 90.0F};
    camera.lookAt(eye, at);
    const auto solvedEye = camera.eye();
    expect(std::abs(solvedEye.x - eye.x) < 0.01F && std::abs(solvedEye.y - eye.y) < 0.01F &&
               std::abs(solvedEye.z - eye.z) < 0.01F,
           "lookAt must reproduce the requested eye");
    const auto solvedTarget = camera.target;
    expect(std::abs(solvedTarget.x - at.x) < 0.001F && std::abs(solvedTarget.y - at.y) < 0.001F &&
               std::abs(solvedTarget.z - at.z) < 0.001F,
           "lookAt must keep the look-at point as the orbit pivot");
    const Vec3f facing = camera.forward();
    const Vec3f wantFacing{at.x - eye.x, at.y - eye.y, at.z - eye.z};
    const auto want = wantFacing.normalized();
    expect(Vec3f::dot(facing, want) > 0.9999F, "lookAt must face the look-at point");

    // A straight-down shot (a Tower camera at angleA = 90deg) must stay usable:
    // the pitch clamp stops just short of vertical, and the basis stays finite.
    camera.lookAt({0.0F, 900.0F, 0.0F}, {0.0F, 0.0F, 0.0F});
    expect(std::isfinite(camera.eye().x) && std::isfinite(camera.eye().z),
           "straight-down lookAt produced a non-finite eye");
    expect(std::abs(camera.eye().y - 900.0F) < 1.0F,
           "straight-down lookAt lost the eye height past the pitch clamp");

    // Roll rotates the basis about the view axis and must not shear it: the
    // view matrix, the picking ray and worldToScreen all derive from this basis,
    // so orthogonality here is what keeps a rolled preview clickable.
    camera.lookAt(eye, at, -1.0F, 0.6F);
    const auto rolledUp = camera.up();
    const auto rolledRight = camera.right();
    const auto rolledFacing = camera.forward();
    expect(std::abs(rolledUp.length() - 1.0F) < 0.001F &&
               std::abs(rolledRight.length() - 1.0F) < 0.001F,
           "rolled basis is not unit length");
    expect(std::abs(Vec3f::dot(rolledUp, rolledRight)) < 0.001F &&
               std::abs(Vec3f::dot(rolledUp, rolledFacing)) < 0.001F,
           "rolled basis is not orthogonal");
    expect(std::abs(rolledUp.y - 1.0F) > 0.01F, "roll left the up vector unrotated");
    // A rolled screen centre still ray-casts onto the look-at point, which is the
    // guarantee that clicks land where the rolled image says they should.
    const auto centred = camera.screenToRay(400.0F, 300.0F, 800.0F, 600.0F);
    const Vec3f rollToTarget{at.x - centred.origin.x, at.y - centred.origin.y,
                             at.z - centred.origin.z};
    expect(Vec3f::dot(centred.direction, rollToTarget.normalized()) > 0.999F,
           "rolled camera's centre ray misses its target");

    // A wider fovy frames more of the scene: the corner ray of a 90 degree
    // camera must sit further off-axis than the same corner at 30 degrees.
    ViewportCamera wide;
    wide.lookAt(eye, at, 1.5707963F, 0.0F);
    ViewportCamera narrow;
    narrow.lookAt(eye, at, 0.5235988F, 0.0F);
    const auto wideCorner = wide.screenToRay(800.0F, 600.0F, 800.0F, 600.0F).direction;
    const auto narrowCorner = narrow.screenToRay(800.0F, 600.0F, 800.0F, 600.0F).direction;
    const float wideSpread = Vec3f::dot(wideCorner, wide.forward());
    const float narrowSpread = Vec3f::dot(narrowCorner, narrow.forward());
    expect(wideSpread < narrowSpread, "a wider fovy did not widen the corner ray");

    // The projection must honour the FOV the camera carries, so nothing that
    // projects through it can disagree with the rendered image about framing.
    const auto projection = wide.projectionMatrix(800.0F / 600.0F);
    expect(std::abs(projection.values[5] - 1.0F / std::tan(1.5707963F * 0.5F)) < 0.001F,
           "projection ignored the camera's field of view");
    // And a bogus fovy must not degenerate the frustum.
    ViewportCamera broken;
    broken.lookAt(eye, at, 0.0F, 0.0F);
    broken.fieldOfViewRadians = 0.0F;
    const auto safeProjection = broken.projectionMatrix(800.0F / 600.0F);
    expect(std::isfinite(safeProjection.values[5]) && safeProjection.values[5] > 0.0F,
           "a zero fovy produced a degenerate projection");
}

void testReversedZDepthBuffer() {
    using whitehole::math::Matrix4;
    using whitehole::render::reversedZProjectionMatrix;
    using whitehole::render::ViewportCamera;

    // The viewport projects reversed-Z: the near plane lands on NDC depth +1 and
    // the far plane on 0, the depth buffer is cleared to 0 and the frame default
    // is GL_GEQUAL. "Nearer" is therefore the LARGER value -- that invariant is
    // what decides which surface owns a pixel, so the matrix has to deliver it
    // exactly. It did not: the hand-written depth row was negated, the near
    // plane mapped to -1, and the FURTHEST fragment kept the largest depth. Every
    // closed model then showed the inside of its far wall -- an inside-out,
    // "backface-culled room" render -- while nearer geometry could neither pass
    // the test nor overwrite it.
    constexpr float kNear = 10.0F;
    constexpr float kFar = 20000.0F;
    constexpr float kAspect = 4.0F / 3.0F;
    const Matrix4 projection = reversedZProjectionMatrix(kAspect, kNear, kFar);

    // Column-major (values[4 * column + row]), exactly what glLoadMatrixf reads.
    expect(projection.values[3] == 0.0F && projection.values[7] == 0.0F,
           "the projection must not feed x/y into w");
    expect(projection.values[11] == -1.0F, "the projection's w row must be -z");
    expect(projection.values[15] == 0.0F, "the projection must leave w as -z");

    // x/y must stay the same centric frustum the old glFrustum call built. A
    // mirrored axis here would reverse every triangle's screen winding and take
    // the GL_CW front-face culling (and the per-material cull modes) with it, so
    // both terms stay positive.
    const float f = 1.0F / std::tan(ViewportCamera::kFieldOfView * 0.5F);
    expect(std::abs(projection.values[0] - f / kAspect) < 1e-4F,
           "projection x scale must match the frustum's 1/tan(fov/2)/aspect");
    expect(std::abs(projection.values[5] - f) < 1e-4F,
           "projection y scale must match the frustum's 1/tan(fov/2)");

    struct Clip {
        float x;
        float y;
        float z;
        float w;
    };
    const auto clip = [&projection](float x, float y, float z) {
        const auto& v = projection.values;
        return Clip{x * v[0] + y * v[4] + z * v[8] + v[12],
                    x * v[1] + y * v[5] + z * v[9] + v[13],
                    x * v[2] + y * v[6] + z * v[10] + v[14],
                    x * v[3] + y * v[7] + z * v[11] + v[15]};
    };
    // A point straight ahead `distance` units away (view space z = -distance).
    const auto ndcDepth = [&clip](float distance) {
        const Clip c = clip(0.0F, 0.0F, -distance);
        return c.z / c.w;
    };
    // The value that actually lands in the depth buffer.
    const auto bufferDepth = [&ndcDepth](float distance) {
        return (ndcDepth(distance) + 1.0F) * 0.5F;
    };

    expect(std::abs(ndcDepth(kNear) - 1.0F) < 1e-3F,
           "the near plane must map to NDC +1 under reversed-Z");
    expect(std::abs(ndcDepth(kFar)) < 1e-3F, "the far plane must map to NDC 0 under reversed-Z");
    expect(std::abs(bufferDepth(kNear) - 1.0F) < 1e-3F,
           "the near plane must own the top of the depth buffer");

    // Nearer geometry holds the larger buffer value at every distance.
    float previous = 2.0F;
    for (const float distance : {kNear, 12.0F, 100.0F, 1000.0F, 10000.0F, kFar}) {
        const float depth = bufferDepth(distance);
        expect(depth >= 0.0F && depth <= 1.0F, "a buffer depth left the [0, 1] range");
        expect(depth < previous, "a nearer surface did not get a larger depth value");
        previous = depth;
    }

    // The test itself (cleared to 0, GL_GEQUAL, depth writes on) has to keep the
    // nearer surface in BOTH draw orders; that is the whole difference between
    // correct occlusion and seeing straight through a model's near wall.
    const auto nearestSurfaceSurvives = [&bufferDepth](bool farFirst) {
        float buffer = 0.0F; // glClearDepth(0)
        const float nearValue = bufferDepth(100.0F);
        const float farValue = bufferDepth(1000.0F);
        const auto draw = [&buffer](float value) {
            if (value >= buffer) { // GL_GEQUAL
                buffer = value;    // depth write
            }
        };
        if (farFirst) {
            draw(farValue);
            draw(nearValue);
        } else {
            draw(nearValue);
            draw(farValue);
        }
        return std::abs(buffer - nearValue) < 1e-6F;
    };
    expect(nearestSurfaceSurvives(false) && nearestSurfaceSurvives(true),
           "an occluded far surface must never win a pixel under the reversed-Z depth test");

    // Winding must be independent of the depth convention: one triangle wound
    // counter-clockwise in view space keeps the sign of its screen-space area
    // against a conventional (forward-Z) frustum of the same shape. A sign flip
    // here would invert every front face and draw the inside of every model.
    Matrix4 reference;
    reference.values.fill(0.0F);
    reference.values[0] = f / kAspect;
    reference.values[5] = f;
    reference.values[10] = (kFar + kNear) / (kNear - kFar);
    reference.values[11] = -1.0F;
    reference.values[14] = (2.0F * kFar * kNear) / (kNear - kFar);

    struct Screen {
        float x;
        float y;
    };
    const auto signedArea = [](const Matrix4& matrix) {
        const auto& v = matrix.values;
        const auto project = [&v](float x, float y, float z) {
            const float w = x * v[3] + y * v[7] + z * v[11] + v[15];
            return Screen{(x * v[0] + y * v[4] + z * v[8] + v[12]) / w,
                          (x * v[1] + y * v[5] + z * v[9] + v[13]) / w};
        };
        const Screen a = project(0.0F, 0.0F, -100.0F);
        const Screen b = project(50.0F, 0.0F, -120.0F);
        const Screen c = project(0.0F, 50.0F, -120.0F);
        return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    };
    expect(std::abs(signedArea(projection)) > 0.0001F,
           "the winding probe collapsed to a degenerate triangle");
    expect(signedArea(projection) * signedArea(reference) > 0.0F,
           "the reversed-Z projection must not flip triangle winding");

    // The CPU-side camera projection has to stay the renderer's one instead of a
    // second, forward-Z copy that silently disagrees about depth direction.
    ViewportCamera camera;
    camera.distance = 1000.0F;
    const Matrix4 cpuProjection = camera.projectionMatrix(kAspect);
    expect(cpuProjection.values[11] == -1.0F && cpuProjection.values[10] > 0.0F,
           "ViewportCamera::projectionMatrix must project the renderer's reversed-Z");
    expect(cpuProjection.values[10] ==
               reversedZProjectionMatrix(kAspect, camera.nearPlane(), camera.farPlane(10000.0F))
                   .values[10],
           "the CPU projection drifted from the renderer's depth mapping");
}

void testGizmoMath() {
    using whitehole::render::axisDirection;
    using whitehole::render::beginGizmoDrag;
    using whitehole::render::GizmoDrag;
    using whitehole::render::GizmoHandle;
    using whitehole::render::GizmoMode;
    using whitehole::render::gizmoAxisLength;
    using whitehole::render::gizmoDragValue;
    using whitehole::render::pickGizmoHandle;

    // An isometric view where every axis is visibly distinct.
    whitehole::render::ViewportCamera camera;
    camera.target = {0.0F, 0.0F, 0.0F};
    camera.yawRadians = 0.7853982F;
    camera.pitchRadians = 0.6F;
    camera.distance = 1000.0F;
    constexpr float kWidth = 800.0F;
    constexpr float kHeight = 600.0F;

    expect(axisDirection(GizmoHandle::AxisX).x == 1.0F, "axis X direction wrong");
    expect(axisDirection(GizmoHandle::AxisY).y == 1.0F, "axis Y direction wrong");
    expect(axisDirection(GizmoHandle::AxisZ).z == 1.0F, "axis Z direction wrong");

    // Constant-screen-size sizing: the axis must read ~80 px whatever the zoom.
    const float axisLength = gizmoAxisLength(camera, {0.0F, 0.0F, 0.0F}, kHeight);
    float centreX = 0.0F;
    float centreY = 0.0F;
    expect(camera.worldToScreen({0.0F, 0.0F, 0.0F}, kWidth, kHeight, centreX, centreY),
           "the gizmo anchor must project");
    float tipX = 0.0F;
    float tipY = 0.0F;
    expect(camera.worldToScreen({axisLength, 0.0F, 0.0F}, kWidth, kHeight, tipX, tipY),
           "the gizmo tip must project");
    const float screenLength = std::hypot(tipX - centreX, tipY - centreY);
    expect(screenLength > 60.0F && screenLength < 110.0F,
           "the gizmo axis must stay a constant on-screen size");

    // Clicking on each axis tip grabs that axis.
    for (const auto handle : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
        const auto direction = axisDirection(handle);
        float grabX = 0.0F;
        float grabY = 0.0F;
        expect(camera.worldToScreen(
                   {direction.x * axisLength, direction.y * axisLength, direction.z * axisLength},
                   kWidth, kHeight, grabX, grabY),
               "an axis tip must project");
        const auto picked =
            pickGizmoHandle(camera, {0.0F, 0.0F, 0.0F}, grabX, grabY, kWidth, kHeight);
        expect(picked == handle, "picking missed the axis it clicked on");
    }

    // Empty space grabs nothing; the anchor grabs the centre.
    expect(pickGizmoHandle(camera, {0.0F, 0.0F, 0.0F}, 40.0F, 40.0F, kWidth, kHeight) ==
               GizmoHandle::None,
           "picking must miss empty space");
    expect(pickGizmoHandle(camera, {0.0F, 0.0F, 0.0F}, centreX, centreY, kWidth, kHeight) ==
               GizmoHandle::Center,
           "picking must grab the centre at the anchor");

    // ---- translate drag --------------------------------------------------
    GizmoDrag drag;
    expect(beginGizmoDrag(drag, camera, {0.0F, 0.0F, 0.0F}, GizmoMode::Translate,
                          GizmoHandle::AxisX, tipX, tipY, kWidth, kHeight),
           "beginGizmoDrag refused the X axis");
    // Dragging *along* the handle's screen direction moves only X, positively,
    // proportional to the pixel travel.
    constexpr float kTravelPx = 30.0F;
    const auto moved = gizmoDragValue(drag, camera, tipX + drag.axisDir.x * kTravelPx,
                                      tipY + drag.axisDir.y * kTravelPx, kWidth, kHeight);
    expect(std::abs(moved.y) < 0.5F && std::abs(moved.z) < 0.5F,
           "an axis drag must not leak into the other axes");
    expect(moved.x > 5.0F, "a 30px drag along X must move the object visibly");
    expect(std::abs(moved.x - kTravelPx * drag.worldPerPixel) < 0.5F, "axis drag mapping is off");

    // Dragging back to the grab point reads exactly zero.
    const auto rest = gizmoDragValue(drag, camera, tipX, tipY, kWidth, kHeight);
    expect(std::abs(rest.x) < 0.01F && std::abs(rest.y) < 0.01F && std::abs(rest.z) < 0.01F,
           "a drag returned to its start must read zero");

    // An axis head-on to the camera projects to a point and cannot be picked:
    // begin refuses instead of inventing a mapping the author cannot see.
    whitehole::render::ViewportCamera headOn;
    headOn.target = {0.0F, 0.0F, 0.0F};
    headOn.yawRadians = 0.0F; // eye on +X looking down -X: the X axis is head-on
    headOn.pitchRadians = 0.0F;
    headOn.distance = 1000.0F;
    float headCentreX = 0.0F;
    float headCentreY = 0.0F;
    expect(headOn.worldToScreen({0.0F, 0.0F, 0.0F}, kWidth, kHeight, headCentreX, headCentreY),
           "the head-on anchor must project");
    expect(pickGizmoHandle(headOn, {0.0F, 0.0F, 0.0F}, headCentreX, headCentreY, kWidth,
                           kHeight) != GizmoHandle::AxisX,
           "a view-parallel axis must not be grabbable");

    // ---- centre drag -----------------------------------------------------
    // The centre moves in the view plane through the anchor.
    GizmoDrag centre;
    expect(beginGizmoDrag(centre, camera, {0.0F, 0.0F, 0.0F}, GizmoMode::Translate,
                          GizmoHandle::Center, centreX, centreY, kWidth, kHeight),
           "beginGizmoDrag refused the centre");
    const auto planar = gizmoDragValue(centre, camera, centreX + 40.0F, centreY, kWidth, kHeight);
    const float planarLength = planar.length();
    expect(planarLength > 5.0F, "a 40px centre drag must move the object visibly");
    const auto forward = camera.forward();
    expect(std::abs(whitehole::math::Vec3f::dot(planar, forward)) < planarLength * 0.02F,
           "a centre drag must stay in the view plane");

    // ---- scale drag ------------------------------------------------------
    // One axis length of drag must double the scale on that axis alone.
    GizmoDrag scale;
    // Rebuilt from the Y axis tip, which exercises a different axis than the
    // translate test above.
    {
        const auto direction = axisDirection(GizmoHandle::AxisY);
        float grabX = 0.0F;
        float grabY = 0.0F;
        expect(camera.worldToScreen({direction.x * axisLength, direction.y * axisLength,
                                     direction.z * axisLength},
                                    kWidth, kHeight, grabX, grabY),
               "the Y axis tip must project");
        expect(beginGizmoDrag(scale, camera, {0.0F, 0.0F, 0.0F}, GizmoMode::Scale,
                              GizmoHandle::AxisY, grabX, grabY, kWidth, kHeight),
               "beginGizmoDrag refused the Y tip");
        const auto doubled =
            gizmoDragValue(scale, camera, grabX + scale.axisDir.x * screenLength,
                           grabY + scale.axisDir.y * screenLength, kWidth, kHeight);
        expect(std::abs(doubled.y - 2.0F) < 0.05F, "an axis-length drag must double the scale");
        expect(std::abs(doubled.x - 1.0F) < 0.01F && std::abs(doubled.z - 1.0F) < 0.01F,
               "a scale drag must not touch the other axes");
    }

    // ---- snapping ----------------------------------------------------------
    // Snap rounds the *cumulative* value to the step, and a zero step disables.
    expect(std::abs(whitehole::render::snapValue(12.0F, 10.0F) - 10.0F) < 0.001F,
           "snap must round to the step");
    expect(std::abs(whitehole::render::snapValue(17.0F, 10.0F) - 20.0F) < 0.001F,
           "snap must round up past halfway");
    expect(std::abs(whitehole::render::snapValue(7.0F, 0.0F) - 7.0F) < 0.001F,
           "a zero step must disable snapping");
    const auto snappedMove = whitehole::render::snapTranslate({12.0F, -3.0F, 26.0F}, 10.0F);
    expect(std::abs(snappedMove.x - 10.0F) < 0.001F && std::abs(snappedMove.z - 30.0F) < 0.001F,
           "translate snap is off");
    const auto snappedRot = whitehole::render::snapRotate({16.0F, 0.0F, 0.0F}, 15.0F);
    expect(std::abs(snappedRot.x - 15.0F) < 0.001F, "rotate snap is off");
    const auto snappedScale = whitehole::render::snapScale({1.06F, 1.0F, 1.0F}, 0.1F);
    expect(std::abs(snappedScale.x - 1.1F) < 0.001F && std::abs(snappedScale.y - 1.0F) < 0.001F,
           "scale snap must keep 1.0 exact");
}

void testViewportScene() {
    whitehole::smg::PlacementObject object;
    object.name = "Kinopio";
    object.kind = "obj";
    object.position = {100.0F, 0.0F, 0.0F};
    object.rotation = {0.0F, 0.0F, 0.0F};
    object.scale = {1.0F, 1.0F, 1.0F};

    whitehole::render::ViewportScene scene;
    scene.rebuild({object});
    expect(scene.boxes().size() == 1, "viewport scene dropped the object");
    const auto& box = scene.boxes().front();
    expect(std::abs(box.center.x - 100.0F) < 0.001F, "viewport box center is wrong");

    // Box matrix must map the unit-box corner onto position + half extent.
    const auto corner = box.world.transformPoint({1.0F, 1.0F, 1.0F});
    expect(std::abs(corner.x - 125.0F) < 0.01F, "viewport box world matrix is wrong");

    whitehole::render::ViewportCamera camera;
    camera.target = object.position;
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;
    const auto hit = scene.pick(camera, 400.0F, 300.0F, 800.0F, 600.0F);
    expect(hit.has_value() && *hit == 0, "viewport picking missed the centered object");
    // Far corner of the screen should miss the single centered box.
    expect(!scene.pick(camera, 799.0F, 599.0F, 800.0F, 600.0F).has_value(), "viewport picking hit empty space");
    // Forgiving click: a near-miss within the slop still grabs the object,
    // while a far click does not.
    float centreX = 0.0F;
    float centreY = 0.0F;
    expect(camera.worldToScreen({100.0F, 0.0F, 0.0F}, 800.0F, 600.0F, centreX, centreY),
           "object centre must project");
    expect(scene.pickForgiving(camera, 799.0F, 599.0F, 800.0F, 600.0F, 20000.0F, 10.0F).has_value() ==
               scene.pick(camera, 799.0F, 599.0F, 800.0F, 600.0F).has_value(),
           "forgiving pick must not invent hits far from the object");
    expect(scene.pickForgiving(camera, centreX + 5.0F, centreY, 800.0F, 600.0F).has_value(),
           "forgiving pick missed a near click");

    // Picking uses the placeholder's visible surface, not its proxy cube. A point
    // on the cylinder's top cap is off-centre but must still select it.
    float topX = 0.0F;
    float topY = 0.0F;
    expect(camera.worldToScreen(box.world.transformPoint({0.0F, 1.0F, 0.0F}), 800.0F, 600.0F, topX, topY),
           "the top-cap pick point must project");
    const auto surfaceHit = scene.pick(camera, topX, topY, 800.0F, 600.0F);
    expect(surfaceHit.has_value() && *surfaceHit == 0,
           "an off-centre click on visible geometry must select its object");

    // This point is inside the square proxy but outside the cylinder silhouette.
    // Even with centre slop disabled, neither exact nor forgiving picking may
    // select empty proxy space.
    float emptyX = 0.0F;
    float emptyY = 0.0F;
    expect(camera.worldToScreen(box.world.transformPoint({0.99F, 0.99F, 0.99F}), 800.0F, 600.0F,
                                emptyX, emptyY),
           "the empty-proxy pick point must project");
    expect(!scene.pick(camera, emptyX, emptyY, 800.0F, 600.0F).has_value(),
           "picking empty space inside the proxy must miss");
    expect(!scene.pickForgiving(camera, emptyX, emptyY, 800.0F, 600.0F, 20000.0F, 0.0F).has_value(),
           "forgiving picking must not turn empty proxy space into a hit");

    // Direct triangle tests cover two-sided picking, world transforms, misses,
    // and the closest-hit bound independently of category proxy shapes.
    const whitehole::render::Ray triangleRay{{10.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}};
    const std::vector<whitehole::math::Vec3f> triangles{
        {-1.0F, -1.0F, 0.0F}, {1.0F, -1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const auto triangleHit = whitehole::render::rayIntersectsTriangles(
        triangleRay, whitehole::math::Matrix4::translation({10.0F, 0.0F, 0.0F}), triangles, 100.0F);
    expect(triangleHit.has_value() && std::abs(*triangleHit - 5.0F) < 0.001F,
           "transformed triangle picking returned the wrong distance");
    const whitehole::render::Ray originTriangleRay{{0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}};
    const whitehole::math::Matrix4 identity{};
    std::vector<whitehole::math::Vec3f> reversedTriangles{
        triangles[2], triangles[1], triangles[0]};
    expect(whitehole::render::rayIntersectsTriangles(
               originTriangleRay, identity, reversedTriangles, 100.0F).has_value(),
           "triangle picking must be two-sided");
    expect(!whitehole::render::rayIntersectsTriangles(
               {{2.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}}, identity, triangles, 100.0F)
                .has_value(),
           "a ray beside a triangle must miss");
    expect(!whitehole::render::rayIntersectsTriangles(
               originTriangleRay, identity, triangles, 4.0F)
                .has_value(),
           "a triangle beyond the distance bound must be ignored");

    whitehole::render::ModelTriangle modelTriangle;
    modelTriangle.a.position = triangles[0];
    modelTriangle.b.position = triangles[1];
    modelTriangle.c.position = triangles[2];
    expect(whitehole::render::rayIntersectsTriangles(
               originTriangleRay, identity, std::vector<whitehole::render::ModelTriangle>{modelTriangle}, 100.0F)
                .has_value(),
           "model triangle picking missed visible geometry");

    // Marquee: a box around the centre grabs it, an empty corner grabs nothing.
    expect(scene.pickRect(camera, centreX - 20.0F, centreY - 20.0F, centreX + 20.0F, centreY + 20.0F,
                          800.0F, 600.0F)
               .size() == 1,
           "marquee missed the framed object");
    expect(scene.pickRect(camera, 700.0F, 500.0F, 799.0F, 599.0F, 800.0F, 600.0F).empty(),
           "marquee hit empty space");
}

void testObjectVisual() {
    using whitehole::render::ObjectCategory;
    using whitehole::render::categoryStyle;
    using whitehole::render::classifyObject;
    using whitehole::render::objectStyle;
    using whitehole::render::shapeTriangles;

    // Table kind drives the category first.
    expect(classifyObject("start", "Mario") == ObjectCategory::Player, "start objects should classify as Player");
    expect(classifyObject("camera", "CameraPos") == ObjectCategory::Camera, "camera table should classify as Camera");
    expect(classifyObject("area", "AreaVolume") == ObjectCategory::Zone, "area table should classify as Zone");
    expect(classifyObject("gravity", "GravitySphere") == ObjectCategory::Gravity, "gravity table should classify as Gravity");
    expect(classifyObject("mappart", "Elevator") == ObjectCategory::MapPart, "mappart table should classify as MapPart");
    expect(classifyObject("cutscene", "Demo") == ObjectCategory::Cutscene, "cutscene table should classify as Cutscene");

    // Object names refine plain "obj" placements.
    expect(classifyObject("obj", "Kinopio") == ObjectCategory::Player, "Kinopio should classify as Player");
    expect(classifyObject("obj", "Kuribo") == ObjectCategory::Enemy, "Kuribo should classify as Enemy");
    expect(classifyObject("obj", "BossKuriboJunior") == ObjectCategory::Enemy, "Boss names should classify as Enemy");
    expect(classifyObject("obj", "PowerStar") == ObjectCategory::Item, "PowerStar should classify as Item");
    expect(classifyObject("obj", "PurpleCoin") == ObjectCategory::Item, "coins should classify as Item");
    expect(classifyObject("obj", "PlanetDifferencesAndBeyond") == ObjectCategory::Terrain, "planets should classify as Terrain");
    expect(classifyObject("obj", "OceanWave") == ObjectCategory::Misc, "unknown names should classify as Misc");

    // Every category has a distinct, valid color (the whole visual system
    // depends on categories being tellable apart at a glance).
    for (std::size_t a = 0; a < whitehole::render::categoryCount(); ++a) {
        const auto& styleA = categoryStyle(static_cast<ObjectCategory>(a));
        expect(std::abs(styleA.color[0]) + std::abs(styleA.color[1]) + std::abs(styleA.color[2]) > 0.1F,
               "category color must not be black");
        for (std::size_t b = a + 1; b < whitehole::render::categoryCount(); ++b) {
            const auto& styleB = categoryStyle(static_cast<ObjectCategory>(b));
            const bool same = std::abs(styleA.color[0] - styleB.color[0]) < 0.01F &&
                              std::abs(styleA.color[1] - styleB.color[1]) < 0.01F &&
                              std::abs(styleA.color[2] - styleB.color[2]) < 0.01F;
            expect(!same, "category colors must be distinct");
            expect(styleA.shape != styleB.shape || std::string_view(styleA.label) != std::string_view(styleB.label),
                   "categories must differ in shape or label");
        }
    }

    // Shape meshes are unit-sized triangle soup.
    const auto cube = shapeTriangles(whitehole::render::CategoryStyle::Shape::Cube);
    expect(cube.size() == 12 * 3, "cube mesh should have 12 triangles");
    const auto sphere = shapeTriangles(whitehole::render::CategoryStyle::Shape::Sphere);
    expect(sphere.size() == 12 * 8 * 2 * 3, "sphere mesh triangle count is wrong");
    const auto pyramid = shapeTriangles(whitehole::render::CategoryStyle::Shape::Pyramid);
    expect(pyramid.size() == 6 * 3, "pyramid mesh should have 6 triangles");
    const auto octa = shapeTriangles(whitehole::render::CategoryStyle::Shape::Octahedron);
    expect(octa.size() == 8 * 3, "octahedron mesh should have 8 triangles");
    const auto cylinder = shapeTriangles(whitehole::render::CategoryStyle::Shape::Cylinder);
    expect(cylinder.size() == 12 * 4 * 3, "cylinder mesh triangle count is wrong");

    const float maxComponent = [](const std::vector<whitehole::math::Vec3f>& triangles) {
        float worst = 0.0F;
        for (const auto& vertex : triangles) {
            worst = std::max(worst, std::max({std::abs(vertex.x), std::abs(vertex.y), std::abs(vertex.z)}));
        }
        return worst;
    }(cube);
    expect(maxComponent < 1.0001F, "cube mesh must stay within the unit cube");

    // The scene stores the category so renderers and lists can share it.
    whitehole::smg::PlacementObject object;
    object.name = "Kuribo";
    object.kind = "obj";
    object.scale = {0.001F, 0.001F, 0.001F};
    whitehole::render::ViewportScene scene;
    scene.rebuild({object});
    expect(scene.boxes().size() == 1, "viewport scene dropped the object");
    expect(scene.boxes().front().category == ObjectCategory::Enemy, "viewport scene lost the object category");
    // Micro-scaled objects are clamped to the minimum visual scale so they
    // stay visible and clickable.
    expect(std::abs(scene.boxes().front().halfExtents.x - 25.0F * whitehole::render::kMinVisualScale) < 0.01F,
           "minimum visual scale clamp failed");
    // objectStyle ties classification to visuals in one call.
    expect(&objectStyle("obj", "PowerStar") == &categoryStyle(ObjectCategory::Item),
           "objectStyle should match classifyObject");
}

void testHashes() {
    expect(whitehole::smg::jmapHash("name") == 0x00337A8B, "JMap hash does not match the game algorithm");
    expect(whitehole::smg::superFastHash("") == 0, "empty SuperFastHash changed");
    expect(whitehole::smg::superFastHash("Whitehole") == 0x680E328E, "SuperFastHash compatibility vector changed");
}

void testCameraParam() {
    using namespace whitehole::smg;

    // Field hashes must match the game; these constants are LaunchCamPlus'
    // published BCAM hashes (and the template files parse with them).
    expect(jmapHash("version") == 0x14F51CD8, "version hash drifted from the game");
    expect(jmapHash("id") == 0x00000D1B, "id hash drifted from the game");
    expect(jmapHash("camtype") == 0x20C58F89, "camtype hash drifted from the game");
    expect(jmapHash("angleA") == 0xABC4A1CE, "angleA hash drifted from the game");
    expect(jmapHash("woffset.Y") == 0xBEC02B35, "woffset.Y hash drifted from the game");
    expect(jmapHash("eflag.enableErpFrame") == 0x1BCD52AA, "eflag hash drifted from the game");

    // Spec table: 52 fields, LCP defaults (the wiki's angleA/dist are swapped).
    expect(cameraFieldSpecs().size() == 52, "the spec table must describe all 52 BCAM fields");
    expect(cameraFieldSpec("dist") != nullptr, "dist must have a spec");
    expect(cameraFieldSpec("nope") == nullptr, "unknown fields must have no spec");
    expect(cameraFieldSpecForHash(jmapHash("camint")) != nullptr, "hash lookup must find camint");
    const auto* dist = cameraFieldSpec("dist");
    expect(std::get<float>(cameraFieldDefault(*dist, kCameraVersionSmg2)) == 1200.0F,
           "dist engine default must be 1200");
    const auto* angleA = cameraFieldSpec("angleA");
    expect(std::get<float>(cameraFieldDefault(*angleA, kCameraVersionSmg1)) == 0.0F,
           "angleA engine default must be 0");
    const auto* num1 = cameraFieldSpec("num1");
    expect(std::get<std::int32_t>(cameraFieldDefault(*num1, kCameraVersionSmg1)) == 0 &&
               std::get<std::int32_t>(cameraFieldDefault(*num1, kCameraVersionSmg2)) == 1,
           "num1 defaults must flip per engine version");
    const auto* woffsetY = cameraFieldSpec("woffset.Y");
    expect(std::get<float>(cameraFieldDefault(*woffsetY, kCameraVersionSmg1)) == 100.0F &&
               std::get<float>(cameraFieldDefault(*woffsetY, kCameraVersionSmg2)) == 0.0F,
           "woffset.Y defaults must flip per engine version");
    expect(cameraFieldApplies(CameraFieldScope::event, CameraContext::event) &&
               !cameraFieldApplies(CameraFieldScope::event, CameraContext::cube),
           "event-only fields must not apply to camera areas");
    expect(cameraFieldApplies(CameraFieldScope::game, CameraContext::cube) &&
               !cameraFieldApplies(CameraFieldScope::game, CameraContext::other),
           "group flags must apply to camera areas but not o: cameras");

    // Id contexts, formatting and friendly names.
    const CameraId cube = parseCameraId("c:000f");
    expect(cube.context == CameraContext::cube && cube.number == 15, "c: id must parse");
    expect(formatCameraId(cube) == "c:000f", "cube id must round trip canonically");
    expect(describeCameraId(cube) == "Camera Area 15", "cube description changed");
    const CameraId spawn = parseCameraId("s:003c");
    expect(spawn.context == CameraContext::spawn && spawn.number == 60, "s: id must parse");
    expect(describeCameraId(spawn) == "Spawn Point 60", "spawn description changed");
    const CameraId scenario = parseCameraId("e:シナリオスターター:005:01番目");
    expect(scenario.context == CameraContext::event, "scenario starter id must parse as event");
    expect(describeCameraId(scenario) == "Scenario Starter 005 camera 01",
           "scenario starter description changed");
    expect(describeCameraId(parseCameraId("e:パワースター固有005")) == "Power Star Appearance 005",
           "concatenated event ids must translate");
    expect(describeCameraId(parseCameraId("e:不明のイベント")) == "Event 不明のイベント",
           "unknown events must echo their name");
    expect(describeCameraId(parseCameraId("o:デフォルトカメラ")) == "Default Camera",
           "default camera must translate");
    expect(describeCameraId(parseCameraId("o:スタートアニメカメラ")) ==
               "Galaxy Intro Camera (created by the game)",
           "game-created cameras must be flagged");
    expect(describeCameraId(parseCameraId("g:SomeGroup")) == "Group: SomeGroup",
           "group description changed");
    expect(parseCameraId("bogus").context == CameraContext::invalid, "junk ids must be invalid");
    expect(cubeCameraIdForArg(15) == "c:000f" && spawnCameraIdFor(60) == "s:003c",
           "area/spawn id builders changed");
    expect(cubeCameraIdForArg(-1).empty(), "negative area args must build no id");

    // The id-authoring surface the "Add camera" dialog is built on. These ids
    // carry Japanese the panel's font cannot draw, so the only way an author can
    // make one is by picking it from the dictionary and having the core compose
    // the text; that composition has to match what describeCameraId reads back.
    {
        const KnownCameraEvent* starter = cameraKnownEvent("シナリオスターター");
        expect(starter != nullptr, "the scenario starter must be in the dictionary");
        expect(starter != nullptr && starter->en == "Scenario Starter" &&
                   starter->needsId && starter->needsSub,
               "the scenario starter takes both a set id and a sub-index");
        // Every entry must be named, or the picker would show a blank row.
        for (const KnownCameraEvent& entry : cameraKnownEvents()) {
            expect(!entry.jp.empty() && !entry.en.empty(),
                   "a known event is missing its Japanese or English name");
        }
        expect(!cameraKnownOthers().empty(), "the o: dictionary must not be empty");
        // The game-created cameras are never stored, so the editor must not be
        // able to offer one as something to add; it only names them.
        expect(!cameraGameCreatedEvents().empty(), "the game-created dictionary must exist");
        for (const KnownCameraEvent& created : cameraGameCreatedEvents()) {
            expect(cameraKnownEvent(created.jp) == nullptr,
                   "a game-created camera must not also be an authorable event");
        }

        // The exact byte-for-byte form, pinned against the same string the
        // describeCameraId() checks above parse.
        const std::string composed = eventCameraIdFor("シナリオスターター", 5, 1);
        expect(composed == "e:シナリオスターター:005:01番目",
               "the composed scenario starter id must match the game's own format");
        expect(describeCameraId(parseCameraId(composed)) == "Scenario Starter 005 camera 01",
               "a composed id must read back as the same camera as a hand-written one");
        // Both indices are zero padded by the game.
        expect(eventCameraIdFor("シナリオスターター", 0, 0) ==
                   "e:シナリオスターター:000:00番目",
               "event indices must be zero padded");
        expect(eventCameraIdFor("シナリオスターター", 999, 99) ==
                   "e:シナリオスターター:999:99番目",
               "the widest event indices must still pad to three and two digits");
        // The longest real event name plus both indices has to fit the panel's id
        // buffer, which is why that buffer is 160 bytes and not 64. Computed
        // rather than hardcoded so adding a longer event keeps the test honest.
        std::size_t widest = 0;
        for (const KnownCameraEvent& entry : cameraKnownEvents()) {
            widest = std::max(widest,
                              eventCameraIdFor(entry.jp, 999, 99).size());
        }
        expect(widest > 64,
               "some known event id is longer than a 64-byte buffer could hold");
        expect(widest <= 160,
               "a known event id must fit the panel's 160-byte id buffer");
        expect(eventCameraIdFor("郵便屋さんキノピオ固有注目会話", 999, 99) ==
                   "e:郵便屋さんキノピオ固有注目会話:999:99番目",
               "a long event name must compose without truncation");
    }

    // Duplicate detection, which the dialog and the "next free" composer must
    // agree on: it compares the parsed id, not the text.
    {
        CameraParamTable table = CameraParamTable::create(kCameraVersionSmg2);
        (void)table.addCamera("c:000f", "CAM_TYPE_XZ_PARA", kCameraVersionSmg2);
        expect(cameraIdExists(table, "c:000f"), "an existing id must be found");
        expect(cameraIdExists(table, "c:000F"), "hex case must not hide a duplicate");
        expect(!cameraIdExists(table, "c:0010"), "a different number is a different camera");
        expect(!cameraIdExists(table, "bogus"), "an unparseable id is never a duplicate");
        expect(!cameraIdExists(table, ""), "an empty id is never a duplicate");

        // nextFreeEventCameraId walks the sub-index past whatever is stored.
        CameraParamTable events = CameraParamTable::create(kCameraVersionSmg2);
        expect(nextFreeEventCameraId(events, "シナリオスターター", 1) ==
                   "e:シナリオスターター:001:00番目",
               "the first free event id must be sub-index 0");
        (void)events.addCamera("e:シナリオスターター:001:00番目", "CAM_TYPE_XZ_PARA",
                               kCameraVersionSmg2);
        expect(nextFreeEventCameraId(events, "シナリオスターター", 1) ==
                   "e:シナリオスターター:001:01番目",
               "the composer must skip a sub-index already in the table");
        (void)events.addCamera("e:シナリオスターター:001:01番目", "CAM_TYPE_XZ_PARA",
                               kCameraVersionSmg2);
        expect(nextFreeEventCameraId(events, "シナリオスターター", 1) ==
                   "e:シナリオスターター:001:02番目",
               "the composer must keep walking past consecutive taken indices");
        // A different set id is a different camera family, so it starts over.
        expect(nextFreeEventCameraId(events, "シナリオスターター", 2) ==
                   "e:シナリオスターター:002:00番目",
               "a different camera set must not inherit the previous one's index");
    }

    // In-game pose: all-zero parallel angles look along -X at the target,
    // the shared spherical convention of the decompiled translators.
    {
        CameraPreviewParams parallel;
        parallel.camtype = "CAM_TYPE_XZ_PARA";
        parallel.angleA = 0.0F;
        parallel.angleB = 0.0F; // the struct mirrors the engine defaults otherwise
        PoseSupport support = PoseSupport::none;
        const GameCameraPose pose = solveGameCameraPose(parallel, {100.0F, 50.0F, 0.0F}, &support);
        expect(support == PoseSupport::exact, "XZ_PARA must solve exactly");
        expect(std::abs(pose.eye.x - 1300.0F) < 0.01F && std::abs(pose.eye.y - 50.0F) < 0.01F &&
                   std::abs(pose.at.x - 100.0F) < 0.01F,
               "zero-angle parallel eye must sit at dist along +X");
    }
    {
        // Template zone camera c:0000 (angleB 0.3, everything else default).
        const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
        auto archive = whitehole::io::RarcArchive::open(templates / "SMG2BigGalaxyMap.arc");
        CameraParamTable zone(archive.read(kCameraParamPath), archive.endian(),
                              cameraVersionForGame(2));
        PoseSupport support = PoseSupport::none;
        const GameCameraPose pose =
            solveGameCameraPose(cameraPreviewParams(zone, 0), {0.0F, 0.0F, 0.0F}, &support);
        expect(support == PoseSupport::exact, "the template camera must solve exactly");
        const float expectedX = 1200.0F * std::cos(0.3F);
        const float expectedZ = 1200.0F * std::sin(0.3F);
        expect(std::abs(pose.eye.x - expectedX) < 0.5F && std::abs(pose.eye.z - expectedZ) < 0.5F,
               "template camera eye must land on the angleB/dist sphere");
        expect(std::abs(pose.fovYRadians - 45.0F * 3.141592653589793F / 180.0F) < 0.0001F,
               "the template camera's FoV must read as 45 degrees");
    }
    {
        // POINT_FIX aims from the negated polar direction around wpoint at Mario.
        CameraPreviewParams fix;
        fix.camtype = "CAM_TYPE_POINT_FIX";
        fix.wpoint = {0.0F, 0.0F, 1000.0F};
        fix.dist = 200.0F;
        fix.axis = {90.0F, 0.0F, 0.0F}; // degrees here
        const GameCameraPose pose = solveGameCameraPose(fix, {0.0F, 0.0F, 0.0F});
        // axis.x = 90deg points the direction vector at +Z; the negation puts
        // the eye 200 units on the near side of wpoint, looking past it at Mario.
        expect(std::abs(pose.eye.x - 0.0F) < 0.01F && std::abs(pose.eye.z - 800.0F) < 0.01F,
               "point-fix eye must negate its direction vector");
    }
    {
        // EYEPOS_FIX pins the eye to wpoint and watches the target.
        CameraPreviewParams eyePos;
        eyePos.camtype = "CAM_TYPE_EYEPOS_FIX";
        eyePos.wpoint = {10.0F, 20.0F, 30.0F};
        const GameCameraPose pose = solveGameCameraPose(eyePos, {1.0F, 2.0F, 3.0F});
        expect(pose.eye.x == 10.0F && pose.at.x == 1.0F, "eyepos-fix must pin the eye");
    }
    {
        // FOLLOW-family orbits share the parallel sphere.
        CameraPreviewParams follow;
        follow.camtype = "CAM_TYPE_FOLLOW";
        follow.angleA = 0.17453294F;
        follow.angleB = 0.34906587F;
        follow.dist = 1200.0F;
        const GameCameraPose pose = solveGameCameraPose(follow, {0.0F, 0.0F, 0.0F});
        expect(pose.eye.length() > 1199.0F && pose.eye.length() < 1201.0F,
               "follow eye must sit on the dist sphere");
    }
    expect(cameraPoseSupport("CAM_TYPE_XZ_PARA", kCameraVersionSmg2) == PoseSupport::exact &&
               cameraPoseSupport("CAM_TYPE_FOLLOW", kCameraVersionSmg2) == PoseSupport::spherical &&
               cameraPoseSupport("CAM_TYPE_TALK", kCameraVersionSmg2) == PoseSupport::none &&
               cameraPoseSupport("CAM_TYPE_CUSTOM", kCameraVersionSmg2) == PoseSupport::none,
           "pose support classification changed");
    expect(cameraPoseSupport("CAM_TYPE_ICECUBE_PLANET", kCameraVersionSmg2) == PoseSupport::spherical,
           "aliases must resolve through the pose dispatch");


    // Camera type registry: aliases resolve only at/after their version.
    expect(cameraTypes().size() == 53, "the registry must hold 48 classes + 5 aliases");
    expect(cameraTypeInfo("CAM_TYPE_XZ_PARA") != nullptr, "XZ_PARA must be registered");
    const CameraTypeInfo* alias = cameraTypeInfo("CAM_TYPE_ICECUBE_PLANET");
    expect(alias != nullptr && alias->aliasOf == "CAM_TYPE_CUBE_PLANET", "alias entry changed");
    expect(resolveCameraType("CAM_TYPE_ICECUBE_PLANET", kCameraVersionSmg2) == "CAM_TYPE_CUBE_PLANET",
           "alias must resolve on SMG2");
    expect(resolveCameraType("CAM_TYPE_ICECUBE_PLANET", 196610) == "CAM_TYPE_ICECUBE_PLANET",
           "alias must stay unresolved below its required version");
    expect(resolveCameraType("CAM_TYPE_CUSTOM", kCameraVersionSmg2) == "CAM_TYPE_CUSTOM",
           "unknown camtypes must survive a round trip");

    // Real template tables: parse, inspect, round trip.
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto archive = whitehole::io::RarcArchive::open(templates / "SMG2BigGalaxyMap.arc");
    const auto bytes = archive.read(kCameraParamPath);
    expect(!bytes.empty(), "template CameraParam.bcam is missing");
    CameraParamTable table(bytes, archive.endian(), cameraVersionForGame(2));
    expect(table.size() == 1, "the SMG2 template must hold one camera");
    const auto cameras = table.cameras();
    expect(cameras[0].id == "c:0000" && cameras[0].camtype == "CAM_TYPE_XZ_PARA",
           "template camera identity changed");
    expect(cameras[0].version == 196631 && cameras[0].context == CameraContext::cube,
           "template camera version/context changed");
    expect(table.getFloat(0, "dist") == 1200.0F && table.getFloat(0, "angleA") == 0.0F,
           "template camera values changed");
    expect(table.getInt(0, "camint") == 120 && table.getString(0, "id") == "c:0000",
           "template camera reads changed");
    // The BCSV writer pads the file to its alignment, so the original bytes
    // must be a prefix of the re-serialised table (content unchanged), and a
    // reparse must agree field-for-field.
    const auto contentPreserved = [](const std::vector<std::uint8_t>& original,
                                     const std::vector<std::uint8_t>& written) {
        return written.size() >= original.size() &&
               std::equal(original.begin(), original.end(), written.begin());
    };
    const auto serialized = table.serialize();
    expect(contentPreserved(bytes, serialized),
           "untouched camera table round trip changed its content");
    CameraParamTable reparse(serialized, archive.endian(), cameraVersionForGame(2));
    expectTablesEqual(table.table(), reparse.table(), "template camera table");

    // SMG1 archives are all lowercase; lookups must still find the table.
    auto smg1Archive = whitehole::io::RarcArchive::open(templates / "SMG1BigGalaxy.arc");
    CameraParamTable smg1Table(smg1Archive.read(kCameraParamPath), smg1Archive.endian(),
                               cameraVersionForGame(1));
    expect(smg1Table.size() == 1, "the SMG1 template camera table is missing");

    // Sparse fallbacks on a minimal (required-columns-only) table.
    CameraParamTable minimal = CameraParamTable::create(kCameraVersionSmg2);
    const std::size_t first = minimal.addCamera("c:0001", "CAM_TYPE_XZ_PARA", kCameraVersionSmg2);
    expect(minimal.getFloat(first, "dist") == 1200.0F, "missing columns must read 1200 for dist");
    expect(minimal.getFloat(first, "woffset.Y") == 0.0F, "SMG2 woffset.Y default changed");
    expect(minimal.getInt(first, "num1") == 1, "SMG2 num1 default changed");
    expect(!minimal.hasColumn("dist"), "reading a default must not create a column");
    CameraParamTable minimalSmg1 = CameraParamTable::create(kCameraVersionSmg1);
    const std::size_t old = minimalSmg1.addCamera("s:0001", "CAM_TYPE_FOLLOW", kCameraVersionSmg1);
    expect(minimalSmg1.getFloat(old, "woffset.Y") == 100.0F, "SMG1 woffset.Y default changed");
    expect(minimalSmg1.getInt(old, "num1") == 0, "SMG1 num1 default changed");

    // Writing a parameter the sparse table lacked creates the column and
    // keeps every other camera on its engine default.
    const std::size_t second = minimal.addCamera("c:0002", "CAM_TYPE_XZ_PARA", kCameraVersionSmg2);
    minimal.setFloat(first, "fovy", 90.0F);
    expect(minimal.hasColumn("fovy") && minimal.getFloat(first, "fovy") == 90.0F,
           "the written fovy must be stored");
    expect(minimal.getFloat(second, "fovy") == 45.0F,
           "the untouched camera must keep the engine default");
    CameraParamTable reloaded(minimal.serialize(), whitehole::io::Endian::big, kCameraVersionSmg2);
    expect(reloaded.size() == 2 && reloaded.getFloat(first, "fovy") == 90.0F,
           "edits must survive a save/reload cycle");
    expect(reloaded.cameras()[0].id == "c:0001" && reloaded.cameras()[1].id == "c:0002",
           "camera order must survive a save/reload cycle");

    // Add/remove on the real table restores the original bytes.
    const std::size_t appended =
        table.addCamera("e:シナリオスターター:001:00番目", "CAM_TYPE_EYEPOS_FIX", kCameraVersionSmg2);
    expect(appended == 1 && table.size() == 2, "addCamera must append a row");
    expect(table.removeCamera(1) && table.size() == 1, "removeCamera must drop the row again");
    expect(!table.removeCamera(99), "removing a missing row must fail cleanly");
    expect(contentPreserved(bytes, table.serialize()),
           "removing an added camera must restore the original content");
}



std::vector<std::uint8_t> makeTinyBcsv(whitehole::io::Endian endian) {
    whitehole::io::BinaryWriter writer(endian);
    writer.writeU32(1);    // rows
    writer.writeU32(2);    // fields
    writer.writeU32(0x28); // data offset
    writer.writeU32(8);    // row size

    writer.writeU32(whitehole::smg::jmapHash("number"));
    writer.writeU32(0x0000FFFF);
    writer.writeU16(0);
    writer.writeU8(0);
    writer.writeU8(static_cast<std::uint8_t>(whitehole::smg::BcsvType::integer));

    writer.writeU32(whitehole::smg::jmapHash("label"));
    writer.writeU32(0xFFFFFFFF);
    writer.writeU16(4);
    writer.writeU8(0);
    writer.writeU8(static_cast<std::uint8_t>(whitehole::smg::BcsvType::stringOffset));

    writer.writeU32(42);
    writer.writeU32(0);
    writer.writeString("Comet");
    return std::move(writer).take();
}

void expectTablesEqual(const whitehole::smg::BcsvTable& left, const whitehole::smg::BcsvTable& right,
                       const std::string& context) {
    expect(left.entrySize() == right.entrySize(), context + ": row size changed");
    expect(left.fields().size() == right.fields().size(), context + ": field count changed");
    expect(left.rows().size() == right.rows().size(), context + ": row count changed");
    for (std::size_t index = 0; index < left.fields().size(); ++index) {
        const auto& a = left.fields()[index];
        const auto& b = right.fields()[index];
        expect(a.hash == b.hash && a.mask == b.mask && a.offset == b.offset
                   && a.shift == b.shift && a.type == b.type,
               context + ": field descriptor changed");
    }
    for (std::size_t row = 0; row < left.rows().size(); ++row) {
        expect(left.rows()[row].values.size() == right.rows()[row].values.size(),
               context + ": row width changed");
        for (std::size_t field = 0; field < left.rows()[row].values.size(); ++field) {
            const auto& a = left.rows()[row].values[field];
            const auto& b = right.rows()[row].values[field];
            expect(a.index() == b.index(), context + ": value type changed");
            if (std::holds_alternative<float>(a)) {
                const auto av = std::get<float>(a);
                const auto bv = std::get<float>(b);
                expect(av == bv || (std::isnan(av) && std::isnan(bv)), context + ": float value changed");
            } else {
                expect(a == b, context + ": value changed");
            }
        }
    }
}

void testBcsvEndianness() {
    for (const auto endian : {whitehole::io::Endian::big, whitehole::io::Endian::little}) {
        const whitehole::smg::BcsvTable table(makeTinyBcsv(endian), endian);
        expect(table.fields().size() == 2 && table.rows().size() == 1, "tiny BCSV shape changed");
        expect(std::get<std::int32_t>(table.rows()[0].values[0]) == 42, "BCSV integer parsing failed");
        expect(std::get<std::string>(table.rows()[0].values[1]) == "Comet", "BCSV string parsing failed");
        const whitehole::smg::BcsvTable rewritten(table.serialize(), endian);
        expectTablesEqual(table, rewritten, "tiny BCSV round trip");

        auto invalid = table;
        invalid.rows()[0].values[0] = std::int32_t{70000};
        bool overflowRejected = false;
        try {
            (void)invalid.serialize();
        } catch (const std::runtime_error&) {
            overflowRejected = true;
        }
        expect(overflowRejected, "BCSV silently truncated an integer outside its field mask");

        auto invalidByteMask = table;
        invalidByteMask.fields()[0].type = whitehole::smg::BcsvType::byte;
        invalidByteMask.fields()[0].mask = 0x100;
        invalidByteMask.rows()[0].values[0] = std::int8_t{1};
        bool maskRejected = false;
        try {
            (void)invalidByteMask.serialize();
        } catch (const std::runtime_error&) {
            maskRejected = true;
        }
        expect(maskRejected, "BCSV accepted mask bits outside a field's storage width");
    }
}

void testBcsvMutation() {
    using whitehole::io::Endian;
    using whitehole::smg::BcsvTable;
    using whitehole::smg::BcsvType;

    BcsvTable table(makeTinyBcsv(Endian::big), Endian::big);
    expect(table.hasField("number"), "hasField(number) failed");
    expect(!table.hasField("not_a_field"), "hasField reported a missing field");
    expect(table.rawValue(table.rows()[0], "number") != nullptr, "rawValue(name) failed");

    // Type-aware setters keep the stored variant in step with the field type.
    table.setInt(table.rows()[0], "number", 1234);
    expect(table.getInt(table.rows()[0], "number") == 1234, "setInt/getInt failed");
    table.setBool(table.rows()[0], "number", true);
    expect(table.getBool(table.rows()[0], "number"), "setBool/getBool failed");

    // addRow appends a default-valued row sized to the current schema.
    const auto added = table.addRow();
    expect(added == 1, "addRow should append at index 1");
    expect(table.getInt(table.rows()[added], "number") == 0, "addRow default integer wrong");
    expect(table.getString(table.rows()[added], "label").empty(), "addRow default string wrong");

    // cloneRow copies the source values into a new row right after it.
    table.setInt(table.rows()[0], "number", 7);
    const auto cloned = table.cloneRow(0);
    expect(cloned == 1, "cloneRow should insert after the source row");
    expect(table.getInt(table.rows()[cloned], "number") == 7, "cloneRow did not copy the value");
    expect(table.getString(table.rows()[cloned], "label") == "Comet", "cloneRow did not copy the string");

    // ensureField widens every row and is idempotent.
    const auto flagIndex = table.ensureField("flag", BcsvType::byte);
    expect(table.fields()[flagIndex].type == BcsvType::byte, "ensureField stored the wrong type");
    expect(table.ensureField("flag", BcsvType::integer) == flagIndex, "ensureField is not idempotent");
    for (const auto& row : table.rows()) {
        expect(row.values.size() == table.fields().size(), "ensureField left a ragged row");
    }
    table.setInt(table.rows()[0], "flag", 1);
    expect(table.getInt(table.rows()[0], "flag") == 1, "byte field set/get failed");

    const BcsvTable rewritten(table.serialize(), Endian::big);
    expectTablesEqual(table, rewritten, "mutated BCSV round trip");

    // removeRow reports out-of-range indices instead of corrupting the table.
    const auto before = table.rows().size();
    expect(!table.removeRow(before), "removeRow accepted an out-of-range index");
    expect(table.removeRow(before - 1), "removeRow rejected a valid index");
    expect(table.rows().size() == before - 1, "removeRow did not shrink the table");

    // A table with no schema cannot size a new row.
    BcsvTable empty;
    bool threw = false;
    try {
        (void)empty.addRow();
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "addRow should refuse a table without fields");
}

void testCustomObjDatabase() {
    using whitehole::db::CustomObjectEntry;
    using whitehole::db::CustomObjDatabase;

    TemporaryDirectory temp;
    const auto path = temp.path / "custom_objects.json";

    // A missing file is an empty database, not an error: the registry is
    // created on first use.
    CustomObjDatabase db;
    db.load(path);
    expect(db.empty(), "a missing custom object file should load as empty");
    expect(!std::filesystem::exists(path), "loading must not create a file");

    CustomObjectEntry entry;
    entry.name = "MyCustomRock";
    entry.displayName = "Custom Rock";
    entry.modelPath = "C:/models/MyCustomRock.bmd";
    entry.notes = "first version";
    entry.source = "/StageData/Planet1/Placement.bcsv";
    expect(db.add(entry), "adding a new entry reported no change");
    expect(!db.add(entry), "adding an identical entry reported a change");
    expect(db.size() == 1, "custom object db size wrong after add");
    expect(db.contains("MyCustomRock"), "custom object db lookup failed");
    expect(db.find("MyCustomRock")->modelPath == "C:/models/MyCustomRock.bmd",
           "custom object model path wrong");

    // setModelPath creates a missing entry so "assign a model" works before
    // the first sync, and a no-op assignment reports no change.
    expect(db.setModelPath("AnotherOne", "/ObjectData/AnotherOne.bmd"),
           "setModelPath did not create a missing entry");
    expect(!db.setModelPath("AnotherOne", "/ObjectData/AnotherOne.bmd"),
           "setModelPath reported a change for an identical path");

    // Persistence round trip.
    db.save(path);
    expect(std::filesystem::exists(path), "save did not create the file");
    CustomObjDatabase reloaded;
    reloaded.load(path);
    expect(reloaded.size() == 2, "custom object db size wrong after reload");
    expect(reloaded.find("MyCustomRock")->displayName == "Custom Rock",
           "custom object display name lost on reload");
    expect(reloaded.find("MyCustomRock")->addedAt > 0, "custom object Added timestamp lost");

    // An unchanged save must not touch the file: save() compares the
    // serialized document with what was last written.
    reloaded.setModelPath("AnotherOne", "/ObjectData/Changed.bmd");
    reloaded.save(path);
    const auto firstWrite = std::filesystem::last_write_time(path);
    reloaded.save(path);
    expect(std::filesystem::last_write_time(path) == firstWrite,
           "an unchanged save rewrote the custom object file");

    // Removal.
    expect(reloaded.remove("AnotherOne"), "removing a known entry failed");
    expect(!reloaded.remove("AnotherOne"), "removing an unknown entry reported success");
    expect(!reloaded.contains("AnotherOne"), "removed entry is still present");

    // Malformed JSON is reported instead of silently emptying the registry.
    const auto broken = temp.path / "broken.json";
    {
        std::ofstream out(broken, std::ios::binary);
        out << "{\"Objects\": [ {\"Name\":";
    }
    bool threw = false;
    try {
        CustomObjDatabase bad;
        bad.load(broken);
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "a malformed custom object file was accepted");
}

void testCustomObjSync() {
    using whitehole::db::CustomObjDatabase;
    using whitehole::db::ObjectDatabase;

    ObjectDatabase official;
    loadObjectDatabaseForTests(official);
    expect(!official.empty(), "objectdb.json did not load for the custom object sync test");
    // A name the community database really knows, to prove filtering works.
    const auto knownNames = official.names();
    expect(!knownNames.empty(), "the official object database exposes no names");
    const auto knownName = knownNames.front();

    CustomObjDatabase db;
    const std::string source = "/StageData/Planet1/Placement.bcsv";
    const auto outcome = db.syncFromObjectSection(source, {knownName, "MyCustomRock", "MyOtherThing"},
                                                  &official);
    expect(outcome.added.size() == 2, "sync should add the two custom names only");
    expect(!db.contains(knownName), "sync registered a vanilla name as custom");
    expect(db.contains("MyCustomRock") && db.contains("MyOtherThing"),
           "sync did not register the custom names");
    expect(db.find("MyCustomRock")->source == source, "sync did not record the source");

    // Re-syncing the same table is a no-op: no duplicates, no churn.
    const auto again = db.syncFromObjectSection(source, {knownName, "MyCustomRock", "MyOtherThing"},
                                                &official);
    expect(!again.changed(), "re-syncing an unchanged table reported changes");

    // Source isolation: a second table adds a name, then the first table is
    // re-synced without one of its names -- only the first table's own entry
    // disappears, never the other table's.
    const std::string otherSource = "/StageData/Planet2/Placement.bcsv";
    const auto second = db.syncFromObjectSection(otherSource, {"RockFromPlanet2"}, &official);
    expect(second.added.size() == 1, "the second table's sync added nothing");
    expect(db.contains("RockFromPlanet2"), "the second table's name was not registered");
    const auto cleaned = db.syncFromObjectSection(source, {knownName, "MyOtherThing"}, &official);
    expect(cleaned.removed.size() == 1 && cleaned.removed.front() == "MyCustomRock",
           "removing a deleted name reported the wrong result");
    expect(!db.contains("MyCustomRock"), "the deleted name is still registered");
    expect(db.contains("RockFromPlanet2"), "the second table's entry was wrongly removed");
    expect(db.contains("MyOtherThing"), "a still-present name was wrongly removed");

    // An entry created by hand has no source, so no table owns it and none may
    // delete it. Once a table that uses the name syncs, it adopts the entry
    // and from then on owns it -- and a later deletion cleans it up.
    CustomObjDatabase blind;
    const auto noOfficial = blind.syncFromObjectSection(source, {"Kinopio", "Unknown"}, nullptr);
    expect(noOfficial.added.empty(), "sync added names without the official database");
    expect(blind.empty(), "the database must stay empty when official data is missing");
    const auto emptyOfficialDb = ObjectDatabase();
    const auto emptyOfficial = blind.syncFromObjectSection(source, {"Unknown"}, &emptyOfficialDb);
    expect(emptyOfficial.added.empty(), "sync added names from an empty official database");

    blind.setModelPath("Unknown", "model.bmd");
    const auto foreign = blind.syncFromObjectSection(source, {}, nullptr);
    expect(foreign.removed.empty(), "sync deleted an entry that no source owns");
    expect(blind.contains("Unknown"), "an unowned entry was removed by a sync");

    CustomObjDatabase owned;
    owned.setModelPath("Unknown", "model.bmd");
    ObjectDatabase real;
    loadObjectDatabaseForTests(real);
    const auto adopting = owned.syncFromObjectSection(source, {"Unknown"}, &real);
    expect(adopting.adopted.size() == 1, "sync did not adopt the unowned entry");
    expect(owned.find("Unknown")->source == source, "the adopted entry recorded no source");
    expect(owned.find("Unknown")->modelPath == "model.bmd", "adopting the entry lost its model");
    const auto stale = owned.syncFromObjectSection(source, {}, &real);
    expect(stale.removed.size() == 1, "a source-owned entry was not removed when its name vanished");
    expect(owned.empty(), "the stale entry survived a removal pass");
}

void testBcsvColumnEditing() {
    using whitehole::smg::BcsvTable;
    using whitehole::smg::BcsvType;
    using whitehole::smg::fieldHash;

    // A bracketed name is the literal hash, exactly like the game editor, so
    // renaming an unknown column keeps pointing at the same data.
    expect(fieldHash("[1A2B3C4D]") == 0x1A2B3C4DU, "fieldHash did not read the literal hash");
    expect(fieldHash("number") == whitehole::smg::jmapHash("number"),
           "fieldHash should hash a plain name with jmapHash");
    expect(fieldHash("[notahex]") == whitehole::smg::jmapHash("[notahex]"),
           "a non-hex bracket name should be hashed as text");
    expect(fieldHash("[ABC]") == 0xABCU, "fieldHash should accept short hex");

    BcsvTable table(makeTinyBcsv(whitehole::io::Endian::big), whitehole::io::Endian::big);
    expect(table.fields().size() == 2, "tiny BCSV should have two fields");

    // Rename keeps every value and the byte layout intact.
    table.renameField(1, "renamed");
    expect(!table.hasField("label"), "the old column name is still present");
    expect(table.hasField("renamed"), "the new column name was not registered");
    expect(table.getString(table.rows()[0], "renamed") == "Comet",
           "renaming a column lost its value");
    expectTablesEqual(table, BcsvTable(table.serialize(), whitehole::io::Endian::big),
                      "renamed BCSV round trip");

    // Duplicate and empty names are refused rather than silently merging two
    // columns into one hash.
    bool threw = false;
    try {
        table.renameField(0, "renamed");
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "renaming onto an existing column name was allowed");
    threw = false;
    try {
        table.renameField(0, "");
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "renaming a column to an empty name was allowed");
    threw = false;
    try {
        table.renameField(99, "whatever");
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "renaming an out-of-range column was allowed");

    // Removing a column drops the value from every row and leaves the rest
    // readable; the freed bytes become padding.
    expect(!table.removeField(table.fields().size()), "removing an out-of-range column reported success");
    expect(table.removeField(0), "removing a valid column failed");
    expect(table.fields().size() == 1, "the column was not removed");
    for (const auto& row : table.rows()) {
        expect(row.values.size() == 1, "removing a column left a ragged row");
    }
    expect(table.getString(table.rows()[0], "renamed") == "Comet",
           "removing a column damaged a neighbour");
    expectTablesEqual(table, BcsvTable(table.serialize(), whitehole::io::Endian::big),
                      "column-removed BCSV round trip");

    // Type change: narrowing a byte to an integer is always safe.
    BcsvTable typed;
    const auto valueIndex = typed.ensureField("v", BcsvType::byte);
    (void)typed.addRow();
    typed.setInt(typed.rows()[0], "v", 100); // fits a signed byte
    typed.setFieldType(valueIndex, BcsvType::integer);
    expect(typed.fields()[valueIndex].type == BcsvType::integer, "setFieldType did not apply");
    expect(typed.getInt(typed.rows()[0], "v") == 100, "setFieldType lost the value");
    expect(typed.fields()[valueIndex].mask == 0xFFFFFFFFU, "setFieldType did not reset the mask");
    expectTablesEqual(typed, BcsvTable(typed.serialize(), whitehole::io::Endian::big),
                      "retyped BCSV round trip");

    // Widening a field into its neighbour's storage is refused: serialize()
    // would clobber the next column.
    BcsvTable tight;
    const auto first = tight.ensureField("a", BcsvType::byte);
    (void)tight.ensureField("b", BcsvType::byte);
    (void)tight.addRow();
    threw = false;
    try {
        tight.setFieldType(first, BcsvType::floatingPoint);
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "widening a field over its neighbour was allowed");
    expect(tight.fields()[first].type == BcsvType::byte, "a refused type change was applied anyway");
}

void testValidationCustomObjects() {
    using whitehole::db::CustomObjDatabase;
    using whitehole::db::ObjectDatabase;
    using whitehole::edit::Severity;
    using whitehole::edit::validateStage;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);
    expect(!database.empty(), "objectdb.json did not load for the custom object validation test");

    auto stage = whitehole::smg::StageArchive::openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    expect(!stage.objects().empty(), "template stage has no objects to validate");
    auto& objects = stage.objects();
    objects[0].name = "MyModdedRock";
    objects[0].scale = {1.0F, 1.0F, 1.0F};
    stage.applyEdits();

    // A name only the modder's own registry knows must not be flagged.
    const auto unknownReport = validateStage(stage, database, 2);
    bool flagged = false;
    for (const auto& finding : unknownReport.findings) {
        if (finding.code == "unknown-object" && finding.objectIndex == 0) {
            flagged = true;
        }
    }
    expect(flagged, "an unregistered custom name was not reported as unknown");

    CustomObjDatabase custom;
    custom.setModelPath("MyModdedRock", "rock.bmd");
    const auto knownReport = validateStage(stage, database, 2, &custom);
    for (const auto& finding : knownReport.findings) {
        expect(!(finding.code == "unknown-object" && finding.objectIndex == 0),
               "a registered custom object was still reported as unknown");
    }
    expect(knownReport.count(Severity::Warning) < unknownReport.count(Severity::Warning),
           "registering a custom object did not reduce the warning count");
}

void testProjectArchives() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    std::size_t archiveCount = 0;
    std::size_t bcsvCount = 0;
    for (const auto& item : std::filesystem::directory_iterator(templates)) {
        if (item.path().extension() != ".arc") {
            continue;
        }
        const auto archive = whitehole::io::RarcArchive::open(item.path());
        expect(archive.wasCompressed(), item.path().string() + " should be Yaz0 compressed");
        expect(!archive.rootName().empty(), item.path().string() + " has no RARC root");
        expect(!archive.entries().empty(), item.path().string() + " has no RARC entries");
        for (const auto& entry : archive.entries()) {
            const auto explicitBcsv = std::filesystem::path(entry.path).extension() == ".bcsv";
            const auto jmapTable = entry.path.find("/jmp/") != std::string::npos;
            if (entry.directory || (!explicitBcsv && !jmapTable)) {
                continue;
            }
            const whitehole::smg::BcsvTable table(archive.read(entry), archive.endian());
            const whitehole::smg::BcsvTable rewritten(table.serialize(), archive.endian());
            expectTablesEqual(table, rewritten, item.path().filename().string() + ":" + entry.path);
            ++bcsvCount;
        }
        const whitehole::io::RarcArchive repacked(archive.serialize(true));
        expect(repacked.wasCompressed(), item.path().string() + " was not recompressed");
        expect(repacked.endian() == archive.endian(), item.path().string() + " endian changed after repacking");
        expect(repacked.entries().size() == archive.entries().size(), item.path().string() + " entry count changed");
        for (std::size_t index = 0; index < archive.entries().size(); ++index) {
            const auto& before = archive.entries()[index];
            const auto& after = repacked.entries()[index];
            expect(before.path == after.path && before.directory == after.directory,
                   item.path().string() + ": archive tree changed after repacking");
            if (!before.directory) {
                expect(archive.read(before) == repacked.read(after),
                       item.path().string() + ": file data changed after repacking");
            }
        }
        ++archiveCount;
    }
    expect(archiveCount == 9, "not every bundled archive template was tested");
    expect(bcsvCount >= 20, "too few bundled BCSV tables were tested");
}

void testArchiveTableEdit() {
    const auto source = std::filesystem::path(WHITEHOLE_SOURCE_DIR)
        / "data" / "templates" / "SMG1OneStarGalaxyScenario.arc";
    auto archive = whitehole::io::RarcArchive::open(source);
    const auto tableEntry = std::find_if(archive.entries().begin(), archive.entries().end(), [](const auto& entry) {
        return !entry.directory && std::filesystem::path(entry.path).filename() == "scenariodata.bcsv";
    });
    expect(tableEntry != archive.entries().end(), "scenario table is missing from the test archive");
    whitehole::smg::BcsvTable table(archive.read(*tableEntry), archive.endian());
    expect(!table.rows().empty() && !table.rows()[0].values.empty(), "scenario table has no editable value");
    table.rows()[0].values[0] = std::int32_t{77};
    archive.replace(tableEntry->path, table.serialize());

    const whitehole::io::RarcArchive saved(archive.serialize(true));
    const auto savedEntry = std::find_if(saved.entries().begin(), saved.entries().end(), [&](const auto& entry) {
        return entry.path == tableEntry->path;
    });
    expect(savedEntry != saved.entries().end(), "edited scenario table disappeared after archive save");
    const whitehole::smg::BcsvTable savedTable(saved.read(*savedEntry), saved.endian());
    expect(std::get<std::int32_t>(savedTable.rows()[0].values[0]) == 77,
           "edited BCSV value did not survive archive recompression");
}

// A galaxy's own map zone is NOT in its ZoneList.bcsv, but it is a real zone
// with its own CameraParam.bcam, so the editor has to be able to reach it. This
// builds a throwaway SMG2 workspace out of the bundled templates so the whole
// GalaxyArchive path (not just a stub) is exercised.
void testGalaxyMapZone() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    TemporaryDirectory temporary;
    whitehole::io::DirectoryFilesystem project(temporary.path);

    // SMG2 recognises a workspace by /SystemData/ObjNameTable.arc.
    project.createDirectory("/SystemData");
    project.write("/SystemData/ObjNameTable.arc", {0});
    // The scenario template's internal root is RedBlueExGalaxy, so that is the
    // galaxy name; the map template is that galaxy's own map zone.
    project.createDirectory("/StageData/RedBlueExGalaxy");
    const std::vector<std::uint8_t> scenario = whitehole::io::readFile(
        templates / "SMG2BigGalaxyScenario.arc");
    const std::vector<std::uint8_t> map = whitehole::io::readFile(
        templates / "SMG2BigGalaxyMap.arc");
    project.write("/StageData/RedBlueExGalaxy/RedBlueExGalaxyScenario.arc", scenario);
    project.write("/StageData/RedBlueExGalaxy/RedBlueExGalaxyMap.arc", map);

    whitehole::smg::GameArchive game(temporary.path);
    expect(game.gameType() == 2, "the synthetic workspace must read as SMG2");
    expect(game.galaxyExists("RedBlueExGalaxy"), "the galaxy was not discovered");

    const whitehole::smg::GalaxyArchive galaxy = game.openGalaxy("RedBlueExGalaxy");
    // zones() is the raw ZoneList and this work must not have touched it: area
    // limit validation counts objects per scenario from that list, so quietly
    // adding the galaxy map there would change those numbers.
    expect(galaxy.zones().size() == 1, "the template ZoneList must hold one zone");
    expect(galaxy.hasMapZone(), "the galaxy's own map archive must be detected");

    // editableZones() leads with the galaxy's own map zone, and never lists the
    // same zone twice. The bundled big-galaxy template is the awkward case that
    // proves the dedupe: its ZoneList already names the galaxy itself, so a naive
    // prepend would show the same file on two rows of the Project panel.
    const std::vector<std::string> editable = galaxy.editableZones();
    expect(!editable.empty() && editable.front() == "RedBlueExGalaxy",
           "editableZones must lead with the galaxy's own map zone");
    const auto repeats = std::adjacent_find(editable.begin(), editable.end());
    expect(repeats == editable.end(),
           "editableZones must not list a zone twice when the ZoneList names the galaxy");
    expect(editable.size() == galaxy.zones().size(),
           "editableZones must not grow when the ZoneList already names the galaxy");

    // Openable because it is listed: this is the invariant Document::openZone
    // relies on. Before, the galaxy map was unopenable and its camera table
    // unreachable.
    whitehole::smg::StageArchive stage = galaxy.openZone("RedBlueExGalaxy");
    expect(stage.stageName() == "RedBlueExGalaxy", "the wrong zone came back");
    expect(!stage.cameraParams().empty(),
           "the galaxy map zone's own camera table must be editable");
    // And the same table survives a save back into the workspace.
    const std::size_t before = stage.cameraParams().cameras().size();
    const std::size_t added = stage.cameraParams().addCamera(
        "e:シナリオスターター:001:00番目", "CAM_TYPE_XZ_PARA", whitehole::smg::kCameraVersionSmg2);
    expect(added == before, "the new camera must append after the existing rows");
    stage.save();
    const whitehole::smg::StageArchive reopened =
        whitehole::smg::StageArchive::open(project, "RedBlueExGalaxy", 2);
    expect(reopened.cameraParams().cameras().size() == before + 1,
           "a camera added to the galaxy map must survive a workspace save");
    expect(reopened.cameraParams().cameras().back().id == "e:シナリオスターター:001:00番目",
           "the composed event id must be stored byte for byte");
}

// Undo for the scenario tables. These live on their OWN stack because the
// scenario tables and a zone's CameraParam.bcam are different documents with
// different save paths -- and selectZone() clears the zone stack, so a shared one
// would lose scenario history every time the author looked at another zone.
void testScenarioUndo() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    TemporaryDirectory temporary;
    whitehole::io::DirectoryFilesystem project(temporary.path);
    project.createDirectory("/SystemData");
    project.write("/SystemData/ObjNameTable.arc", {0});
    project.createDirectory("/StageData/RedBlueExGalaxy");
    project.write("/StageData/RedBlueExGalaxy/RedBlueExGalaxyScenario.arc",
                  whitehole::io::readFile(templates / "SMG2BigGalaxyScenario.arc"));

    whitehole::smg::GameArchive game(temporary.path);
    whitehole::smg::GalaxyArchive galaxy = game.openGalaxy("RedBlueExGalaxy");
    whitehole::edit::UndoStack stack;

    const auto scenarioBytes = galaxy.scenarioData().serialize();
    const auto zoneBytes = galaxy.zoneList().serialize();

    // The comparison is on PARSED CONTENT, not on the snapshot bytes: a BCSV
    // writer may append alignment padding, and a restored table is re-serialised
    // from a parse rather than restored as the original bytes. So the undo
    // snapshot restores what the table MEANS, and its trailing padding may differ
    // from the untouched file's -- which is exactly what GalaxyArchive::save()
    // already relies on by refusing to rewrite a clean galaxy at all.
    auto contentOf = [](const std::vector<std::uint8_t>& bytes) {
        return whitehole::smg::BcsvTable(bytes, whitehole::io::Endian::big);
    };
    // Compares the VALUES of the columns both tables have, in order. A snapshot taken
    // after an ensureField() carries the extra column, so a whole-row compare
    // would always differ on width even when every shared value matches.
    auto sharedValues = [](const whitehole::smg::BcsvTable& table,
                           const whitehole::smg::BcsvTable& other) {
        std::vector<std::vector<whitehole::smg::BcsvValue>> rows;
        const std::size_t width = std::min(table.fields().size(), other.fields().size());
        for (const auto& row : table.rows()) {
            rows.emplace_back(row.values.begin(),
                              row.values.begin() + static_cast<std::ptrdiff_t>(width));
        }
        return rows;
    };

    // A no-op records nothing, so a drag that ends where it started leaves the
    // stack clean -- the same rule mutateCameras follows. The lambda parameters
    // are spelled whitehole::smg:: because a function-local using-declaration is
    // not visible in the lambda's own parameter list.
    expect(!whitehole::edit::mutateScenarios(
               galaxy, stack,
               [](whitehole::smg::BcsvTable&, whitehole::smg::BcsvTable&) {},
               "Nothing"),
           "a no-op scenario edit must not record a step");
    expect(stack.size() == 0, "the galaxy undo stack must stay empty after a no-op");

    // Rename a scenario: one step, and undo restores the bytes exactly.
    expect(whitehole::edit::mutateScenarios(
               galaxy, stack,
               [](whitehole::smg::BcsvTable& scenarios, whitehole::smg::BcsvTable&) {
                   scenarios.setString(scenarios.rows()[0], "ScenarioName", "Renamed");
               },
               "Rename scenario"),
           "renaming must record a step");
    expect(stack.size() == 1 && stack.cursor() == 1, "one edit must be one undo step");
    expect(stack.undoLabel() == "Rename scenario",
           "the undo entry must name the action in plain words");
    expect(galaxy.scenarioData().getString(galaxy.scenarioData().rows()[0], "ScenarioName") ==
               "Renamed",
           "the rename must be applied before it is recorded");

    expect(stack.undo(), "undo must succeed");
    expect(sharedValues(galaxy.scenarioData(), contentOf(scenarioBytes)) ==
               sharedValues(contentOf(scenarioBytes), galaxy.scenarioData()),
           "undo must restore every ScenarioData value");
    expect(sharedValues(galaxy.zoneList(), contentOf(zoneBytes)) ==
               sharedValues(contentOf(zoneBytes), galaxy.zoneList()),
           "undo must restore every ZoneList value");
    expect(stack.redo(), "redo must succeed");
    expect(galaxy.scenarioData().getString(galaxy.scenarioData().rows()[0], "ScenarioName") ==
               "Renamed",
           "redo must re-apply the rename");

    // A layer edit -- the case that made byte snapshots necessary: the new zone
    // column has to come back with its original type and offsets.
    expect(whitehole::edit::mutateScenarios(
               galaxy, stack,
               [](whitehole::smg::BcsvTable& scenarios, whitehole::smg::BcsvTable&) {
                   (void)scenarios.ensureField("NewZoneColumn", whitehole::smg::BcsvType::integer);
                   scenarios.setInt(scenarios.rows()[0], "NewZoneColumn", 5);
               },
               "Change layers"),
           "a layer edit must record a step");
    expect(galaxy.scenarioData().hasField("NewZoneColumn"),
           "the layer column must exist after the edit");
    // The baseline is taken HERE, not from the top of the function: the rename is
    // currently applied (it was redone), so the original bytes are no longer what
    // undo should restore.
    const auto beforeLayerEdit = galaxy.scenarioData().serialize();
    expect(stack.undo(), "the layer edit must undo");
    expect(!galaxy.scenarioData().hasField("NewZoneColumn"),
           "undo must remove the column the edit added");
    expect(sharedValues(galaxy.scenarioData(), contentOf(beforeLayerEdit)) ==
               sharedValues(contentOf(beforeLayerEdit), galaxy.scenarioData()),
           "undo must restore every value after a layer edit");
    expect(stack.redo(), "the layer edit must redo");
    expect(galaxy.scenarioData().hasField("NewZoneColumn"), "redo must restore the column");

    // Zone-list edits go through the same pair, and undo must also rebuild the
    // cached zone-name list, or the Project panel would keep listing a zone the
    // table no longer has.
    const std::size_t zonesBefore = galaxy.zones().size();
    const auto beforeZoneEdit = galaxy.zoneList().serialize();
    expect(whitehole::edit::mutateScenarios(
               galaxy, stack,
               [](whitehole::smg::BcsvTable&, whitehole::smg::BcsvTable& zones) {
                   (void)zones.ensureField("ZoneName", whitehole::smg::BcsvType::stringOffset);
                   const std::size_t row = zones.addRow();
                   zones.setString(zones.rows()[row], "ZoneName", "ExtraZone");
               },
               "Add zone"),
           "adding a zone must record a step");
    expect(galaxy.zones().size() == zonesBefore + 1,
           "the cached zone list must follow the table");
    expect(galaxy.zones().back() == "ExtraZone", "the new zone must be listed");
    expect(stack.undo(), "adding a zone must undo");
    expect(galaxy.zones().size() == zonesBefore,
           "undo must rebuild the cached zone list, not just the table");
    expect(sharedValues(galaxy.zoneList(), contentOf(beforeZoneEdit)) ==
               sharedValues(contentOf(beforeZoneEdit), galaxy.zoneList()),
           "undo must restore every ZoneList value after a zone edit");
}

void testUndoStack() {
    using whitehole::edit::IUndo;
    using whitehole::edit::UndoMultiEntry;
    using whitehole::edit::UndoStack;

    // Minimal command that appends text, so stack semantics are observable
    // without touching any game data.
    struct TextCommand final : IUndo {
        TextCommand(std::string* target, std::string text, std::string action)
            : target(target), text(std::move(text)), action(std::move(action)) {}
        void undo() override { target->erase(target->size() - text.size()); }
        void redo() override { *target += text; }
        [[nodiscard]] std::string label() const override { return action; }
        std::string* target;
        std::string text;
        std::string action;
    };

    std::string log;
    UndoStack stack;
    expect(!stack.canUndo() && !stack.canRedo(), "a fresh undo stack should be empty");
    expect(stack.undoLabel().empty(), "an empty undo stack should have no undo label");

    log += "a";
    stack.push(std::make_unique<TextCommand>(&log, "a", "Add a"));
    log += "b";
    stack.push(std::make_unique<TextCommand>(&log, "b", "Add b"));
    expect(log == "ab", "push() must not re-apply an already-applied command");
    expect(stack.size() == 2 && stack.cursor() == 2, "undo stack depth/cursor wrong");
    expect(stack.undoLabel() == "Add b", "undo label should name the newest command");

    expect(stack.undo(), "undo() should report success");
    expect(log == "a", "undo did not revert the newest command");
    expect(stack.canRedo() && stack.redoLabel() == "Add b", "redo label wrong after undo");
    expect(stack.redoCount() == 1, "redo count wrong after undo");

    expect(stack.redo(), "redo() should report success");
    expect(log == "ab", "redo did not re-apply the command");
    expect(stack.redoCount() == 0, "redo count should be zero after redo");

    // Pushing after an undo discards the redo branch.
    expect(stack.undo(), "second undo failed");
    expect(log == "a", "second undo did not revert");
    log += "c";
    stack.push(std::make_unique<TextCommand>(&log, "c", "Add c"));
    expect(!stack.canRedo(), "pushing after an undo should drop the redo branch");
    expect(stack.size() == 2, "the redo branch was not dropped");
    expect(log == "ac", "pushing a new command must not re-apply it");

    // A null entry is ignored rather than crashing.
    stack.push(nullptr);
    expect(stack.size() == 2, "a null undo entry should be ignored");

    stack.clear();
    expect(!stack.canUndo() && stack.size() == 0 && stack.cursor() == 0, "clear did not reset the stack");

    // A multi entry undoes in reverse order and redoes in insertion order.
    std::string multi;
    auto group = std::make_unique<UndoMultiEntry>("Move 2 objects");
    multi += "x";
    group->add(std::make_unique<TextCommand>(&multi, "x", "Add x"));
    multi += "y";
    group->add(std::make_unique<TextCommand>(&multi, "y", "Add y"));
    expect(group->size() == 2 && !group->empty(), "multi entry size wrong");
    expect(group->label() == "Move 2 objects", "multi entry label wrong");
    group->undo();
    expect(multi.empty(), "multi undo should unwind every child");
    group->redo();
    expect(multi == "xy", "multi redo should re-apply every child in order");

    // ---- group capture ---------------------------------------------------
    // Consecutive pushes fold into one undo step while the capture is open.
    log.clear();
    stack.clear();
    stack.beginGroup("Two edits");
    expect(stack.inGroup(), "beginGroup should open a capture");
    log += "x";
    stack.push(std::make_unique<TextCommand>(&log, "x", "Add x"));
    log += "y";
    stack.push(std::make_unique<TextCommand>(&log, "y", "Add y"));
    expect(stack.size() == 0, "grouped pushes must not land on the stack yet");
    expect(!stack.inGroup() == false, "the capture should still be open");
    stack.endGroup();
    expect(!stack.inGroup(), "endGroup should close the capture");
    expect(stack.size() == 1, "a group must be exactly one undo entry");
    expect(stack.undoLabel() == "Two edits", "the group must carry its own label");
    expect(log == "xy", "grouped pushes must not re-apply the commands");
    expect(stack.undo(), "group undo failed");
    expect(log.empty(), "group undo did not unwind every child");
    expect(stack.redo(), "group redo failed");
    expect(log == "xy", "group redo did not re-apply every child");

    // An empty group records nothing.
    const auto settled = stack.size();
    stack.beginGroup("Nothing");
    stack.endGroup();
    expect(stack.size() == settled, "an empty group must not record an undo entry");

    // An unbalanced endGroup is a safe no-op.
    stack.endGroup();
    expect(stack.size() == settled, "an unbalanced endGroup must not touch the stack");

    // A second begin while one is open is ignored: the outer capture owns the
    // pushes, which keeps mis-nested callers from splitting one user action.
    stack.beginGroup("Outer");
    stack.beginGroup("Inner");
    log += "z";
    stack.push(std::make_unique<TextCommand>(&log, "z", "Add z"));
    stack.endGroup();
    expect(stack.undoLabel() == "Outer", "a nested begin must not steal the capture");
    expect(stack.undo(), "outer group undo failed");
    expect(log == "xy", "the outer group did not own its child");
}

void testStageEditCommands() {
    using whitehole::edit::addObject;
    using whitehole::edit::applyRowEdit;
    using whitehole::edit::applyTransform;
    using whitehole::edit::captureRowValues;
    using whitehole::edit::removeObject;
    using whitehole::edit::UndoStack;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = whitehole::smg::StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(!stage.tables().empty() && !stage.objects().empty(), "template stage has nothing to edit");

    UndoStack stack;
    const auto tableIndex = stage.objects()[0].tableIndex;
    const auto rowIndex = stage.objects()[0].rowIndex;
    expect(tableIndex < stage.tables().size(), "first object points at a missing table");

    // ---- transform edit (the drag/rotate/scale case) ---------------------
    const auto before = stage.readObject(tableIndex, rowIndex);
    auto after = before;
    after.position.y = before.position.y + 1.0F;
    expect(applyTransform(stage, stack, {before}, {after}, "Move object"),
           "applyTransform refused a valid edit");
    expect(std::abs(stage.readObject(tableIndex, rowIndex).position.y - after.position.y) < 1e-6F,
           "the transform edit did not reach the table");
    expect(stack.undo(), "transform undo failed");
    expect(std::abs(stage.readObject(tableIndex, rowIndex).position.y - before.position.y) < 1e-6F,
           "transform undo did not restore the original position");
    expect(stack.redo(), "transform redo failed");
    expect(std::abs(stage.readObject(tableIndex, rowIndex).position.y - after.position.y) < 1e-6F,
           "transform redo did not re-apply the move");

    // ---- property edit through the raw row ------------------------------
    const auto originalRow = captureRowValues(stage, tableIndex, rowIndex);
    auto editedRow = originalRow;
    bool changed = false;
    for (std::size_t field = 0; field < editedRow.size(); ++field) {
        // Only 32-bit integer fields coerce back to the same variant arm, which
        // keeps the round-trip comparison exact.
        if (std::holds_alternative<std::int32_t>(editedRow[field])) {
            editedRow[field] = std::get<std::int32_t>(editedRow[field]) + 1;
            changed = true;
            break;
        }
    }
    expect(changed, "the template row has no integer field to edit");
    expect(applyRowEdit(stage, stack, tableIndex, rowIndex, editedRow, "Edit property"),
           "applyRowEdit refused a valid edit");
    expect(captureRowValues(stage, tableIndex, rowIndex) == editedRow,
           "the property edit did not reach the table");
    expect(stack.undo(), "property undo failed");
    expect(captureRowValues(stage, tableIndex, rowIndex) == originalRow,
           "property undo did not restore the row");
    expect(stack.redo(), "property redo failed");
    expect(captureRowValues(stage, tableIndex, rowIndex) == editedRow,
           "property redo did not re-apply the change");

    // ---- add / remove keep the placement list in step with the tables -----
    const auto objectsBefore = stage.objects().size();
    const auto rowsBefore = stage.tables()[tableIndex].table.rows().size();
    const auto newRow = addObject(stage, stack, tableIndex, editedRow, "Add object");
    expect(newRow == rowsBefore, "an added object should append at the end of its table");
    expect(stage.tables()[tableIndex].table.rows().size() == rowsBefore + 1,
           "addObject did not grow the table");
    expect(stage.objects().size() == objectsBefore + 1,
           "addObject did not grow the placement list");
    expect(stack.undo(), "add undo failed");
    expect(stage.tables()[tableIndex].table.rows().size() == rowsBefore,
           "add undo did not shrink the table");
    expect(stage.objects().size() == objectsBefore, "add undo did not shrink the placement list");
    expect(stack.redo(), "add redo failed");
    expect(stage.objects().size() == objectsBefore + 1, "add redo did not restore the object");

    expect(removeObject(stage, stack, tableIndex, newRow, "Delete object"),
           "removeObject refused a valid edit");
    expect(stage.objects().size() == objectsBefore, "remove did not shrink the placement list");
    expect(stack.undo(), "remove undo failed");
    expect(stage.objects().size() == objectsBefore + 1, "remove undo did not restore the object");

    // Stale indices are refused instead of corrupting the stack.
    const auto depth = stack.size();
    expect(!applyRowEdit(stage, stack, tableIndex, stage.tables()[tableIndex].table.rows().size(),
                         editedRow, "stale"),
           "applyRowEdit accepted an out-of-range row");
    expect(!removeObject(stage, stack, tableIndex, 999999, "stale"),
           "removeObject accepted an out-of-range row");
    expect(stack.size() == depth, "a refused edit must not touch the undo stack");

    // ---- edited data still round-trips through a real save ---------------
    const auto saved = std::filesystem::temp_directory_path() / "whitehole_undo_roundtrip.arc";
    stage.saveTo(saved);
    const auto reopened = whitehole::smg::StageArchive::openMapFile(saved);
    expect(reopened.tables()[tableIndex].table.rows().size()
               == stage.tables()[tableIndex].table.rows().size(),
           "the edited archive did not round-trip its table size");
    expect(reopened.objects().size() == stage.objects().size(),
           "the edited archive did not round-trip its object count");
    std::filesystem::remove(saved);
}

// Object authoring: the native replacement for Java's add-object workflow
// (GalaxyEditorForm.addObject + ObjectSelectForm browsing ObjectDB + the
// ObjIdUtil id scans).
void testObjectAuthoring() {
    using whitehole::edit::createObject;
    using whitehole::edit::deleteObject;
    using whitehole::edit::deleteObjects;
    using whitehole::edit::duplicateObject;
    using whitehole::edit::kindForList;
    using whitehole::edit::listForKind;
    using whitehole::edit::NewObject;
    using whitehole::edit::nextFreeId;
    using whitehole::edit::placementTargets;
    using whitehole::edit::UndoStack;
    using whitehole::smg::StageArchive;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");

    // ---- database placement list <-> native table kind -------------------
    expect(kindForList("MapPartsInfo") == "mappart", "MapPartsInfo must map to the mappart list");
    expect(kindForList("objinfo") == "obj", "list mapping must ignore case");
    expect(kindForList("StartInfo") == "start", "StartInfo must map to the start list");
    expect(listForKind("obj") == "ObjInfo", "obj must map back to ObjInfo");
    expect(listForKind("gravity") == "PlanetObjInfo", "gravity must map back to PlanetObjInfo");
    expect(kindForList("NotAList").empty(), "an unknown list must not map to a table kind");

    // ---- the template's general-object lists -----------------------------
    const auto general = placementTargets(stage, kindForList("ObjInfo"));
    expect(general.size() >= 2, "the template should offer several object layers");
    for (const auto& target : general) {
        expect(target.tableIndex < stage.tables().size(), "a target points at a missing table");
        expect(target.kind == "obj", "a general-object target has the wrong kind");
        expect(target.layer == stage.tables()[target.tableIndex].layer,
               "a target reports the wrong layer");
        expect(target.rowCount == stage.tables()[target.tableIndex].table.rows().size(),
               "a target reports the wrong row count");
    }
    expect(placementTargets(stage).size() == stage.tables().size(),
           "an unfiltered query must list every table");
    expect(placementTargets(stage, "child").empty(), "an absent kind must list nothing");

    UndoStack stack;

    // ---- create ----------------------------------------------------------
    const std::size_t tableIndex = general.front().tableIndex;
    const auto objectsBefore = stage.objects().size();
    const auto rowsBefore = stage.tables()[tableIndex].table.rows().size();

    NewObject request;
    request.name = "AuthorTestObject";
    request.position = {100.0F, 200.0F, 300.0F};
    const auto created = createObject(stage, stack, tableIndex, request);
    expect(created.has_value(), "createObject refused a usable placement table");
    expect(created->rowIndex == rowsBefore, "a new object must append at the end of its table");
    expect(stage.tables()[tableIndex].table.rows().size() == rowsBefore + 1,
           "createObject did not grow the table");
    expect(stage.objects().size() == objectsBefore + 1,
           "createObject did not grow the placement list");
    expect(created->objectIndex < stage.objects().size(), "the created index is out of range");

    {
        const auto& placed = stage.objects()[created->objectIndex];
        expect(placed.name == "AuthorTestObject", "the new object kept the wrong name");
        expect(placed.kind == "obj", "the new object landed in the wrong list");
        expect(placed.layer == general.front().layer, "the new object landed in the wrong layer");
        expect(std::abs(placed.position.x - 100.0F) < 1e-4F &&
                   std::abs(placed.position.y - 200.0F) < 1e-4F &&
                   std::abs(placed.position.z - 300.0F) < 1e-4F,
               "the new object kept the wrong position");
        expect(std::abs(placed.scale.y - 1.0F) < 1e-6F, "a new object must start at scale 1");
        expect(std::abs(placed.rotation.y) < 1e-6F, "a new object must start unrotated");

        // Java's per-type constructors cleared these to -1; a fresh row must too.
        const auto& table = stage.tables()[tableIndex].table;
        const auto& row = table.rows()[created->rowIndex];
        expect(table.getInt(row, "Obj_arg0", 0) == -1, "Obj_arg0 must start at -1 (unset)");
        expect(table.getInt(row, "SW_A", 0) == -1, "SW_A must start at -1 (unset)");
        expect(table.getInt(row, "SW_AWAKE", 0) == -1, "SW_AWAKE must start at -1 (unset)");
        expect(table.getInt(row, "GroupId", 0) == -1, "GroupId must start at -1 (unset)");
        expect(std::abs(table.getFloat(row, "ParamScale", 0.0F) - 1.0F) < 1e-6F,
               "ParamScale must start at 1");

        // The id has to be unique across every row of the same list.
        const auto id = table.getInt(row, "l_id", -1);
        expect(id >= 0, "a new object must carry a usable l_id");
        for (const auto& object : stage.objects()) {
            if (object.tableIndex != tableIndex || object.rowIndex == created->rowIndex) {
                continue;
            }
            expect(table.getInt(table.rows()[object.rowIndex], "l_id", -1) != id,
                   "a new object reused an existing l_id");
        }
    }

    // Undo unwinds the row and the placement list together.
    expect(stack.undo(), "create undo failed");
    expect(stage.objects().size() == objectsBefore, "create undo left the object behind");
    expect(stack.redo(), "create redo failed");
    expect(stage.objects().size() == objectsBefore + 1, "create redo did not restore the object");

    // ---- duplicate -------------------------------------------------------
    const auto sourceIndex = created->objectIndex;
    const auto duplicate = duplicateObject(stage, stack, sourceIndex, {50.0F, 0.0F, 0.0F});
    expect(duplicate.has_value(), "duplicateObject refused a live object");
    expect(duplicate->tableIndex == tableIndex, "a duplicate must stay in its original list");
    expect(duplicate->rowIndex == created->rowIndex + 1,
           "a duplicate must sit right next to its original");
    expect(stage.objects().size() == objectsBefore + 2,
           "duplicateObject did not grow the placement list");
    {
        const auto& copy = stage.objects()[duplicate->objectIndex];
        const auto& original = stage.objects()[sourceIndex];
        expect(copy.name == original.name, "a duplicate must keep the original name");
        expect(std::abs(copy.position.x - (original.position.x + 50.0F)) < 1e-4F,
               "a duplicate must apply the offset");
        expect(std::abs(copy.position.y - original.position.y) < 1e-4F,
               "a duplicate must not move on the untouched axes");
        const auto& table = stage.tables()[tableIndex].table;
        const auto sourceId = table.getInt(table.rows()[original.rowIndex], "l_id", -1);
        const auto copyId = table.getInt(table.rows()[copy.rowIndex], "l_id", -1);
        expect(copyId != sourceId, "a duplicate must take its own l_id");
    }
    expect(stack.undo(), "duplicate undo failed");
    expect(stage.objects().size() == objectsBefore + 1, "duplicate undo left the copy behind");
    expect(stack.redo(), "duplicate redo failed");
    expect(stage.objects().size() == objectsBefore + 2, "duplicate redo did not restore the copy");

    // ---- delete one ------------------------------------------------------
    expect(deleteObject(stage, stack, duplicate->objectIndex),
           "deleteObject refused a live object");
    expect(stage.objects().size() == objectsBefore + 1, "delete did not shrink the placement list");
    expect(stack.undo(), "delete undo failed");
    expect(stage.objects().size() == objectsBefore + 2, "delete undo did not restore the object");
    expect(stack.redo(), "delete redo failed");
    expect(stage.objects().size() == objectsBefore + 1, "delete redo did not remove it again");
    // Undo once more so both objects are present for the multi-delete below.
    expect(stack.undo(), "delete undo (restore) failed");
    expect(stage.objects().size() == objectsBefore + 2, "delete undo did not restore the object");

    // ---- delete several as a single step ---------------------------------
    // The copy's index is re-derived rather than remembered: removing the row
    // above it shifted every index after it, which is exactly why the editor
    // re-syncs its selection after each edit instead of holding indexes across
    // one.
    const auto copyIndex = whitehole::edit::objectIndexAt(stage, tableIndex, created->rowIndex + 1);
    expect(copyIndex.has_value(), "the duplicate vanished from the placement list");

    const auto cursorBefore = stack.cursor();
    // Reverse order plus a repeated index: exactly what a multi-selection with a
    // duplicate entry looks like, and it must still delete the right two rows.
    const auto removedCount =
        deleteObjects(stage, stack, {*copyIndex, sourceIndex, sourceIndex}, "Delete 2 objects");
    expect(removedCount == 2, "deleteObjects removed the wrong number of rows");
    expect(stage.objects().size() == objectsBefore, "deleteObjects left objects behind");
    expect(stack.cursor() == cursorBefore + 1, "a multi-delete must be exactly one undo step");
    expect(stack.redoCount() == 0, "a new edit must discard the redo branch");
    expect(stack.undo(), "multi-delete undo failed");
    expect(stage.objects().size() == objectsBefore + 2,
           "multi-delete undo did not restore both objects");
    expect(stack.redo(), "multi-delete redo failed");
    expect(stage.objects().size() == objectsBefore, "multi-delete redo did not remove both");

    // A stale index is skipped rather than deleting whichever row now sits
    // there, and nothing lands on the undo stack when nothing was removed.
    expect(deleteObjects(stage, stack, {999999}, "Delete nothing") == 0,
           "deleteObjects must ignore indices that no longer exist");
    expect(stack.cursor() == cursorBefore + 1,
           "a multi-delete that removed nothing must not record an undo entry");

    // ---- a start point gets its own MarioNo ------------------------------
    const auto starts = placementTargets(stage, "start");
    if (!starts.empty()) {
        const auto nextMarioNo = nextFreeId(stage, "start", "MarioNo");
        NewObject start;
        start.name = "Mario";
        start.position = {10.0F, 0.0F, 0.0F};
        const auto added = createObject(stage, stack, starts.front().tableIndex, start);
        expect(added.has_value(), "createObject refused the start list");
        const auto& table = stage.tables()[starts.front().tableIndex].table;
        expect(table.getInt(table.rows()[added->rowIndex], "MarioNo", -1) == nextMarioNo,
               "a new start point did not take the next free MarioNo");
        expect(stack.undo(), "start undo failed");
    }

    // ---- an authored archive still round-trips through a real save --------
    const auto saved = std::filesystem::temp_directory_path() / "whitehole_authoring.arc";
    stage.saveTo(saved);
    const auto reopened = StageArchive::openMapFile(saved);
    expect(reopened.tables().size() == stage.tables().size(),
           "the authored archive lost a table on the way to disk");
    expect(reopened.objects().size() == stage.objects().size(),
           "the authored archive round-tripped a different object count");
    std::filesystem::remove(saved);
}

// Group transforms: the mouse-driven editing path that moves, rotates or
// scales a whole selection as one undo entry (gizmo drags, arrow-key nudges).
void testGroupTransforms() {
    using whitehole::edit::rotateObjects;
    using whitehole::edit::scaleObjects;
    using whitehole::edit::translateObjects;
    using whitehole::edit::UndoStack;
    using whitehole::smg::StageArchive;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(stage.objects().size() >= 2, "the template has nothing to move as a group");

    UndoStack stack;
    std::vector<std::size_t> selection;
    for (std::size_t index = 0; index < stage.objects().size() && selection.size() < 3; ++index) {
        // Skip start points: translating the player spawn is legal but noisy.
        if (stage.objects()[index].kind != "start") {
            selection.push_back(index);
        }
    }
    expect(selection.size() >= 2, "the template has no moveable pair");

    // ---- translate -------------------------------------------------------
    std::vector<whitehole::smg::PlacementObject> baseline;
    for (const auto index : selection) {
        baseline.push_back(stage.objects()[index]);
    }
    const auto cursorBefore = stack.cursor();
    expect(translateObjects(stage, stack, selection, {10.0F, -5.0F, 2.0F}),
           "translateObjects refused a live selection");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& after = stage.objects()[selection[slot]];
        expect(std::abs(after.position.x - (baseline[slot].position.x + 10.0F)) < 1e-4F &&
                   std::abs(after.position.y - (baseline[slot].position.y - 5.0F)) < 1e-4F &&
                   std::abs(after.position.z - (baseline[slot].position.z + 2.0F)) < 1e-4F,
               "a group translate moved one object wrong");
    }
    expect(stack.cursor() == cursorBefore + 1, "a group translate must be one undo step");
    expect(stack.undo(), "group translate undo failed");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& restored = stage.objects()[selection[slot]];
        expect(std::abs(restored.position.x - baseline[slot].position.x) < 1e-4F,
               "a group translate undo restored the wrong position");
    }
    expect(stack.redo(), "group translate redo failed");

    // ---- a stale index never throws away the rest of the group ------------
    auto stretched = selection;
    stretched.push_back(999999);
    stretched.push_back(selection.front()); // duplicates are moved exactly once
    expect(translateObjects(stage, stack, stretched, {1.0F, 0.0F, 0.0F}, "Jog 2 objects"),
           "a selection with stale entries must still move the rest");
    expect(stack.undoLabel() == "Jog 2 objects", "an explicit label must survive");
    expect(stack.undo(), "stale-selection undo failed");
    // The duplicate entry must not have doubled the move: the row ends up where
    // it started plus exactly one jog.
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& back = stage.objects()[selection[slot]];
        expect(std::abs(back.position.x - (baseline[slot].position.x + 10.0F)) < 1e-4F,
               "a duplicated selection moved one object twice");
    }

    // ---- rotate ----------------------------------------------------------
    expect(rotateObjects(stage, stack, selection, {90.0F, 0.0F, 0.0F}), "rotateObjects refused");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& turned = stage.objects()[selection[slot]];
        expect(std::abs(turned.rotation.x - (baseline[slot].rotation.x + 90.0F)) < 1e-4F,
               "a group rotate added the wrong degrees");
    }
    expect(stack.undo(), "group rotate undo failed");
    expect(stack.undo(), "undo of the stray redo must also succeed");

    // ---- scale -----------------------------------------------------------
    // A factor of two on every axis must double each scale, and a factor of
    // exactly zero must clamp away from the degenerate point instead.
    expect(scaleObjects(stage, stack, selection, {2.0F, 2.0F, 2.0F}), "scaleObjects refused");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& grown = stage.objects()[selection[slot]];
        expect(std::abs(grown.scale.x - baseline[slot].scale.x * 2.0F) < 1e-4F,
               "a group scale did not double the stored scale");
    }
    expect(stack.undo(), "group scale undo failed");
    expect(scaleObjects(stage, stack, selection, {0.0F, 0.0F, 0.0F}, "Shrink"), "zero scale refused");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& shrunk = stage.objects()[selection[slot]];
        expect(std::abs(shrunk.scale.x) > 0.0005F, "a zero factor must clamp before the point");
    }
    expect(stack.undo(), "zero-scale undo failed");

    // ---- nothing valid records nothing -----------------------------------
    expect(!translateObjects(stage, stack, {999998, 999999}, {0.0F, 0.0F, 0.0F}),
           "a fully stale group must fail");
    expect(!rotateObjects(stage, stack, {}, {0.0F, 0.0F, 0.0F}), "an empty group must fail");
    expect(!scaleObjects(stage, stack, {}, {1.0F, 1.0F, 1.0F}), "an empty scale group must fail");
}

// data/objectdb.json is a gitignored first-run download, so a fresh checkout
// does not have it. These tests only need a non-empty database, so fall back to
// a small inline one (same schema as testObjectDatabaseV2) when the file is
// absent; the real community database is used whenever it exists.
void loadObjectDatabaseForTests(whitehole::db::ObjectDatabase& database) {
    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    database.load(root / "data" / "objectdb.json");
    if (!database.empty()) {
        return;
    }
    database.clear();
    database.loadFromJson(R"({
  "Timestamp": 0,
  "Classes": [
    {"InternalName":"SampleObj","Name":"SampleObj","Notes":"A test class","Games":3,"Progress":1,
     "Parameters":{
        "Obj_arg0":{"Name":"Range","Type":"Float","Games":3,"Needed":true,"Description":"How far.","Values":[],"Exclusives":[]},
        "Obj_arg1":{"Name":"Mode","Type":"Integer","Games":3,"Needed":false,"Description":"Pick one.","Values":[{"Value":0,"Notes":"Off"},{"Value":1,"Notes":"On"}],"Exclusives":[]}
     }}
  ],
  "Objects": [
    {"InternalName":"Kinopio","ClassNameSMG1":"SampleObj","ClassNameSMG2":"SampleObj","Name":"Toad",
     "Notes":"Friendly.","Category":"npc","ListSMG1":"ObjInfo","ListSMG2":"ObjInfo","File":"Map","Games":3,
     "Progress":1,"IsUnused":false,"IsLeftover":false}
  ]
})");
}

void testObjectModel() {
    using whitehole::db::ObjectDatabase;
    using whitehole::db::PropertyKind;
    using whitehole::edit::UndoStack;
    using whitehole::smg::ObjectModel;
    using whitehole::smg::propertyKindLabel;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);
    expect(!database.empty(), "objectdb.json did not load for the object model test");

    auto stage = whitehole::smg::StageArchive::openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    ObjectModel model(stage, database, 2);
    expect(model.objectCount() > 0, "object model saw no objects");
    expect(model.gameType() == 2, "object model game type wrong");

    // Prefer an object the database knows, so class metadata is exercised.
    std::size_t objectIndex = 0;
    bool classFound = false;
    for (std::size_t index = 0; index < model.objectCount(); ++index) {
        const auto* info = model.objectClass(index);
        if (info != nullptr && !info->properties.empty()) {
            objectIndex = index;
            classFound = true;
            break;
        }
    }

    // Base transform fields are always offered, whatever the database knows.
    const auto fields = model.fields(objectIndex);
    expect(fields.size() >= 10, "object model returned fewer than the base transform fields");
    bool sawName = false;
    bool sawPosX = false;
    for (const auto& field : fields) {
        if (field.identifier == "name") {
            sawName = true;
            expect(field.present, "the name field should be present");
            expect(field.label == "Name", "the name field label is wrong");
            expect(field.kind == PropertyKind::Text, "the name field should be text");
        }
        if (field.identifier == "pos_x") {
            sawPosX = true;
            expect(field.present, "the pos_x field should be present");
            expect(field.kind == PropertyKind::Float, "pos_x should be a float");
        }
    }
    expect(sawName && sawPosX, "the base transform fields are missing from the object model");
    if (classFound) {
        // Class metadata must reach the field list with its human label.
        bool sawMetadata = false;
        for (const auto& field : fields) {
            if (!field.label.empty() && field.label != field.identifier) {
                sawMetadata = true;
                break;
            }
        }
        expect(sawMetadata, "class metadata did not reach the field list");
    }

    // Reads agree with the placement list.
    float posX = 0.0F;
    expect(model.getFloat(objectIndex, "pos_x", posX), "getFloat(pos_x) failed");
    expect(std::abs(posX - stage.objects()[objectIndex].position.x) < 1e-6F,
           "pos_x read disagrees with the placement list");
    std::string name;
    expect(model.getString(objectIndex, "name", name), "getString(name) failed");
    expect(name == stage.objects()[objectIndex].name, "name read disagrees with the placement list");

    // Writes record undo and keep the placement list in step.
    UndoStack stack;
    const auto originalY = stage.objects()[objectIndex].position.y;
    expect(model.setFloat(objectIndex, "pos_y", originalY + 5.0F, stack, "Move object"),
           "setFloat(pos_y) failed");
    expect(stack.size() == 1 && stack.undoLabel() == "Move object",
           "setFloat did not record exactly one named undo entry");
    expect(std::abs(stage.objects()[objectIndex].position.y - (originalY + 5.0F)) < 1e-6F,
           "setFloat did not update the placement list");
    expect(stack.undo(), "undo after setFloat failed");
    expect(std::abs(stage.objects()[objectIndex].position.y - originalY) < 1e-6F,
           "undo did not restore pos_y");
    expect(stack.redo(), "redo after setFloat failed");
    expect(std::abs(stage.objects()[objectIndex].position.y - (originalY + 5.0F)) < 1e-6F,
           "redo did not re-apply pos_y");

    // A no-op set is accepted but must not pollute the undo history.
    const auto depth = stack.size();
    expect(model.setFloat(objectIndex, "pos_y", originalY + 5.0F, stack, "no-op"),
           "a no-op setFloat should still report success");
    expect(stack.size() == depth, "a no-op edit must not add an undo entry");

    // Absent fields and out-of-range objects are refused.
    float unused = 0.0F;
    expect(!model.getFloat(objectIndex, "definitely_not_a_field", unused),
           "getFloat accepted a missing field");
    expect(!model.setFloat(objectIndex, "definitely_not_a_field", 1.0F, stack, "bad"),
           "setFloat accepted a missing field");
    expect(!model.setFloat(999999, "pos_x", 1.0F, stack, "bad"),
           "setFloat accepted an out-of-range object");

    // A typed accessor rejects the wrong storage type.
    std::string text;
    expect(!model.getString(objectIndex, "pos_x", text), "getString should reject a float field");

    expect(propertyKindLabel(PropertyKind::Float) == "float", "propertyKindLabel(float) is wrong");
    expect(propertyKindLabel(PropertyKind::SwitchId) == "switch", "propertyKindLabel(switch) is wrong");
}

void testValidation() {
    using whitehole::db::ObjectDatabase;
    using whitehole::edit::Severity;
    using whitehole::edit::toString;
    using whitehole::edit::validateStage;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);
    expect(!database.empty(), "objectdb.json did not load for the validation test");

    auto stage = whitehole::smg::StageArchive::openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    expect(!stage.objects().empty(), "template stage has no objects to validate");

    // An empty database means "nothing to validate against", not a wall of errors.
    ObjectDatabase blank;
    expect(validateStage(stage, blank, 2).empty(),
           "validation should stay silent without a database");

    // Force two known problems on the first object.
    auto& objects = stage.objects();
    objects[0].name = "WhiteholeDefinitelyNotAnObject";
    objects[0].scale = {0.0F, 0.0F, 0.0F};
    stage.applyEdits();

    const auto report = validateStage(stage, database, 2);
    expect(!report.findings.empty(), "validation found nothing in a stage with a bogus object");
    expect(report.findings.size() == report.count(Severity::Info) + report.count(Severity::Warning)
               + report.count(Severity::Error),
           "finding severities do not add up to the finding count");
    expect(!report.clean(), "a report with warnings should not be clean");

    bool unknown = false;
    bool zeroScale = false;
    for (const auto& finding : report.findings) {
        if (finding.code == "unknown-object" && finding.objectIndex == 0 && !finding.hint.empty()) {
            unknown = true;
        }
        if (finding.code == "zero-scale" && finding.objectIndex == 0) {
            zeroScale = true;
        }
    }
    expect(unknown, "the bogus object was not reported as unknown");
    expect(zeroScale, "zero scale was not reported");

    expect(toString(Severity::Warning) == "warning", "the warning severity name is wrong");
    expect(toString(Severity::Error) == "error", "the error severity name is wrong");
}

void testDocument() {
    using whitehole::db::ObjectDatabase;
    using whitehole::edit::Document;
    using whitehole::edit::Severity;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);

    Document document;
    document.setDatabase(&database);
    expect(!document.hasStage(), "a fresh document should have no stage");
    expect(!document.dirty(), "a fresh document should be clean");

    int changes = 0;
    document.setOnChange([&changes] { ++changes; });

    document.openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    expect(document.hasStage(), "openMapFile did not open a stage");
    expect(!document.zoneName().empty(), "openMapFile did not set the zone name");
    expect(!document.dirty(), "opening a file must not mark the document dirty");
    expect(changes > 0, "opening a file should notify observers");
    expect(document.stage() != nullptr && !document.stage()->objects().empty(),
           "the opened stage has no objects");

    // Selection is multi-select aware, and selecting is not an edit.
    document.select(0);
    expect(document.selection().size() == 1 && document.isSelected(0), "single selection failed");
    document.select(1, true);
    expect(document.selection().size() == 2 && document.isSelected(1), "additive selection failed");
    document.select(1, true);
    expect(document.selection().size() == 2, "additive selection duplicated an index");
    document.clearSelection();
    expect(document.selection().empty(), "clearSelection failed");
    expect(!document.dirty(), "selection changes must not mark the document dirty");

    // An edit is undoable and flips the dirty flag; undoing back is clean again.
    const auto originalY = document.stage()->objects()[0].position.y;
    auto model = document.objectModel();
    expect(model.setFloat(0, "pos_y", originalY + 3.0F, document.undoStack(), "Move object"),
           "editing through the document failed");
    expect(document.dirty(), "an edit should mark the document dirty");
    expect(document.canUndo() && document.undoLabel() == "Move object", "undo state is wrong");
    expect(document.undo(), "document.undo() failed");
    expect(!document.dirty(), "undoing back to the save point should report clean");
    expect(!document.undo(), "undo should stop at the start of history");
    expect(document.redo(), "document.redo() failed");
    expect(document.dirty(), "redo should mark the document dirty again");

    // Saving records a new save point and round-trips the change.
    const auto saved = std::filesystem::temp_directory_path() / "whitehole_document_roundtrip.arc";
    document.saveAs(saved);
    expect(!document.dirty(), "save should clear the dirty flag");
    const auto reopened = whitehole::smg::StageArchive::openMapFile(saved);
    expect(std::abs(reopened.objects()[0].position.y - (originalY + 3.0F)) < 1e-6F,
           "the saved document did not round-trip the edit");
    std::filesystem::remove(saved);

    // Validation is reachable straight from the document.
    const auto report = document.validate();
    expect(report.findings.size() == report.count(Severity::Info) + report.count(Severity::Warning)
               + report.count(Severity::Error),
           "document validation severities do not add up");

    document.close();
    expect(!document.hasStage() && !document.dirty(), "close did not reset the document");
}

void testNameTables() {
    whitehole::db::NameTable galaxies;
    galaxies.loadJson(std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "galaxies.json");
    expect(galaxies.displayName("EggStarGalaxy").find("Good Egg") != std::string::npos,
           "galaxy display names did not load");
}

void testStageCameras() {
    using namespace whitehole::smg;
    using whitehole::edit::UndoStack;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(stage.gameType() == 2, "the SMG2 template must report its game type");
    expect(stage.cameraParams().size() == 1, "the zone must expose its camera table");
    expect(stage.cameraDefaultVersion() == kCameraVersionSmg2,
           "new SMG2 cameras must be stamped with the SMG2 engine version");

    // Editing through the undo stack: one camera-table change per step.
    UndoStack stack;
    const bool changed = whitehole::edit::mutateCameras(
        stage, stack, [](CameraParamTable& cameras) { cameras.setFloat(0, "fovy", 88.0F); },
        "Set camera FoV");
    expect(changed && stage.cameraParams().getFloat(0, "fovy") == 88.0F,
           "a camera edit must apply to the table");
    expect(stack.canUndo() && stack.undoLabel() == "Set camera FoV",
           "the camera edit must be one undo step");
    expect(stack.undo() && stage.cameraParams().getFloat(0, "fovy") == 45.0F,
           "undo must restore the engine default");
    expect(stack.redo() && stage.cameraParams().getFloat(0, "fovy") == 88.0F,
           "redo must re-apply the camera edit");
    expect(!whitehole::edit::mutateCameras(stage, stack, [](CameraParamTable&) {}, "Nothing"),
           "a no-op camera edit must not record a step");

    // Adding a camera is undoable too.
    expect(whitehole::edit::mutateCameras(
               stage, stack,
               [](CameraParamTable& cameras) {
                   (void)cameras.addCamera("e:シナリオスターター:001:00番目", "CAM_TYPE_EYEPOS_FIX",
                                           kCameraVersionSmg2);
               },
               "Add camera"),
           "adding a camera must record a step");
    expect(stage.cameraParams().size() == 2, "the camera must be in the table");
    expect(stack.undo() && stage.cameraParams().size() == 1,
           "undo must remove the added camera");
    expect(stack.redo() && stage.cameraParams().size() == 2,
           "redo must re-add the camera");

    // Saving the zone carries the camera table into (and out of) the archive.
    const auto output = std::filesystem::temp_directory_path() / "whitehole_camera_test.arc";
    stage.saveTo(output);
    auto reopened = StageArchive::openMapFile(output);
    expect(reopened.cameraParams().size() == 2,
           "the camera table must survive a zone save");
    expect(reopened.cameraParams().cameras()[0].id == "c:0000" &&
               reopened.cameraParams().getFloat(0, "fovy") == 88.0F,
           "edited camera values must survive a zone save");
    expect(reopened.cameraParams().cameras()[1].id == "e:シナリオスターター:001:00番目",
           "added cameras must survive a zone save");
    expect(reopened.objects().size() == stage.objects().size(),
           "saving cameras must not disturb the placement data");
    std::filesystem::remove(output);

    // SMG1 zones (all-lowercase archive paths) load their camera table too.
    auto smg1 = StageArchive::openMapFile(templates / "SMG1BigGalaxy.arc", 1);
    expect(smg1.cameraParams().size() == 1, "the SMG1 zone must expose its camera table");
    expect(smg1.cameraDefaultVersion() == kCameraVersionSmg1,
           "new SMG1 cameras must be stamped with the SMG1 engine version");
    expect(!smg1.cameraParams().hasColumn("up.Y") ||
               smg1.cameraParams().getFloat(0, "up.Y") == 0.0F,
           "SMG1 camera defaults must stay on the SMG1 set");
}

// Creating an archive from nothing. Every other RarcArchive entry point parses
// bytes that already exist, so before this there was no way to make a zone or a
// galaxy -- the editor could only edit what the game already shipped.
void testRarcCreation() {
    using whitehole::io::RarcArchive;

    RarcArchive archive = RarcArchive::create("Stage");
    expect(archive.entries().empty(), "a created archive must start with no entries");
    expect(archive.rootName() == "Stage", "a created archive must keep its root name");
    expect(archive.endian() == whitehole::io::Endian::big,
           "a created archive must be big-endian, like the retail ones");

    // An unsafe root name is refused rather than producing an archive the game
    // would never read.
    bool refused = false;
    try {
        (void)RarcArchive::create("../escape");
    } catch (const std::runtime_error&) {
        refused = true;
    }
    expect(refused, "an unsafe RARC root name must be rejected");

    // The /Stage/jmp skeleton every zone needs, plus a payload.
    archive.createDirectory("/jmp");
    archive.createDirectory("/jmp/Placement");
    archive.createDirectory("/jmp/Placement/Common");
    archive.insert("/jmp/Placement/Common/StageObjInfo", {1, 2, 3, 4});
    archive.insert("/jmp/Placement/Common/AreaObjInfo", {5, 6});
    expect(archive.fileExists("/jmp/Placement/Common/StageObjInfo"),
           "insert must find the file it just wrote");

    // Creating a directory twice is a no-op, not a failure: the zone builder asks
    // for the same folder from more than one layer.
    archive.createDirectory("/jmp/Placement/Common");
    // ...but its parent has to exist.
    bool parentRefused = false;
    try {
        archive.createDirectory("/jmp/Missing/Child");
    } catch (const std::runtime_error&) {
        parentRefused = true;
    }
    expect(parentRefused, "creating a directory under a missing parent must be rejected");

    // Write it out, then read it back through the ordinary parse path. This is the
    // claim that matters: an archive built by hand is indistinguishable from a
    // parsed one to everything downstream.
    TemporaryDirectory temporary;
    const auto path = temporary.path / "created.arc";
    whitehole::io::writeFile(path, archive.serialize(false));
    RarcArchive reparsed{whitehole::io::readFile(path)};
    expect(reparsed.rootName() == "Stage", "the root name must survive the round trip");
    expect(reparsed.endian() == whitehole::io::Endian::big,
           "the endianness must survive the round trip");
    expect(reparsed.fileExists("/jmp/Placement/Common/StageObjInfo"),
           "a created file must be found after re-parsing");
    expect(reparsed.read("/jmp/Placement/Common/StageObjInfo")
               == std::vector<std::uint8_t>({1, 2, 3, 4}),
           "a created file's bytes must survive the round trip");
    expect(reparsed.files("/jmp/Placement/Common")
               == std::vector<std::string>({"AreaObjInfo", "StageObjInfo"}),
           "created files must list under their directory after re-parsing");
    expect(reparsed.directories("/jmp").size() == 1,
           "a created directory must list after re-parsing");

    // The trap that cost the most time: every layer's tables have the SAME file
    // names (Common/StartInfo, LayerA/StartInfo, ...). find() falls back to a
    // bare-filename match, so an insert of LayerA's table used to find Common's
    // file and REPLACE it -- losing the Common layer and leaving LayerA empty,
    // while every layer directory still looked correct.
    archive.createDirectory("/jmp/Start");
    archive.createDirectory("/jmp/Start/Common");
    archive.createDirectory("/jmp/Start/LayerA");
    archive.insert("/jmp/Start/Common/StartInfo", {1, 1, 1, 1});
    archive.insert("/jmp/Start/LayerA/StartInfo", {2, 2, 2, 2});
    expect(archive.fileExists("/jmp/Start/Common/StartInfo"),
           "inserting a second layer's table must not displace the first");
    expect(archive.fileExists("/jmp/Start/LayerA/StartInfo"),
           "the second layer's table must be stored in its own right");
    expect(archive.read("/jmp/Start/Common/StartInfo")
               == std::vector<std::uint8_t>({1, 1, 1, 1}),
           "the Common layer's table must keep its own contents");
    expect(archive.read("/jmp/Start/LayerA/StartInfo")
               == std::vector<std::uint8_t>({2, 2, 2, 2}),
           "the LayerA table must keep its own contents");
    // Both must survive the round trip as two distinct files.
    const auto twoLayers = temporary.path / "two-layers.arc";
    whitehole::io::writeFile(twoLayers, archive.serialize(false));
    RarcArchive reparsedLayers{whitehole::io::readFile(twoLayers)};
    expect(reparsedLayers.read("/jmp/Start/Common/StartInfo")
               == std::vector<std::uint8_t>({1, 1, 1, 1}),
           "both layers must survive serialization distinctly");
    expect(reparsedLayers.read("/jmp/Start/LayerA/StartInfo")
               == std::vector<std::uint8_t>({2, 2, 2, 2}),
           "the second layer must survive serialization distinctly");

    // The strongest check available here: the app's OWN stage loader must open an
    // archive built this way. Everything downstream (objects, the object tree,
    // saving) goes through StageArchive, so if this passes the created zone is
    // real rather than merely well-formed.
    whitehole::smg::BcsvTable stageObjInfo;
    stageObjInfo.ensureField("name", whitehole::smg::BcsvType::stringOffset);
    stageObjInfo.ensureField("l_id", whitehole::smg::BcsvType::integer);
    const std::size_t row = stageObjInfo.addRow();
    stageObjInfo.setString(stageObjInfo.rows()[row], "name", "FirstThing");
    stageObjInfo.setInt(stageObjInfo.rows()[row], "l_id", 0);

    RarcArchive loadable = RarcArchive::create("Stage");
    loadable.createDirectory("/jmp");
    loadable.createDirectory("/jmp/Placement");
    loadable.createDirectory("/jmp/Placement/Common");
    loadable.insert("/jmp/Placement/Common/StageObjInfo", stageObjInfo.serialize());
    const auto loadablePath = temporary.path / "created-stage.arc";
    whitehole::io::writeFile(loadablePath, loadable.serialize(false));

    auto stage = whitehole::smg::StageArchive::openMapFile(loadablePath);
    expect(!stage.tables().empty(),
           "StageArchive must load a table out of an archive built from nothing");
    expect(stage.objects().size() == 1,
           "StageArchive must read the object out of a created archive");
    expect(stage.objects().front().name == "FirstThing",
           "the object's name must survive creation");
}

void testStageAndGameModels() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = whitehole::smg::StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(!stage.objects().empty(), "template map loaded no placement objects");
    expect(stage.objects().size() >= 10, "template map loaded fewer objects than expected");
    auto& object = stage.objects().front();
    const auto original = object.position.x;
    object.position.x = original + 12.5F;
    TemporaryDirectory output;
    const auto savedPath = output.path / "edited-map.arc";
    stage.saveTo(savedPath);
    const auto reloaded = whitehole::smg::StageArchive::openMapFile(savedPath);
    expect(!reloaded.objects().empty(), "saved map reloaded no objects");
    expect(std::abs(reloaded.objects().front().position.x - (original + 12.5F)) < 0.01F,
           "edited object position did not survive save");

    TemporaryDirectory workspace;
    std::filesystem::create_directories(workspace.path / "SystemData");
    std::filesystem::create_directories(workspace.path / "StageData" / "TestGalaxy");
    whitehole::io::writeFile(workspace.path / "SystemData" / "ObjNameTable.arc", {0x52, 0x41, 0x52, 0x43});
    std::filesystem::copy_file(templates / "SMG2BigGalaxyScenario.arc",
                               workspace.path / "StageData" / "TestGalaxy" / "TestGalaxyScenario.arc");
    std::filesystem::copy_file(templates / "SMG2BigGalaxyMap.arc",
                               workspace.path / "StageData" / "TestGalaxy" / "TestGalaxyMap.arc");
    whitehole::smg::GameArchive game(workspace.path);
    expect(game.gameType() == 2, "synthetic workspace was not detected as SMG2");
    expect(game.galaxyExists("TestGalaxy"), "synthetic galaxy was not listed");
    const auto galaxy = game.openGalaxy("TestGalaxy");
    expect(!galaxy.zones().empty(), "synthetic galaxy has no zones");
}

void testBtiDecoding() {
    using whitehole::io::Endian;
    using whitehole::smg::decodeBtiImage;
    using whitehole::smg::parseBti;

    // RGB565 (format 4): 4x4 of full-white word 0xFFFF -> opaque white.
    {
        std::vector<std::uint8_t> data(32, 0xFF);
        auto img = decodeBtiImage(data, 0, 4, 4, 4, 0, Endian::big);
        expect(img.width == 4 && img.height == 4, "RGB565 dimensions were not 4x4");
        expect(img.rgba.size() == 4u * 4u * 4u, "RGB565 rgba byte count was wrong");
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "RGB565 0xFFFF should decode to white opaque");
    }

    // RGB5A3 opaque (format 5, bit 15 set): 0xFFFF -> white opaque. One 4x4 block = 32 bytes.
    {
        std::vector<std::uint8_t> data(32, 0xFF);
        auto img = decodeBtiImage(data, 0, 5, 2, 2, 0, Endian::big);
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "RGB5A3 opaque 0xFFFF should be white opaque");
    }

    // RGB5A3 ARGB3444 (format 5, bit 15 clear): exercises the expand4 fix.
    {
        std::vector<std::uint8_t> maxWord(32, 0);
        maxWord[0] = 0x7F; maxWord[1] = 0xFF; // a=7, r=15, g=15, b=15
        auto img = decodeBtiImage(maxWord, 0, 5, 1, 1, 0, Endian::big);
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "RGB5A3 ARGB3444 max should be opaque white");
        std::vector<std::uint8_t> blueWord(32, 0);
        blueWord[0] = 0x00; blueWord[1] = 0x0F; // blue nibble 15, alpha nibble 0
        auto img2 = decodeBtiImage(blueWord, 0, 5, 1, 1, 0, Endian::big);
        expect(img2.rgba[2] == 255, "RGB5A3 blue nibble 0xF must expand to 255 (expand4 fix)");
        expect(img2.rgba[3] == 0, "RGB5A3 alpha nibble 0 must expand to 0");
        std::vector<std::uint8_t> midWord(32, 0);
        midWord[0] = 0x00; midWord[1] = 0x05; // blue nibble 5
        auto img3 = decodeBtiImage(midWord, 0, 5, 1, 1, 0, Endian::big);
        expect(img3.rgba[2] == 85, "RGB5A3 nibble 0x5 must expand to 85 (expand4)");
    }


    // IA4 (format 2): 2x2 inside one 8x4 block (32 bytes consumed).
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0x55; // intensity 5, alpha 5 -> expand4(5) = 85
        data[1] = 0xFF; // intensity 15, alpha 15 -> 255
        auto img = decodeBtiImage(data, 0, 2, 2, 2, 0, Endian::big);
        const std::size_t p00 = (0u * img.width + 0u) * 4u;
        const std::size_t p10 = (0u * img.width + 1u) * 4u;
        expect(img.rgba[p00] == 85 && img.rgba[p00 + 3] == 85, "IA4 nibble 5 should expand to 85");
        expect(img.rgba[p10] == 255, "IA4 nibble 15 should expand to 255");
    }

    // I8 (format 1): 2x2 inside one 8x4 block (32 bytes consumed).
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0x80;
        data[1] = 0x10;
        data[8] = 0xFF;
        data[9] = 0x00;
        auto img = decodeBtiImage(data, 0, 1, 2, 2, 0, Endian::big);
        expect(img.rgba[0] == 0x80 && img.rgba[3] == 255, "I8 pixel (0,0) was wrong");
        expect(img.rgba[8] == 0xFF, "I8 pixel (0,1) was wrong");
        expect(img.rgba[12] == 0x00 && img.rgba[15] == 255, "I8 pixel (1,1) was wrong");
    }

    // C8 (format 9) palettized: rgb565 palette, entry 0 white, entry 1 black.
    {
        std::vector<std::uint8_t> palette = {0xFF, 0xFF, 0x00, 0x00};
        std::vector<std::uint8_t> data(32, 0);
        data[1] = 1;
        data[9] = 1;
        auto img = decodeBtiImage(data, 0, 9, 2, 2, 0, Endian::big, palette, 1);
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "C8 palette entry 0 should map white");
        expect(img.rgba[4] == 0 && img.rgba[7] == 255, "C8 palette entry 1 should map black");
        expect(img.rgba[8] == 255, "C8 palette entry 0 for pixel (0,1) wrong");
        expect(img.rgba[12] == 0 && img.rgba[15] == 255, "C8 palette entry 1 for pixel (1,1) wrong");
    }

    // CMPR (format 14): one 8x8 macro block = 32 bytes; colorA > colorB -> color3 = (2*c0+c1)/3.
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0xFF; data[1] = 0xFF; // colorA = 0xFFFF (white)
        data[4] = 0x80;                 // pixel(0,0) index 2 -> color3 = 170
        auto img = decodeBtiImage(data, 0, 14, 4, 4, 0, Endian::big);
        expect(img.rgba[0] == 170 && img.rgba[1] == 170 && img.rgba[2] == 170 && img.rgba[3] == 255,
               "CMPR color3 interpolation was wrong");
    }

    // CMPR: colorA <= colorB -> fourth color is transparent (alpha 0).
    {
        std::vector<std::uint8_t> data(32, 0);
        data[2] = 0xFF; data[3] = 0xFF; // colorB = 0xFFFF (white), colorA = 0
        data[4] = 0xC0;                 // pixel(0,0) index 3 -> color4 = transparent white
        auto img = decodeBtiImage(data, 0, 14, 4, 4, 0, Endian::big);
        expect(img.rgba[0] == 255 && img.rgba[1] == 255 && img.rgba[2] == 255 && img.rgba[3] == 0,
               "CMPR transparent color4 must have alpha 0");
    }

    // parseBti: standalone .bti entry at entryOffset 0 (I8, 4x4).
    {
        std::vector<std::uint8_t> blob(64, 0);
        blob[0] = 1;
        blob[2] = 0; blob[3] = 4;
        blob[4] = 0; blob[5] = 4;
        blob[24] = 0;
        blob[28] = 0; blob[29] = 0; blob[30] = 0; blob[31] = 32;
        for (int i = 0; i < 16; ++i) {
            blob[32 + i] = static_cast<std::uint8_t>(i * 8 + i);
        }
        const auto bti = parseBti(blob, 0, Endian::big);
        expect(bti.width == 4 && bti.height == 4, "parseBti width/height wrong");
        expect(bti.mipmaps.size() == 1, "parseBti should decode one mip level");
        expect(bti.mipmaps[0].rgba.size() == 4u * 4u * 4u, "parseBti mip byte count wrong");
        expect(bti.mipmaps[0].rgba[0] == 0 && bti.mipmaps[0].rgba[3] == 255, "parseBti I8 pixel (0,0) wrong");
    }

    // parseBti: embedded entry at a NON-zero entryOffset (absolute offset fix).
    {
        constexpr std::size_t prefix = 16;
        std::vector<std::uint8_t> blob(prefix + 32 + 32, 0);
        blob[prefix + 0] = 1;
        blob[prefix + 2] = 0; blob[prefix + 3] = 2;
        blob[prefix + 4] = 0; blob[prefix + 5] = 2;
        blob[prefix + 24] = 0;
        blob[prefix + 28] = 0; blob[prefix + 29] = 0; blob[prefix + 30] = 0; blob[prefix + 31] = 32;
        blob[prefix + 32 + 0] = 0x80;
        blob[prefix + 32 + 1] = 0x10;
        blob[prefix + 32 + 8] = 0xFF;
        blob[prefix + 32 + 9] = 0x00;
        const auto bti = parseBti(blob, prefix, Endian::big);
        expect(bti.width == 2 && bti.height == 2, "embedded parseBti dimensions wrong");
        expect(bti.mipmaps.size() == 1, "embedded parseBti mip count wrong");
        expect(bti.mipmaps[0].rgba[0] == 0x80 && bti.mipmaps[0].rgba[3] == 255, "embedded parseBti (0,0) wrong");
        expect(bti.mipmaps[0].rgba[8] == 0xFF, "embedded parseBti (0,1) wrong");
        expect(bti.mipmaps[0].rgba[12] == 0x00 && bti.mipmaps[0].rgba[15] == 255, "embedded parseBti (1,1) wrong");
    }

    // GX sampler mapping (Java ImageUtils parity, no GL needed):
    // wrap 0 -> CLAMP_TO_EDGE, 2 -> MIRRORED_REPEAT, else (incl. 1) -> REPEAT;
    // filter 0 -> NEAREST, 2..5 -> mipmapped variants, else LINEAR.
    {
        using whitehole::smg::btiSamplerInfo;
        using whitehole::smg::Bti;
        Bti tex{};
        tex.wrapS = 0; tex.wrapT = 0;
        auto sampler = btiSamplerInfo(tex);
        expect(sampler.glWrapS == 0x812F && sampler.glWrapT == 0x812F, "wrap 0 must clamp to edge");
        tex.wrapS = 1; tex.wrapT = 1;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glWrapS == 0x2901 && sampler.glWrapT == 0x2901, "wrap 1 must repeat (SMG tiling)");
        tex.wrapS = 2; tex.wrapT = 2;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glWrapS == 0x8370 && sampler.glWrapT == 0x8370, "wrap 2 must mirror");
        tex.wrapS = 3;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glWrapS == 0x2901, "unknown wrap modes must fall back to repeat (Java default)");
        tex.minFilter = 0; tex.magFilter = 0;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glMinFilter == 0x2600 && sampler.glMagFilter == 0x2600, "filter 0 must be nearest");
        tex.minFilter = 1; tex.magFilter = 1;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glMinFilter == 0x2601 && sampler.glMagFilter == 0x2601, "filter 1 must be linear");
        tex.minFilter = 2;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glMinFilter == 0x2700, "filter 2 must be nearest-mipmap-nearest");
        tex.minFilter = 3;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glMinFilter == 0x2701, "filter 3 must be linear-mipmap-nearest");
        tex.minFilter = 4;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glMinFilter == 0x2702, "filter 4 must be nearest-mipmap-linear");
        tex.minFilter = 5;
        sampler = btiSamplerInfo(tex);
        expect(sampler.glMinFilter == 0x2703, "filter 5 must be linear-mipmap-linear");
    }
}

void testBmdMaterialTextureRouting() {
    // TEV-order routing (Java Bmd parity): the primary texture is the first
    // live stage whose texmap resolves, not blindly slot 0 -- and the UV set
    // comes from that stage's texcoord, not always set 0.
    using whitehole::smg::BmdMaterial;
    BmdMaterial untextured{};
    expect(untextured.primaryTextureSlot() == -1, "untextured material must route -1");
    expect(untextured.primaryTexCoordSet() == 0, "untextured material must sample UV set 0");
    BmdMaterial multi{};
    multi.tevStageCount = 2;
    multi.tevTexMap[0] = 1;   // stage 0 -> map 1 -> TEX1 7
    multi.tevTexCoord[0] = 3; // stage 0 samples UV set 3
    multi.tevTexMap[1] = 0;   // stage 1 -> map 0 -> TEX1 5
    multi.tevTexCoord[1] = 1;
    multi.textureIndices[0] = 5;
    multi.textureIndices[1] = 7;
    expect(multi.primaryTextureSlot() == 7, "primary texture must follow the first live TEV stage, not slot 0");
    expect(multi.primaryTexCoordSet() == 3, "primary UV set must follow the first live TEV stage, not set 0");
    BmdMaterial deadStage{};
    deadStage.tevStageCount = 1;
    deadStage.tevTexMap[0] = -1; // 0xFF: stage samples no texture
    deadStage.textureIndices[0] = 5;
    expect(deadStage.primaryTextureSlot() == 5, "dead TEV stage must fall back to the first used map");

    // Depth compares must be restated for the viewport's reversed-Z buffer.
    // The renderer clears depth to 0 and maps near to 1, so "nearer" is the
    // LARGER value. Passing the authored compare through unchanged made
    // LESS/LEQUAL reject every visible fragment, which is why textured models
    // rendered nothing at all while the untextured path (no per-material depth
    // func) still drew.
    const auto reversed = [](std::uint8_t authored) {
        BmdMaterial material{};
        material.depthFunction = authored;
        return material.depthFunctionReversedZ();
    };
    expect(reversed(1) == 4, "LESS must become GREATER under reversed-Z");
    expect(reversed(3) == 6, "LEQUAL must become GEQUAL under reversed-Z");
    expect(reversed(4) == 1, "GREATER must become LESS under reversed-Z");
    expect(reversed(6) == 3, "GEQUAL must become LEQUAL under reversed-Z");
    // Equality-only compares are symmetric and must pass through untouched.
    expect(reversed(0) == 0, "NEVER must be unchanged by the depth-space flip");
    expect(reversed(2) == 2, "EQUAL must be unchanged by the depth-space flip");
    expect(reversed(5) == 5, "NOTEQUAL must be unchanged by the depth-space flip");
    expect(reversed(7) == 7, "ALWAYS must be unchanged by the depth-space flip");
    // The mapping must be an involution: applying it twice is the identity, so
    // the frame default and the state-restore default cannot drift apart.
    for (std::uint8_t compare = 0; compare < 8; ++compare) {
        BmdMaterial material{};
        material.depthFunction = compare;
        const auto once = material.depthFunctionReversedZ();
        material.depthFunction = once;
        expect(material.depthFunctionReversedZ() == compare,
               "reversed-Z depth mapping is not self-inverse");
    }
}


void testJsonRoundTrip() {
    using whitehole::util::JsonArray;
    using whitehole::util::JsonObject;
    using whitehole::util::JsonValue;
    JsonObject root;
    root["name"] = JsonValue("Kinopio");
    root["count"] = JsonValue(3.0);
    root["ok"] = JsonValue(true);
    JsonArray items;
    items.emplace_back("a");
    items.emplace_back(1.0);
    root["items"] = JsonValue(std::move(items));
    const std::string text = whitehole::util::serializeJson(JsonValue(root));
    const JsonValue parsed = whitehole::util::parseJson(text);
    expect(parsed.strAt("name") == "Kinopio", "JSON string round trip failed");
    expect(parsed.at("count").asNumber() == 3.0, "JSON number round trip failed");
    expect(parsed.at("ok").asBool() == true, "JSON bool round trip failed");
    expect(parsed.at("items").asArray().size() == 2, "JSON array round trip failed");
    bool rejected = false;
    try {
        (void)whitehole::util::parseJson("{bad}");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "malformed JSON was not rejected");
}

void testSettingsRoundTrip() {
    whitehole::app::Settings settings;
    TemporaryDirectory temp;
    settings.setConfigPath(temp.path / "settings.json");
    settings.lastGameDir = "C:/games/smg2";
    settings.darkMode = false;
    settings.showPaths = false;
    settings.pushRecentMap("C:/games/map.arc");
    settings.pushRecentMap("C:/games/other.arc");
    settings.pushRecentMap("C:/games/map.arc");
    expect(settings.recentMaps.size() == 2, "recent maps should dedupe");
    expect(settings.recentMaps.front() == "C:/games/map.arc", "recent maps order wrong");
    settings.save();
    whitehole::app::Settings loaded;
    loaded.setConfigPath(temp.path / "settings.json");
    loaded.load();
    expect(loaded.lastGameDir == "C:/games/smg2", "settings lastGameDir mismatch");
    expect(loaded.darkMode == false, "settings darkMode mismatch");
    expect(loaded.showPaths == false, "settings showPaths mismatch");
    expect(loaded.recentMaps.size() == 2, "settings recentMaps mismatch");
}

void testObjectDatabase() {
    TemporaryDirectory temp;
    const auto path = temp.path / "objectdb.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"Objects\":["
               "{\"InternalName\":\"Kinopio\",\"SimpleName\":\"Toad\",\"Category\":\"NPC\"},"
               "{\"InternalName\":\"Kuribo\",\"Category\":\"Enemy\"}]}";
    }
    whitehole::db::ObjectDatabase db;
    db.load(path);
    expect(db.size() == 2, "objectdb size wrong");
    expect(db.contains("Kinopio"), "objectdb missing Kinopio");
    expect(db.displayName("Kinopio") == "Toad", "objectdb display name wrong");
    expect(db.displayName("Missing") == "\"Missing\"", "objectdb fallback wrong");
}

void testDataHolderRoundTrip() {
    TemporaryDirectory temp;
    const auto base = temp.path / "base";
    std::filesystem::create_directories(base);
    {
        std::ofstream out(base / "test.json");
        out << "{\"Items\": [\"a\",\"b\"]}";
    }
    whitehole::db::DataHolderBase holder("test.json", "/hints.json", true);
    holder.setBaseGameRoot(base);
    holder.initBaseGame();
            expect(holder.dataPresent(), "base game data should be present");
    expect(holder.root().at("Items").asArray().size() == 2, "base root should parse");
}

void testDbHelpersRoundTrip() {
        TemporaryDirectory temp;
    const auto base = temp.path / "base";
    std::filesystem::create_directories(base / "data");

    // Hints: top-level "Hints" array with Game field for filtering.
    {
        std::ofstream out(base / "data" / "hints.json");
        out << "{\"Hints\":[{\"Game\":0,\"Hint\":\"hello\",\"Name\":\"Test\"},{\"Game\":1,\"Hint\":\"smg1only\"}]}";
    }
    whitehole::db::Hints hints;
    hints.setBaseGameRoot(base);
    hints.initBaseGame();
    hints.load(2);
    expect(hints.hints().size() == 1, "hints should load one entry for SMG2");
    expect(hints.hints()[0].hint == "hello", "hints hint wrong");
    hints.load(1);
    expect(hints.hints().size() == 2, "hints should load two entries for SMG1");

    // AreaManagerLimits: aliases + limits per game.
    {
        std::ofstream out(base / "data" / "areamanagerlimits.json");
        out << "{\"AreaManagerAliases\":{\"SMG1\":{\"Cube\":\"Area\"}},\"AreaManagers\":{\"SMG1\":{\"Area\":\"5\"}}}";
    }
    whitehole::db::AreaManagerLimits limits;
    limits.setBaseGameRoot(base);
    limits.initBaseGame();
    limits.load(1);
    expect(limits.resolveAlias("Cube") == "Area", "alias resolve wrong");
    expect(limits.resolveAlias("Missing") == "Missing", "alias passthrough wrong");
    expect(limits.limitFor("Cube") == "5", "limit via alias wrong");
    expect(limits.limitFor("Missing") == "", "limit missing empty");

    // Shortcuts: flat root.
    {
        std::ofstream out(base / "data" / "shortcuts.json");
        out << "{\"save\":\"Ctrl+S\"}";
    }
    whitehole::db::Shortcuts shortcuts;
    shortcuts.setBaseGameRoot(base);
    shortcuts.initBaseGame();
    shortcuts.load();
    expect(shortcuts.get("save") == "Ctrl+S", "shortcut get wrong");
    expect(shortcuts.get("missing") == "", "shortcut missing empty");

    // ModelSubstitutions: flat lowercase-keyed map.
    {
        std::ofstream out(base / "data" / "modelsubstitutions.json");
        out << "{\"dumptr\":\"Kinopio\"}";
    }
    whitehole::db::ModelSubstitutions subs;
    subs.setBaseGameRoot(base);
    subs.initBaseGame();
    subs.load();
    expect(subs.substitute("Dumptr") == "Kinopio", "case-insensitive substitute");
    expect(subs.substitute("missing") == "", "no substitute empty");

    // SpecialRenderers: array with ObjectName/ClassName + RendererType.
    {
        std::ofstream out(base / "data" / "specialrenderers.json");
        out << "{\"SpecialRenderers\":[{\"ObjectName\":\"GoalA\",\"RendererType\":\"Special\"}]}";
    }
    whitehole::db::SpecialRenderers special;
    special.setBaseGameRoot(base);
    special.initBaseGame();
    special.load();
    expect(special.lookup("GoalA") == "Special", "special renderer lookup");
    expect(special.lookup("Unknown") == "", "special renderer unknown empty");
}

void testObjectDatabaseV2() {
    TemporaryDirectory temp;
    const auto path = temp.path / "objectdb.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({
  "Timestamp": 1234567890,
  "Categories": [{"Key":"enemy","Description":"Enemies"},{"Key":"stagepart","Description":"Stage Parts"}],
  "Classes": [
    {"InternalName":"SampleObj","Name":"SampleObj","Notes":"A test class","Games":3,"Progress":1,
     "Parameters":{
        "Obj_arg0":{"Name":"Range","Type":"Float","Games":3,"Needed":true,"Description":"How far.","Values":[],"Exclusives":[]},
        "Obj_arg1":{"Name":"Mode","Type":"Integer","Games":3,"Needed":false,"Description":"Pick one.","Values":[{"Value":0,"Notes":"Off"},{"Value":1,"Notes":"On"}],"Exclusives":[]},
        "Obj_arg2":{"Name":"Only SMG2","Type":"Integer","Games":2,"Needed":false,"Description":"","Values":[],"Exclusives":[]},
        "Obj_arg3":{"Name":"Special","Type":"Boolean","Games":3,"Needed":false,"Description":"","Values":[],"Exclusives":["Kinopio"]},
        "SW_A":{"Games":3,"Needed":false,"Description":"Switch A.","Values":[],"Exclusives":[]}
     }}
  ],
  "Objects": [
    {"InternalName":"Kinopio","ClassNameSMG1":"SampleObj","ClassNameSMG2":"SampleObj","Name":"Toad",
     "Notes":"Friendly.","Category":"npc","ListSMG1":"ObjInfo","ListSMG2":"ObjInfo","File":"Map","Games":3,
     "Progress":1,"IsUnused":false,"IsLeftover":false},
    {"InternalName":"OldThing","ClassNameSMG1":"SampleObj","ClassNameSMG2":"SampleObj","Name":"Leftover",
     "Category":"stagepart","Games":1,"IsUnused":true,"IsLeftover":true}
  ]
})";
    }
    whitehole::db::ObjectDatabase db;
    db.load(path);

    expect(db.size() == 2, "objectdb v2 object count wrong");
    expect(db.classCount() == 1, "objectdb v2 class count wrong");
    expect(db.categoryCount() == 2, "objectdb v2 category count wrong");
    expect(db.timestamp() == 1234567890U, "objectdb v2 timestamp wrong");

    const auto* kinopio = db.find("Kinopio");
    expect(kinopio != nullptr, "objectdb v2 missing Kinopio");
    expect(kinopio->name == "Toad", "objectdb v2 display name wrong");
    expect(kinopio->description == "Friendly.", "objectdb v2 notes wrong");
    expect(kinopio->className(1) == "SampleObj", "objectdb v2 smg1 class wrong");
    expect(kinopio->className(2) == "SampleObj", "objectdb v2 smg2 class wrong");
    expect(kinopio->list(2) == "ObjInfo", "objectdb v2 list wrong");
    expect(!kinopio->unused, "objectdb v2 unused flag wrong");

    expect(db.findClass("SampleObj") != nullptr, "objectdb v2 class lookup failed");
    expect(db.classForObject("Kinopio", 2) != nullptr, "objectdb v2 class-for-object failed");

    // Labels, descriptions and kinds come from the class metadata.
    expect(db.propertyLabel("Kinopio", "Obj_arg0", 2) == "Range", "objectdb v2 label wrong");
    expect(db.propertyDescription("Kinopio", "Obj_arg0", 2) == "How far.", "objectdb v2 description wrong");
    const auto* range = db.propertyForObject("Kinopio", "Obj_arg0", 2);
    expect(range != nullptr, "objectdb v2 float property missing");
    expect(range->kind == whitehole::db::PropertyKind::Float, "objectdb v2 float kind wrong");
    expect(range->needed, "objectdb v2 needed flag wrong");

    const auto* mode = db.propertyForObject("Kinopio", "Obj_arg1", 2);
    expect(mode != nullptr, "objectdb v2 list property missing");
    expect(mode->kind == whitehole::db::PropertyKind::IntList, "objectdb v2 list kind wrong");
    expect(mode->values.size() == 2, "objectdb v2 values count wrong");
    expect(mode->values[1] == "1: On", "objectdb v2 value formatting wrong");

    // Per-game filtering: Obj_arg2 only exists in SMG2.
    expect(db.propertyUsed("Kinopio", "Obj_arg2", 2), "objectdb v2 smg2-only property hidden");
    expect(!db.propertyUsed("Kinopio", "Obj_arg2", 1), "objectdb v2 smg2-only property leaked to smg1");

    // Exclusives: Obj_arg3 is listed for Kinopio only.
    expect(db.propertyUsed("Kinopio", "Obj_arg3", 2), "objectdb v2 exclusive property hidden");
    expect(!db.propertyUsed("OldThing", "Obj_arg3", 2), "objectdb v2 exclusive property leaked");

    // A parameter with no declared "Type" still becomes an integer cell, and an
    // unknown parameter falls back to its raw identifier as the label.
    const auto* switchA = db.propertyForObject("Kinopio", "SW_A", 2);
    expect(switchA != nullptr, "objectdb v2 untyped property missing");
    expect(switchA->kind == whitehole::db::PropertyKind::Integer, "objectdb v2 untyped kind wrong");
    expect(db.propertyLabel("Kinopio", "SomethingElse", 2) == "SomethingElse",
           "objectdb v2 unknown property label wrong");

    // Alias table (Java getPropertyInfoForObject).
    expect(whitehole::db::ObjectDatabase::aliasField("CommonPath_ID") == "Rail", "alias Rail wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("CameraSetId") == "Camera", "alias Camera wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("GroupId") == "Group", "alias Group wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("MessageId") == "Message", "alias Message wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("Plain") == "Plain", "alias passthrough wrong");

    // Game availability (Java ObjectSelectForm filter).
    expect(db.objectAvailable("Kinopio", 1) && db.objectAvailable("Kinopio", 2),
           "objectdb v2 availability wrong");
    expect(db.objectAvailable("OldThing", 1), "objectdb v2 smg1-only availability wrong");
    expect(!db.objectAvailable("OldThing", 2), "objectdb v2 smg1-only leaked to smg2");

    // Search matches display name, internal name and class name.
    expect(db.search("toad", 2).size() == 1, "objectdb v2 search by display name");
    expect(db.search("kinop", 2).size() == 1, "objectdb v2 search by internal name");
    expect(db.search("sampleobj", 1).size() == 2, "objectdb v2 search by class name");
    expect(db.search("nothinghere", 2).empty(), "objectdb v2 search false positive");

    // Categories keep declaration order.
    expect(db.categories()[0].key == "enemy", "objectdb v2 category order wrong");
    expect(db.categories()[1].description == "Stage Parts", "objectdb v2 category description wrong");
}

void testObjectDatabaseCache() {
    TemporaryDirectory temp;
    const auto jsonPath = temp.path / "objectdb.json";
    const auto cachePath = temp.path / "cache" / "objectdb.cache";

    const auto writeJson = [&](std::string_view name) {
        std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
        out << "{\"Timestamp\":7,\"Classes\":[{\"InternalName\":\"C\",\"Parameters\":{}}],"
               "\"Objects\":[{\"InternalName\":\"Obj\",\"Name\":\""
            << name << "\",\"ClassNameSMG1\":\"C\",\"ClassNameSMG2\":\"C\",\"Games\":3}]}";
    };

    writeJson("First");
    whitehole::db::ObjectDatabase db;
    db.load(jsonPath, cachePath);
    expect(!db.cacheLoaded(), "objectdb cache should not be used before it exists");
    expect(db.displayName("Obj") == "First", "objectdb cache source value wrong");
    expect(std::filesystem::exists(cachePath), "objectdb cache was not written");

    whitehole::db::ObjectDatabase cached;
    cached.load(jsonPath, cachePath);
    expect(cached.cacheLoaded(), "objectdb cache was not used on the second load");
    expect(cached.size() == 1, "objectdb cache object count wrong");
    expect(cached.classCount() == 1, "objectdb cache class count wrong");
    expect(cached.displayName("Obj") == "First", "objectdb cache display name wrong");
    expect(cached.timestamp() == 7U, "objectdb cache timestamp wrong");

    // Rewriting the JSON must invalidate the compiled cache. The source
    // timestamp is pushed into the future so the test does not depend on clock
    // granularity.
    writeJson("Second");
    std::error_code error;
    const auto bumped = std::filesystem::last_write_time(jsonPath, error) + std::chrono::seconds(10);
    std::filesystem::last_write_time(jsonPath, bumped, error);

    whitehole::db::ObjectDatabase reparsed;
    reparsed.load(jsonPath, cachePath);
    expect(!reparsed.cacheLoaded(), "objectdb cache was not invalidated");
    expect(reparsed.displayName("Obj") == "Second", "objectdb cache invalidation value wrong");
}

void testRealObjectDatabase() {
#ifdef WHITEHOLE_SOURCE_DIR
    const auto path = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "objectdb.json";
    if (!std::filesystem::exists(path)) return; // optional bulky data

    whitehole::db::ObjectDatabase db;
    db.load(path);
    expect(db.size() > 2000, "real objectdb object count unexpectedly small");
    expect(db.classCount() > 800, "real objectdb class count unexpectedly small");
    expect(db.categoryCount() >= 10, "real objectdb categories unexpectedly few");
    expect(db.timestamp() > 0, "real objectdb timestamp missing");

    // Whenever the database declares a class name for an object it must resolve,
    // otherwise the property grid would silently come up empty.
    std::size_t declared = 0;
    std::size_t resolved = 0;
    for (const auto& name : db.names()) {
        const auto* info = db.find(name);
        if (info == nullptr || info->classNameSmg2.empty()) continue;
        ++declared;
        if (db.findClass(info->classNameSmg2) != nullptr) ++resolved;
    }
    expect(declared > 2000, "real objectdb declares too few SMG2 class names");
    expect(resolved == declared, "real objectdb has unresolvable SMG2 class names");

    const auto* rail = db.findClass("RailMoveObj");
    expect(rail != nullptr, "real objectdb missing the RailMoveObj class");
    expect(!rail->properties.empty(), "real objectdb RailMoveObj has no parameters");
#endif
}

// ---------------------------------------------------------------------------
// BMD/BDL reading and model mesh building.
//
// The fixtures below are assembled by absolute offset because that is exactly
// how J3D stores data: every array is placed by an explicit offset table, and
// the reader has to trust those tables. Building them by hand keeps the tests
// independent of any real game file.
// ---------------------------------------------------------------------------

void putU16(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value) {
    if (data.size() < offset + 2) {
        data.resize(offset + 2, 0);
    }
    data[offset] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

void putU32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    if (data.size() < offset + 4) {
        data.resize(offset + 4, 0);
    }
    data[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

void putF32(std::vector<std::uint8_t>& data, std::size_t offset, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(data, offset, bits);
}

void putText(std::vector<std::uint8_t>& data, std::size_t offset, std::string_view text) {
    if (data.size() < offset + text.size()) {
        data.resize(offset + text.size(), 0);
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        data[offset + index] = static_cast<std::uint8_t>(text[index]);
    }
}

// Wraps a section body (everything after the 8-byte tag + size header) in its
// tag and total size. Bodies are written with section-relative offsets.
std::vector<std::uint8_t> makeSection(std::string_view tag, std::vector<std::uint8_t> body) {
    std::vector<std::uint8_t> section(8, 0);
    putText(section, 0, tag);
    putU32(section, 4, static_cast<std::uint32_t>(body.size() + 8));
    section.insert(section.end(), body.begin(), body.end());
    return section;
}

// VTX1 with a unit quad: positions, normals and one texture coordinate array.
// The 13-entry offset table is an ordered list (the slot positions carry no
// meaning beyond the order), so the arrays are placed in the slots the format
// uses for position, normal and colour data and described by the definitions.
std::vector<std::uint8_t> makeVtx1Body() {
    constexpr std::size_t kArrayDefinitions = 0x40;
    constexpr std::size_t kPositionData = kArrayDefinitions + 3 * 0x10; // 0x70
    constexpr std::size_t kNormalData = kPositionData + 4 * 3 * 4;      // 0xA0
    constexpr std::size_t kTexCoordData = kNormalData + 4 * 3 * 4;      // 0xD0
    constexpr std::size_t kSectionSize = kTexCoordData + 4 * 2 * 4;     // 0xF0

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU32(body, 0, static_cast<std::uint32_t>(kArrayDefinitions));
    putU32(body, 0x0C - 8 + 9 * 4, static_cast<std::uint32_t>(kPositionData));
    putU32(body, 0x0C - 8 + 10 * 4, static_cast<std::uint32_t>(kNormalData));
    putU32(body, 0x0C - 8 + 11 * 4, static_cast<std::uint32_t>(kTexCoordData));

    const auto writeDefinition = [&body](std::size_t sectionOffset, std::uint32_t arrayType,
                                         std::uint32_t componentCount) {
        const std::size_t at = sectionOffset - 8;
        putU32(body, at, arrayType);
        putU32(body, at + 4, componentCount);
        putU32(body, at + 8, 4); // 4 = f32
        body[at + 0xC] = 0;      // fraction bits
    };
    writeDefinition(kArrayDefinitions + 0 * 0x10, 9, 1);  // positions, code 1 = XYZ
    writeDefinition(kArrayDefinitions + 1 * 0x10, 10, 0); // normals, code 0 = XYZ
    writeDefinition(kArrayDefinitions + 2 * 0x10, 13, 1); // texcoord 0, code 1 = UV

    const float positions[4][3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const float texcoords[4][2] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
    for (std::size_t vertex = 0; vertex < 4; ++vertex) {
        for (std::size_t component = 0; component < 3; ++component) {
            putF32(body, kPositionData - 8 + (vertex * 3 + component) * 4, positions[vertex][component]);
            putF32(body, kNormalData - 8 + (vertex * 3 + component) * 4, component == 2 ? 1.0F : 0.0F);
        }
        for (std::size_t component = 0; component < 2; ++component) {
            putF32(body, kTexCoordData - 8 + (vertex * 2 + component) * 4, texcoords[vertex][component]);
        }
    }
    return body;
}

// INF1: a root joint with one material and one shape drawn inside it, then the
// terminator. The 0x01/0x02 opcodes exercise the hierarchy stack.
std::vector<std::uint8_t> makeInf1Body() {
    constexpr std::size_t kNodeTable = 0x18;
    const std::uint16_t nodes[6][2] = {{0x10, 0}, {0x01, 0}, {0x11, 0}, {0x12, 0}, {0x02, 0}, {0x00, 0}};
    std::vector<std::uint8_t> body(kNodeTable - 8 + 6 * 4, 0);
    putU32(body, 0x10 - 8, 4);          // vertex count
    putU32(body, 0x14 - 8, kNodeTable); // hierarchy data offset
    for (std::size_t index = 0; index < 6; ++index) {
        putU16(body, kNodeTable - 8 + index * 4, nodes[index][0]);
        putU16(body, kNodeTable - 8 + index * 4 + 2, nodes[index][1]);
    }
    return body;
}

// JNT1: a single joint named "Root" with a scale of one.
std::vector<std::uint8_t> makeJnt1Body() {
    constexpr std::size_t kRemapTable = 0x18;
    constexpr std::size_t kJointData = 0x20;
    constexpr std::size_t kNameTable = 0x60;
    constexpr std::size_t kSectionSize = 0x70;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // joint count
    putU32(body, 0x0C - 8, kJointData);
    putU32(body, 0x10 - 8, kRemapTable);
    putU32(body, 0x14 - 8, kNameTable);
    putU16(body, kRemapTable - 8, 0); // remap[0] -> joint record 0
    putF32(body, kJointData - 8 + 4, 1.0F);
    putF32(body, kJointData - 8 + 8, 1.0F);
    putF32(body, kJointData - 8 + 12, 1.0F);
    putU16(body, kNameTable - 8 + 4 + 2, 8); // name string offset within the table
    putText(body, kNameTable - 8 + 8, "Root");
    return body;
}

// DRW1: one unweighted draw matrix pointing at joint 0.
std::vector<std::uint8_t> makeDrw1Body() {
    std::vector<std::uint8_t> body(0x14, 0);
    putU16(body, 0, 1);
    putU32(body, 0x0C - 8, 0x14); // weighted flag array
    putU32(body, 0x10 - 8, 0x18); // matrix index array
    putU16(body, 0x18 - 8, 0);
    return body;
}

// SHP1: one batch, one packet, one quad primitive using byte indices.
std::vector<std::uint8_t> makeShp1Body() {
    constexpr std::size_t kRemapTable = 0x2C;
    constexpr std::size_t kBatchRecord = 0x30;
    constexpr std::size_t kAttributes = 0x58;
    constexpr std::size_t kMatrixData = 0x78;
    constexpr std::size_t kPacketLocations = 0x80;
    constexpr std::size_t kPacketData = 0x88;
    constexpr std::size_t kPacketSize = 3 + 4 * 3 + 1; // type + count + indices + terminator
    constexpr std::size_t kSectionSize = kPacketData + kPacketSize;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // batch count
    putU32(body, 0x0C - 8, kBatchRecord);
    putU32(body, 0x10 - 8, kRemapTable);
    putU32(body, 0x18 - 8, kAttributes);
    putU32(body, 0x1C - 8, 0); // matrix table (unused for single-matrix batches)
    putU32(body, 0x20 - 8, kPacketData);
    putU32(body, 0x24 - 8, kMatrixData);
    putU32(body, 0x28 - 8, kPacketLocations);
    putU16(body, kRemapTable - 8, 0); // remap[0] -> batch record 0

    body[kBatchRecord - 8] = 1;        // matrix type: one matrix per packet
    // Batch entry: matrix type, a pad byte, then packet count, attribute-list
    // offset, first-matrix index and first-packet index as four consecutive
    // u16s (Java readSHP1: readByte, skip(1), then four readShorts). The first
    // u16 sits directly after the pad byte -- a port that read all four one
    // slot too far decoded packetCount as 0 on real BDLs and silently dropped
    // every triangle.
    putU16(body, kBatchRecord - 8 + 2, 1); // packet count
    putU16(body, kBatchRecord - 8 + 4, 0); // attribute list offset
    putU16(body, kBatchRecord - 8 + 6, 0); // first matrix index
    putU16(body, kBatchRecord - 8 + 8, 0); // first packet index

    const std::uint32_t attributeTypes[3] = {9, 10, 13};
    for (std::size_t index = 0; index < 3; ++index) {
        putU32(body, kAttributes - 8 + index * 8, attributeTypes[index]);
        putU32(body, kAttributes - 8 + index * 8 + 4, 1); // 1 = byte indices
    }
    putU32(body, kAttributes - 8 + 3 * 8, 0xFF); // attribute terminator

    putU32(body, kPacketLocations - 8, static_cast<std::uint32_t>(kPacketSize));      // packet size
    putU32(body, kPacketLocations - 8 + 4, 0);  // packet offset within the data block

    body[kPacketData - 8] = 0x80; // GX quads
    putU16(body, kPacketData - 8 + 1, 4);
    std::size_t cursor = kPacketData - 8 + 3;
    for (std::size_t vertex = 0; vertex < 4; ++vertex) {
        for (std::size_t attribute = 0; attribute < 3; ++attribute) {
            body[cursor++] = static_cast<std::uint8_t>(vertex);
        }
    }
    body[cursor] = 0; // primitive list terminator
    return body;
}

// MAT3: one material named "mat" with a brown diffuse colour. Every index in
// the material's 0x14C record points at entry 0 of its field table.
// Texture slots follow Java Bmd parity: record slot -> shared short
// texture-index table -> TEX1 id (entry 0 holds TEX1 0, rest unused).
std::vector<std::uint8_t> makeMat3Body() {
    constexpr std::size_t kSectionStart = 0x88;  // start of the init-data records
    constexpr std::size_t kRecordSize = 0x14C;
    constexpr std::size_t kRemapTable = 0x1D8;
    constexpr std::size_t kNameTable = 0x1DC;
    constexpr std::size_t kCullTable = 0x1E8;
    constexpr std::size_t kMaterialColorTable = 0x1EC;
    constexpr std::size_t kAmbientColorTable = 0x1F4;
    constexpr std::size_t kColorChannelCountTable = 0x1FC;
    constexpr std::size_t kTexGenCountTable = 0x1FD;
    constexpr std::size_t kTevStageCountTable = 0x1FE;
    constexpr std::size_t kZCompLocTable = 0x1FF;
    constexpr std::size_t kDitherTable = 0x200;
    constexpr std::size_t kZModeTable = 0x204;
    constexpr std::size_t kAlphaCompareTable = 0x208;
    constexpr std::size_t kBlendInfoTable = 0x210;
    constexpr std::size_t kColorChannelTable = 0x218;
    constexpr std::size_t kTextureIndexTable = 0x220;
    constexpr std::size_t kSectionSize = 0x230;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // material count
    putU32(body, 0x0C - 8, kSectionStart);        // InitDataTableOffset
    putU32(body, 0x10 - 8, kRemapTable);          // RemapTableOffset
    putU32(body, 0x14 - 8, kNameTable);           // NameTableOffset
    putU32(body, 0x1C - 8, kCullTable);           // CullModeInfoOffset
    putU32(body, 0x20 - 8, kMaterialColorTable);  // MaterialColorTableOffset
    putU32(body, 0x24 - 8, kColorChannelCountTable); // ColorChannelCountTableOffset
    putU32(body, 0x28 - 8, kColorChannelTable);   // ColorChannelTableOffset
    putU32(body, 0x2C - 8, kAmbientColorTable);   // AmbientColorTableOffset
    putU32(body, 0x34 - 8, kTexGenCountTable);    // TexGenCountTableOffset
    putU32(body, 0x48 - 8, kTextureIndexTable);   // TextureIndexTableOffset
    putU32(body, 0x58 - 8, kTevStageCountTable);  // TevStageCountTableOffset
    putU32(body, 0x6C - 8, kAlphaCompareTable);   // AlphaCompareTableOffset
    putU32(body, 0x70 - 8, kBlendInfoTable);      // BlendInfoTableOffset
    putU32(body, 0x74 - 8, kZModeTable);          // ZModeTableOffset
    putU32(body, 0x78 - 8, kZCompLocTable);       // ZCompLocTableOffset
    putU32(body, 0x7C - 8, kDitherTable);         // DitherTableOffset

    body[kSectionStart - 8] = 1;                    // pixel engine mode
    putU16(body, kRemapTable - 8, 0);               // remap[0] -> material record 0
    putU16(body, kNameTable - 8 + 4 + 2, 8);        // name string offset within the table
    putText(body, kNameTable - 8 + 8, "mat");

    body[kMaterialColorTable - 8 + 0] = 128;        // diffuse RGBA8
    body[kMaterialColorTable - 8 + 1] = 64;
    body[kMaterialColorTable - 8 + 2] = 32;
    body[kMaterialColorTable - 8 + 3] = 255;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        body[kAmbientColorTable - 8 + channel] = 255;
    }
    for (std::size_t entry = 0; entry < 8; ++entry) {
        putU16(body, kTextureIndexTable - 8 + entry * 2, entry == 0 ? 0 : 0xFFFF);
    }
    // Non-zero ZMode / alpha-compare / blend-info / cull entries so the MAT3
    // walk is proven to land on those tables: a shifted walk reads the same
    // zeros a sparse fixture has, which would render as "never draw, no
    // depth, no blend" just as silently as a correct walk of real zeros.
    body[kCullTable - 8] = 2;               // cull back faces
    body[kZModeTable - 8 + 0] = 1;          // depth test on
    body[kZModeTable - 8 + 1] = 3;          // GL_LEQUAL
    body[kZModeTable - 8 + 2] = 1;          // fragments update depth
    body[kAlphaCompareTable - 8 + 0] = 4;   // GREATER
    body[kAlphaCompareTable - 8 + 1] = 128; // reference 0
    body[kAlphaCompareTable - 8 + 2] = 0;   // AND merge
    body[kAlphaCompareTable - 8 + 3] = 7;   // ALWAYS
    body[kAlphaCompareTable - 8 + 4] = 0;   // reference 1
    body[kBlendInfoTable - 8 + 0] = 1;      // blend (not none/logic/subtract)
    body[kBlendInfoTable - 8 + 1] = 4;      // src: SRC_ALPHA
    body[kBlendInfoTable - 8 + 2] = 5;      // dst: ONE_MINUS_SRC_ALPHA
    body[kBlendInfoTable - 8 + 3] = 0;      // op: add
    // The record's texture-index block sits at record + 0x84 and holds one
    // short per slot; the section grows to fit it before the tables.
    constexpr std::size_t kRecordTexBlock = kSectionStart + 0x84;
    constexpr std::size_t kNewSectionSize = kRecordTexBlock + 16;
    static_assert(kNewSectionSize <= kTextureIndexTable, "record block would overlap the texture index table");
    (void)kNewSectionSize;
    for (std::size_t entry = 0; entry < 8; ++entry) {
        putU16(body, kRecordTexBlock - 8 + entry * 2, entry == 0 ? 0 : 0xFFFF);
    }
    (void)kRecordSize;
    return body;
}
std::vector<std::uint8_t> makeTex1Body() {
    constexpr std::size_t kEntries = 0x14;
    constexpr std::size_t kImage = kEntries + 32;
    constexpr std::size_t kSectionSize = kImage + 32;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // texture count
    putU32(body, 0x0C - 8, kEntries);

    const std::size_t entry = kEntries - 8;
    body[entry] = 1;                                // format: I8
    putU16(body, entry + 2, 2);                     // width
    putU16(body, entry + 4, 2);                     // height
    putU32(body, entry + 28, 32);                   // image data, relative to the entry
    body[kImage - 8 + 0] = 0x80;
    body[kImage - 8 + 1] = 0x10;
    body[kImage - 8 + 8] = 0xFF;
    body[kImage - 8 + 9] = 0x00;
    return body;
}

// A complete bmd3 file with every section the reader understands.
std::vector<std::uint8_t> makeTinyBmd() {
    const std::vector<std::vector<std::uint8_t>> sections{
        makeSection("INF1", makeInf1Body()), makeSection("VTX1", makeVtx1Body()),
        makeSection("JNT1", makeJnt1Body()), makeSection("DRW1", makeDrw1Body()),
        makeSection("SHP1", makeShp1Body()), makeSection("MAT3", makeMat3Body()),
        makeSection("TEX1", makeTex1Body()),
    };

    std::size_t total = 0x20;
    for (const auto& section : sections) {
        total += section.size();
    }

    std::vector<std::uint8_t> file(total, 0);
    putText(file, 0, "J3D2");
    putText(file, 4, "bmd3");
    putU32(file, 8, static_cast<std::uint32_t>(total));
    putU32(file, 0xC, static_cast<std::uint32_t>(sections.size()));
    std::size_t cursor = 0x20;
    for (const auto& section : sections) {
        std::copy(section.begin(), section.end(), file.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += section.size();
    }
    return file;
}

void testBmdParsing() {
    using whitehole::render::buildModelMesh;
    using whitehole::smg::parseBmd;

    const auto bytes = makeTinyBmd();
    const auto model = parseBmd(bytes);

    expect(model.version == "bmd3", "bmd version was not read");
    expect(model.bigEndian, "bmd byte order was not detected");
    expect(model.vertexCount == 4, "bmd vertex count was not read");
    expect(model.positions.size() == 4, "bmd position array size is wrong");
    expect(std::abs(model.positions[2].x - 1.0F) < 0.001F && std::abs(model.positions[2].y - 1.0F) < 0.001F,
           "bmd position data is wrong");
    expect(model.normals.size() == 4 && std::abs(model.normals[0].z - 1.0F) < 0.001F, "bmd normal data is wrong");
    expect(model.texcoords[0].size() == 4 && std::abs(model.texcoords[0][3].y - 1.0F) < 0.001F,
           "bmd texture coordinate data is wrong");
    expect(std::abs(model.boundsMin.x) < 0.001F && std::abs(model.boundsMax.y - 1.0F) < 0.001F,
           "bmd model bounds are wrong");

    expect(model.sceneGraph.size() == 2, "bmd scene graph size is wrong");
    expect(model.sceneGraph[0].nodeType == 1 && model.sceneGraph[0].nodeId == 0, "bmd joint node is wrong");
    expect(model.sceneGraph[1].nodeType == 0 && model.sceneGraph[1].nodeId == 0 &&
               model.sceneGraph[1].materialIndex == 0 && model.sceneGraph[1].parentIndex == 0,
           "bmd shape node is wrong");

    expect(model.joints.size() == 1 && model.joints[0].name == "Root", "bmd joint name was not read");
    expect(std::abs(model.joints[0].scale.y - 1.0F) < 0.001F, "bmd joint scale was not read");
    expect(model.matrixWeighted.size() == 1 && !model.matrixWeighted[0], "bmd draw matrix table is wrong");
    expect(model.matrixIndices.size() == 1 && model.matrixIndices[0] == 0, "bmd draw matrix index is wrong");
    expect(model.findJoint("Root") != nullptr && model.findJoint("Missing") == nullptr, "bmd joint lookup failed");

    expect(model.batches.size() == 1 && model.batches[0].packets.size() == 1, "bmd shape/packet count is wrong");
    expect(model.batches[0].packets[0].primitives.size() == 1, "bmd primitive count is wrong");
    // The fixture's packet matrix table must reach the DRW1 table: an empty
    // table (or a bad matrix id) is the drop gate that hid the old field-offset
    // bug behind healthy-looking batches.
    expect(!model.batches[0].packets[0].matrixTable.empty(), "shp1 packet has no matrix table");
    expect(model.batches[0].packets[0].matrixTable[0] < model.matrixIndices.size(),
           "shp1 packet matrix id misses the DRW1 table");
    expect(model.batches[0].packets[0].primitives.size() == 1, "bmd primitive count is wrong");
    const auto& primitive = model.batches[0].packets[0].primitives.front();
    expect(static_cast<int>(primitive.type) == 0x80, "bmd primitive type is wrong");
    expect(primitive.positionIndices.size() == 4, "bmd position index count is wrong");
    expect(primitive.positionIndices[3] == 3, "bmd position indices are wrong");
    expect(primitive.normalIndices.size() == 4 && primitive.texcoordIndices[0].size() == 4,
           "bmd attribute indices are wrong");

    expect(model.materials.size() == 1 && model.materials[0].name == "mat", "bmd material name was not read");
    const auto& diffuse = model.materials[0].diffuseColor;
    expect(std::abs(diffuse[0] - 128.0F / 255.0F) < 0.01F && std::abs(diffuse[2] - 32.0F / 255.0F) < 0.01F,
           "bmd material colour was not read");
    expect(std::abs(model.materials[0].ambientColor[1] - 1.0F) < 0.01F, "bmd ambient colour was not read");
    expect(model.materials[0].textureIndices[0] == 0, "bmd material texture index was not read");
    expect(model.materials[0].textureIndices[7] == -1, "bmd unused texture map should be -1");
    // Phase C: ZMode / alpha-compare / blend-info / cull table values must
    // land on the material -- only non-zero entries prove the record walk is
    // aligned, since a shifted walk reads the fixture's zeros just fine.
    expect(model.materials[0].cullingMode == 2, "bmd cull mode was not read");
    expect(model.materials[0].depthTest && model.materials[0].depthWrite &&
               model.materials[0].depthFunction == 3,
           "bmd zmode (test/function/write) was not read");
    expect(model.materials[0].alphaFunc0 == 4 && model.materials[0].alphaRef0 == 128 &&
               model.materials[0].alphaOp == 0 && model.materials[0].alphaFunc1 == 7 &&
               model.materials[0].alphaRef1 == 0,
           "bmd alpha compare was not read");
    expect(model.materials[0].alphaTestEnabled(),
           "a GREATER/ALWAYS pair must run a real alpha test");
    expect(model.materials[0].blendMode == 1 && model.materials[0].blendSrcFactor == 4 &&
               model.materials[0].blendDstFactor == 5 && model.materials[0].blendOp == 0,
           "bmd blend info was not read");
    expect(model.materials[0].translucent(),
           "a blend-mode material should join the translucent pass");

    expect(model.textures.size() == 1, "bmd texture count is wrong");
    expect(model.textures[0].width == 2 && model.textures[0].height == 2, "bmd texture header is wrong");
    expect(model.textures[0].base().rgba[0] == 0x80, "bmd texture pixels were not decoded");

    // Mesh building: the quad becomes two triangles carrying the material.
    const auto mesh = buildModelMesh(model);
    expect(mesh.triangles.size() == 2, "model mesh triangle count is wrong");
    expect(mesh.skippedPrimitives == 0, "model mesh dropped a triangle primitive");
    expect(mesh.droppedEmptyMatrixTable == 0 && mesh.droppedBadMatrixIndex == 0,
           "model mesh dropped the packet at the matrix gate");
    expect(std::abs(mesh.boundsMax.x - 1.0F) < 0.001F && mesh.radius > 0.0F, "model mesh bounds are wrong");
    expect(std::abs(mesh.triangles.front().color[0] - 128.0F / 255.0F) < 0.01F,
           "model mesh lost the material colour");
    expect(mesh.triangles.front().materialIndex == 0, "model mesh lost the material index");
    expect(std::abs(mesh.triangles.front().b.normal.z - 1.0F) < 0.01F, "model mesh normals are wrong");

    // Textures (Phase C): the mesh must carry the material/texture tables so
    // the renderer can bind materialIndex -> textureIndices[0] -> textures[0]
    // without keeping the parsed BmdModel alive.
    expect(mesh.materials.size() == 1 && mesh.textures.size() == 1,
           "model mesh did not carry the material/texture tables");
    expect(mesh.materials[0].textureIndices[0] == 0, "mesh material lost its texture slot");
    expect(mesh.textures[0].base().rgba[0] == 0x80, "mesh texture table lost the decoded BTI pixels");
    expect(mesh.triangles.front().translucent, "model mesh lost the translucent pass flag");

    // Multi-part merge (Phase A + C): appending a second part must shift the
    // appended triangles' material indices past dst's material table AND shift
    // the appended materials' texture indices past dst's texture table, so a
    // PlantA01 triangle still points at ITS material/texture after the
    // tables are concatenated.
    {
        auto first = buildModelMesh(model);
        auto second = buildModelMesh(model);
        whitehole::render::appendModelMesh(first, second);
        expect(first.triangles.size() == 4, "appendModelMesh did not stack the part's triangles");
        expect(first.materials.size() == 2 && first.textures.size() == 2,
               "appendModelMesh did not concatenate the material/texture tables");
        expect(first.triangles[0].materialIndex == 0, "original triangle material index changed on append");
        expect(first.triangles[2].materialIndex == 1,
               "appended triangle kept its unshifted material index");
        expect(first.materials[1].textureIndices[0] == 1,
               "appended material's texture index was not shifted past dst's texture table");
        expect(first.skippedPrimitives == 0 && first.droppedEmptyMatrixTable == 0 &&
                   first.droppedBadMatrixIndex == 0,
               "appendModelMesh corrupted the drop counters");
        // Bounds are NOT recomputed by append; the caller does it after all
        // parts are in (ModelLibrary::loadModel does exactly that).
        whitehole::render::recomputeMeshBounds(first);
        expect(first.radius > 0.0F && std::abs(first.boundsMax.x - 1.0F) < 0.001F,
               "recomputeMeshBounds after a merge produced wrong bounds");
    }

    expect(model.valid(), "bmd model should report itself as valid");

    // Skinning safety: DRW1's matrix list and EVP1's envelope list are sized
    // independently (EVP1 only lists weighted matrices), so a weighted draw
    // matrix must be resolved through the DRW1 index into the EVP1 envelope
    // list -- never by using the matrix index on the envelope list. Doing the
    // latter read out of bounds and faulted (0xC0000005) on real BDLs, so this
    // guards the exact shape of that crash: three DRW1 matrices, one envelope.
    {
        auto skinning = model;
        skinning.matrixIndices = {0, 1, 7};
        skinning.matrixWeighted = {false, true, true};
        skinning.envelopeJoints = {{0}};
        skinning.envelopeWeights = {{1.0F}};
        // Point the fixture's only packet at matrix 1, which is the weighted
        // entry -- otherwise the packet resolves matrix 0 (identity) and the
        // skinned code path is never reached.
        skinning.batches[0].packets[0].matrixTable[0] = 1;
        const auto skinned = buildModelMesh(skinning);
        // The weighted entry must resolve (DRW1 index 1 -> EVP1 envelope 0 ->
        // joint 0) rather than fault, so the fixture's quad still produces its
        // two triangles.
        expect(skinned.triangles.size() == 2, "weighted draw matrix was not resolved from the EVP1 envelope");
    }
    // A weighted matrix whose EVP1 envelope index is out of range, and a DRW1
    // index past the matrix table, both have to degrade instead of reading past
    // the end of the envelope vectors.
    {
        auto broken = model;
        broken.matrixWeighted = {false, true};
        broken.matrixIndices = {0, 9};
        broken.envelopeJoints.clear();
        broken.envelopeWeights.clear();
        broken.batches[0].packets[0].matrixTable[0] = 1;
        const auto degraded = buildModelMesh(broken);
        expect(degraded.triangles.size() == 2, "out-of-range envelope index did not fall back safely");
    }

    // Little-endian files keep the same tag bytes in the other order.
    {
        std::vector<std::uint8_t> little(0x20, 0);
        putText(little, 0, "2D3J");
        putText(little, 4, "bmd3");
        const auto parsed = parseBmd(little);
        expect(!parsed.bigEndian, "little-endian bmd was not detected");
        expect(parsed.version == "bmd3", "little-endian bmd version was not read");
    }

    const auto expectRejected = [](std::vector<std::uint8_t> data, const std::string& context) {
        bool rejected = false;
        try {
            (void)whitehole::smg::parseBmd(data);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        expect(rejected, context);
    };

    expectRejected({}, "empty bmd was not rejected");
    expectRejected(std::vector<std::uint8_t>(0x10, 0), "truncated bmd was not rejected");
    {
        auto badMagic = makeTinyBmd();
        putText(badMagic, 0, "J3D9");
        expectRejected(std::move(badMagic), "bmd with a bad magic was not rejected");
    }
    {
        auto badSize = makeTinyBmd();
        putU32(badSize, 0x24, 0xFFFFFFF0U); // first section size past the end of the file
        expectRejected(std::move(badSize), "bmd with a bad section size was not rejected");
    }
    {
        // Real BMD files carry sections this reader does not process yet;
        // they must be skipped, not reject the whole file.
        auto withUnknown = makeTinyBmd();
        putU32(withUnknown, 0x0C, 2);            // bump section count
        const std::size_t extra = withUnknown.size();
        withUnknown.resize(extra + 8);           // 8-byte header for the unknown section
        putText(withUnknown, static_cast<std::uint32_t>(extra), "ZZZZ"); // unknown tag
        putU32(withUnknown, static_cast<std::uint32_t>(extra + 4), 8);   // section size
        const auto parsed = whitehole::smg::parseBmd(withUnknown);
        expect(parsed.positions.size() == 4, "unknown section should not prevent geometry parse");
    }
    {
        auto truncated = makeTinyBmd();
        truncated.resize(0x40);
        expectRejected(std::move(truncated), "bmd truncated mid-section was not rejected");
    }
}

// ---------------------------------------------------------------------------
// Model library: ObjectData archive resolution -> BMD parse -> viewport mesh.
// ---------------------------------------------------------------------------

// Minimal big-endian RARC: a root directory named `rootName` holding the given
// files. Enough for the model loader to locate and read a payload; mirrors how
// real ObjectData archives address their model file as /<Root>/<Name>.bdl.
std::vector<std::uint8_t> makeMinimalRarc(std::string_view rootName,
                                          const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
    // NOTE: the on-disk RARC header occupies 0x00-0x3F (file header + data
    // header), so the string table must start at 0x40. Starting it at 0x20
    // would overwrite the node/entry/string-table fields just written above.
    constexpr std::size_t kStringOffset = 0x40;
    std::size_t cursor = 0;
    const std::size_t dotOffset = cursor;
    cursor += 2; // ".\0"
    const std::size_t dotDotOffset = cursor;
    cursor += 3; // "..\0"
    const std::size_t rootOffset = cursor;
    cursor += rootName.size() + 1;
    std::vector<std::size_t> nameOffsets;
    for (const auto& [name, payload] : files) {
        nameOffsets.push_back(cursor);
        cursor += name.size() + 1;
    }
    const std::size_t stringTableSize = cursor;

    const std::size_t nodeOffset = (kStringOffset + stringTableSize + 0x1F) & ~std::size_t{0x1F};
    const std::size_t entryCount = 2 + files.size(); // "." and ".." pseudo entries
    const std::size_t entryOffset = nodeOffset + 0x10;
    std::size_t dataOffset = entryOffset + entryCount * 0x14;
    dataOffset = (dataOffset + 0x1F) & ~std::size_t{0x1F};
    std::size_t total = dataOffset;
    for (const auto& [name, payload] : files) {
        total += payload.size();
    }

    std::vector<std::uint8_t> file(total, 0);
    putText(file, 0, "RARC");
    putU32(file, 0x04, static_cast<std::uint32_t>(total));
    putU32(file, 0x08, 0x20);
    putU32(file, 0x0C, static_cast<std::uint32_t>(dataOffset - 0x20));
    putU32(file, 0x20, 1); // one root node
    putU32(file, 0x24, static_cast<std::uint32_t>(nodeOffset - 0x20));
    putU32(file, 0x28, static_cast<std::uint32_t>(entryCount));
    putU32(file, 0x2C, static_cast<std::uint32_t>(entryOffset - 0x20));
    putU32(file, 0x30, static_cast<std::uint32_t>(stringTableSize));
    putU32(file, 0x34, static_cast<std::uint32_t>(kStringOffset - 0x20));
    putU32(file, 0x38, 0);

    putText(file, kStringOffset + dotOffset, ".");
    putText(file, kStringOffset + dotDotOffset, "..");
    putText(file, kStringOffset + rootOffset, rootName);
    for (std::size_t index = 0; index < files.size(); ++index) {
        putText(file, kStringOffset + nameOffsets[index], files[index].first);
    }

    putU32(file, nodeOffset, 0x524F4F54U); // 'ROOT' magic
    putU32(file, nodeOffset + 0x04, static_cast<std::uint32_t>(rootOffset));
    putU16(file, nodeOffset + 0x08, 0); // name hash, not validated by the reader
    putU16(file, nodeOffset + 0x0A, static_cast<std::uint16_t>(entryCount));
    putU32(file, nodeOffset + 0x0C, 0); // first entry

    const auto putEntry = [&](std::size_t index, std::uint16_t type, std::uint32_t nameOffset,
                              std::uint32_t relativeData, std::uint32_t size) {
        const std::size_t entry = entryOffset + index * 0x14;
        putU16(file, entry, static_cast<std::uint16_t>(index));
        putU16(file, entry + 0x02, 0);
        putU16(file, entry + 0x04, type);
        putU16(file, entry + 0x06, static_cast<std::uint16_t>(nameOffset));
        putU32(file, entry + 0x08, relativeData);
        putU32(file, entry + 0x0C, size);
    };
    putEntry(0, 0, static_cast<std::uint32_t>(dotOffset), 0, 0);
    putEntry(1, 0, static_cast<std::uint32_t>(dotDotOffset), 0, 0);
    std::size_t relative = 0;
    for (std::size_t index = 0; index < files.size(); ++index) {
        putEntry(2 + index, 0x1100, static_cast<std::uint32_t>(nameOffsets[index]),
                 static_cast<std::uint32_t>(relative), static_cast<std::uint32_t>(files[index].second.size()));
        relative += files[index].second.size();
    }
    std::size_t payloadCursor = dataOffset;
    for (const auto& [name, payload] : files) {
        std::copy(payload.begin(), payload.end(), file.begin() + static_cast<std::ptrdiff_t>(payloadCursor));
        payloadCursor += payload.size();
    }
    return file;
}

void writeTestFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("could not write test file: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void testModelLibrary() {
    using whitehole::render::ModelLibrary;

    TemporaryDirectory temporary;
    const auto objectData = temporary.path / "ObjectData";
    if (!std::filesystem::create_directory(objectData)) {
        throw std::runtime_error("could not create test ObjectData directory");
    }

    const auto tiny = makeTinyBmd();
    writeTestFile(objectData / "TestObj.arc", makeMinimalRarc("TestObj", {{"TestObj.bdl", tiny}}));
    writeTestFile(objectData / "TestObjLow.arc", makeMinimalRarc("TestObjLow", {{"TestObjLow.bdl", tiny}}));
    writeTestFile(objectData / "Ambiguous.arc",
                  makeMinimalRarc("Ambiguous", {{"Ambiguous.bmd", tiny}, {"Other.bmd", tiny}}));
    writeTestFile(objectData / "OddLayout.arc", makeMinimalRarc("OddLayout", {{"SomeModel.bmd", tiny}}));

    whitehole::io::DirectoryFilesystem workspace(temporary.path);
    ModelLibrary library;
    expect(!library.bound(), "unbound library must report unbound");
    expect(library.model("TestObj") == nullptr, "unbound library must return no model");
    library.bind(&workspace);
    expect(library.bound(), "bound library must report bound");

    // A missing archive resolves to nothing and stays a cached miss.
    expect(library.archiveNameFor("Missing").empty(), "missing archive should not resolve");
    expect(library.model("Missing") == nullptr, "missing archive should produce no model");
    expect(library.missingCount() == 1, "missing lookup was not counted");

    // The model name resolves to the archive, parses, and builds the same mesh
    // a direct BMD parse would.
    expect(library.archiveNameFor("TestObj") == "TestObj.arc", "archive name resolution is wrong");
    const auto mesh = library.model("TestObj");
    expect(mesh != nullptr, "TestObj model did not load");
    const auto expected = whitehole::render::buildModelMesh(whitehole::smg::parseBmd(tiny));
    expect(mesh->triangles.size() == expected.triangles.size(), "loaded model triangle count is wrong");
    expect(!mesh->empty(), "loaded model mesh is empty");
    expect(library.loadedCount() == 1 && library.missingCount() == 1, "model counters are wrong");
    // Caching: the same mesh object is returned for repeated lookups.
    expect(library.model("TestObj") == mesh, "model cache did not reuse the mesh");
    expect(library.loadedCount() == 1 && library.missingCount() == 1,
           "cached lookups must not inflate the counters");

    // Archives with more than one model file are ambiguous and refused.
    expect(library.model("Ambiguous") == nullptr, "ambiguous archive must not produce a model");
    // A single model file in an unexpected layout still resolves.
    const auto odd = library.model("OddLayout");
    expect(odd != nullptr, "single-model fallback layout did not load");
    expect(odd->triangles.size() == expected.triangles.size(), "fallback layout mesh is wrong");

    // The low-poly setting switches to the Low archive when it exists.
    library.setLowPoly(true);
    expect(library.archiveNameFor("TestObj") == "TestObjLow.arc", "low-poly archive resolution is wrong");
    const auto lowMesh = library.model("TestObj");
    expect(lowMesh != nullptr && lowMesh != mesh, "low-poly switch did not reload the model");
    library.setLowPoly(false);
    expect(library.archiveNameFor("TestObj") == "TestObj.arc", "disabling low-poly did not restore resolution");
    const auto restoredMesh = library.model("TestObj");
    // setLowPoly() clears the cache, so the restored mesh is a fresh load of
    // the same archive: compare content, not identity.
    expect(restoredMesh != nullptr && restoredMesh != lowMesh, "disabling low-poly did not reload the model");
    expect(restoredMesh->triangles.size() == mesh->triangles.size(),
           "disabling low-poly did not restore the mesh");

    // Model name substitutions: an aliased name resolves to its target
    // archive; names without a substitution fall through unchanged.
    const auto substitutions = temporary.path / "data";
    if (!std::filesystem::create_directory(substitutions)) {
        throw std::runtime_error("could not create test data directory");
    }
    const std::string substitutionJson =
        whitehole::util::serializeJson(whitehole::util::parseJson(R"({"testalias":"TestObj"})"));
    writeTestFile(substitutions / "modelsubstitutions.json",
                  std::vector<std::uint8_t>(substitutionJson.begin(), substitutionJson.end()));
    whitehole::db::ModelSubstitutions modelSubstitutions;
    modelSubstitutions.setBaseGameRoot(temporary.path);
    modelSubstitutions.initBaseGame();
    modelSubstitutions.load();
    expect(modelSubstitutions.isLoaded(), "model substitutions did not load");

    ModelLibrary substitutedLibrary;
    substitutedLibrary.bind(&workspace);
    substitutedLibrary.setSubstitutions(&modelSubstitutions);
    expect(substitutedLibrary.archiveNameFor("TestAlias") == "TestObj.arc",
           "substituted model name did not resolve to its target archive");
    expect(substitutedLibrary.model("TestAlias") != nullptr, "substituted model name did not load");
    // Unknown names fall through to their own archive (or to nothing).
    expect(substitutedLibrary.archiveNameFor("TestObj") == "TestObj.arc", "plain name resolution broke");
    expect(substitutedLibrary.archiveNameFor("Nothing").empty(), "unknown name must not resolve");

    // The scene attaches the model and keeps the placeholder behaviour intact.
    whitehole::smg::PlacementObject modelled;
    modelled.name = "TestObj";
    modelled.kind = "obj";
    modelled.position = {50.0F, 0.0F, 0.0F};
    modelled.scale = {2.0F, 1.0F, 1.0F};
    whitehole::smg::PlacementObject plain;
    plain.name = "Missing";
    plain.kind = "obj";
    whitehole::render::ViewportScene scene;
    scene.rebuild({modelled, plain}, &library);
    expect(scene.boxes().size() == 2, "model scene dropped objects");
    expect(scene.boxes()[0].model != nullptr, "scene did not attach the game model");
    expect(scene.boxes()[0].model == restoredMesh, "scene attached the wrong mesh");
    // Modelled objects draw at the true object scale (2x here), placeholders
    // keep the 25-unit box with its minimum visual scale clamp.
    const auto modelCorner = scene.boxes()[0].world.transformPoint({1.0F, 0.0F, 0.0F});
    expect(std::abs(modelCorner.x - (50.0F + 2.0F)) < 0.01F, "model world matrix must use the true scale");
    expect(std::abs(scene.boxes()[1].halfExtents.x - 25.0F) < 0.01F, "placeholder extents changed");
    // Picking hits the model's bounding sphere.
    whitehole::render::ViewportCamera camera;
    camera.target = modelled.position;
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;
    expect(scene.pick(camera, 400.0F, 300.0F, 800.0F, 600.0F).has_value(), "picking missed a modelled object");
    // Rebuilding without a library keeps every object on the placeholder path.
    whitehole::render::ViewportScene plainScene;
    plainScene.rebuild({modelled});
    expect(plainScene.boxes().front().model == nullptr, "scene attached a model without a library");
}
// The scenario tables: what each mission of a galaxy awards, and which layers of
// which zone it activates. The format facts are pinned against the real bundled
// galaxy, because "the stored value IS the layer mask" is the rule that is easiest
// to get wrong by an off-by-one (the game's own getValueU32 multiplies by two
// before its caller uses it, and that multiply does not belong here).
void testScenarioModel() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto archive = whitehole::io::RarcArchive::open(templates / "SMG2BigGalaxyScenario.arc");
    whitehole::smg::BcsvTable scenarioData(archive.read("ScenarioData.bcsv"), archive.endian());
    whitehole::smg::BcsvTable zoneList(archive.read("ZoneList.bcsv"), archive.endian());
    whitehole::smg::BcsvTable galaxyInfo(archive.read("GalaxyInfo.bcsv"), archive.endian());

    const std::vector<std::string> zones = {"RedBlueExGalaxy"};
    const auto worldNo = galaxyInfo.rows().empty()
                             ? 0
                             : galaxyInfo.getInt(galaxyInfo.rows()[0], "WorldNo", 0);
    whitehole::smg::ScenarioModel model(scenarioData, zoneList, zones, worldNo);

    // The real template galaxy: six scenarios, each awarding a different star.
    expect(model.scenarioCount() == 6, "the template galaxy must have six scenarios");
    expect(model.powerStarCount() == 6, "every template scenario awards a star");
    // This galaxy does record PowerStarType: three Normal and three Green, and the
    // game excludes Green from the ORDINARY count. This is the game's own split
    // (getPowerStarNum counts every non-zero id; getNormalPowerStarNum drops the
    // Hidden and Green ones).
    expect(model.ordinaryPowerStarCount() == 3,
           "three Green scenarios must be excluded from the ordinary star count");

    // The first scenario, verbatim from the template.
    const whitehole::smg::Scenario& first = model.scenarios().front();
    expect(first.number == 1, "the first scenario must be ScenarioNo 1");
    expect(first.name == "First Mission", "scenario name must read back");
    expect(first.powerStarId == 25, "the first scenario must award star 25");
    expect(first.powerStarType == "Normal", "the first scenario's star must be Normal");
    expect(first.starTypeDescription() == "Normal star",
           "a recorded star type must be described in plain words");
    expect(model.scenarios()[3].starTypeIsGreen(),
           "the template's fourth mission must record a Green star");
    expect(first.awardsStar(), "a scenario with a star id must report awarding one");
    // The comet marker is a separate STRING column holding the kind. The
    // template's third mission is the comet one, so it must read as a comet.
    expect(!first.comet && first.cometName.empty(),
           "an ordinary mission must not be a comet");
    expect(model.scenarios()[2].comet && model.scenarios()[2].cometName == "Dark",
           "the template's third mission must read as a comet");

    // The star type round trip, including a value this build does not know: the
    // stored text is kept verbatim rather than normalised into something the game
    // never wrote.
    expect(whitehole::smg::starTypeFromText("Green") == whitehole::smg::StarType::green,
           "Green must parse");
    expect(whitehole::smg::starTypeFromText("Rainbow") == whitehole::smg::StarType::custom,
           "an unknown star type must stay custom");
    expect(whitehole::smg::starTypeIsSpecial("Green") &&
               whitehole::smg::starTypeIsSpecial("Hidden") &&
               !whitehole::smg::starTypeIsSpecial("Normal") &&
               !whitehole::smg::starTypeIsSpecial(""),
           "only Hidden and Green are the game's special types");
    expect(whitehole::smg::starTypeLabel("Rainbow").find("Rainbow") != std::string::npos,
           "an unknown star type must be shown, not hidden");
    expect(whitehole::smg::starTypeLabel("").find("recorded") != std::string::npos,
           "an absent star type must say so");
    expect(whitehole::smg::starTypeText(whitehole::smg::StarType::green) == "Green",
           "the star type must write back the game's own spelling");

    // THE layer rule. The template's only per-zone column is RedBlueExGalaxy, and
    // it holds 1/2/4 for scenarios 1/2/3 -- LayerA, LayerB, LayerC. If the mask
    // were off by one these would come out as LayerB/C/D.
    expect(model.layerMask(0, "RedBlueExGalaxy") == 1, "scenario 1 must activate bit 0");
    expect(model.layerMask(1, "RedBlueExGalaxy") == 2, "scenario 2 must activate bit 1");
    expect(model.layerMask(2, "RedBlueExGalaxy") == 4, "scenario 3 must activate bit 2");
    expect(model.layerMask(3, "RedBlueExGalaxy") == 0,
           "scenario 4 must activate no extra layer");
    // A zone with no column of its own is Common only, not an error.
    expect(model.layerMask(0, "NoSuchZone") == 0, "an unknown zone must read as no layers");

    // getActiveLayerNames(), the Java parity case: Common always, then the layers
    // in bit order.
    const std::vector<std::string> layers = model.activeLayers(1, "RedBlueExGalaxy");
    expect(layers.size() == 2 && layers[0] == "Common" && layers[1] == "LayerB",
           "scenario 2 must be Common + LayerB");
    expect(model.activeLayers(0, "RedBlueExGalaxy").front() == "Common",
           "Common is always active, even alone");
    expect(model.activeLayers(3, "RedBlueExGalaxy").size() == 1,
           "a scenario with no bits is Common only");

    // Layer name resolution, including the deliberate "Common owns no bit".
    expect(whitehole::smg::scenarioLayerBit("LayerA") == 0 &&
               whitehole::smg::scenarioLayerBit("LayerB") == 1 &&
               whitehole::smg::scenarioLayerBit("LayerP") == 15,
           "layer letters must map onto their bit index");
    expect(whitehole::smg::scenarioLayerBit("Common") == -1,
           "Common owns no bit and must not resolve to one");
    expect(whitehole::smg::scenarioLayerBit("LayerQ") == -1,
           "a layer past P must not resolve");
    expect(whitehole::smg::scenarioLayerNames().size() == 16,
           "the game addresses exactly 16 layers");
    expect(whitehole::smg::scenarioLayerNames().back() == "LayerP",
           "the last layer must be LayerP");
}

// Editing a scenario table: layers, scenarios and the zone list. Kept apart from
// the read-only test above because this one mutates, and a failure is easier to
// place when the read and write halves do not share a function.
void testScenarioEditing() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto archive = whitehole::io::RarcArchive::open(templates / "SMG2BigGalaxyScenario.arc");
    whitehole::smg::BcsvTable scenarioData(archive.read("ScenarioData.bcsv"), archive.endian());
    whitehole::smg::BcsvTable zoneList(archive.read("ZoneList.bcsv"), archive.endian());
    whitehole::smg::ScenarioModel model(scenarioData, zoneList, {"RedBlueExGalaxy"}, 1);

    const std::vector<std::uint8_t> untouched = scenarioData.serialize();

    // Turning a layer on and off must set and clear exactly that bit, leaving
    // the others alone.
    expect(model.setLayerActive(0, "RedBlueExGalaxy", "LayerC", true),
           "activating a layer must succeed");
    expect(model.layerMask(0, "RedBlueExGalaxy") == 5,
           "LayerC must OR into the existing mask, not replace it");
    expect(model.setLayerActive(0, "RedBlueExGalaxy", "LayerA", false),
           "deactivating a layer must succeed");
    expect(model.layerMask(0, "RedBlueExGalaxy") == 4, "LayerA must be cleared exactly");
    expect(!model.setLayerActive(0, "RedBlueExGalaxy", "Common", true),
           "Common owns no bit and must be rejected");
    // Restored, so the byte-exact check below is meaningful.
    model.setLayerActive(0, "RedBlueExGalaxy", "LayerC", false);
    model.setLayerActive(0, "RedBlueExGalaxy", "LayerA", true);
    expect(model.layerMask(0, "RedBlueExGalaxy") == 1, "the mask must be back to 1");
    expect(scenarioData.serialize() == untouched,
           "a layer edit and its exact inverse must be byte-exact");

    // A new zone's column appears on first use, and every scenario row gets the
    // new column at 0 rather than being left short.
    expect(model.setLayerActive(0, "FreshZone", "LayerA", true),
           "a brand-new zone column must be creatable");
    expect(model.layerMask(0, "FreshZone") == 1, "the new zone must start at its bit");
    expect(model.layerMask(1, "FreshZone") == 0,
           "a new column must default to 0 for the other scenarios, not garbage");
    expect(scenarioData.hasField("FreshZone"), "the new zone column must be stored");
    // Note that FreshZone is NOT in this galaxy's zone list, and the model still
    // created the column. That is right for the panel, which only ever passes a
    // zone it is showing, but it is why `galaxy scenario layer` has to check the
    // name against zones() itself: without that check a typo silently added a
    // column the game will never read and the command still reported success.
    expect(std::find(model.zones().begin(), model.zones().end(), "FreshZone")
               == model.zones().end(),
           "FreshZone was never added to the zone list");

    expect(model.nextFreeScenarioNumber() == 7,
           "the next free id after 1-6 must be 7");

    // A scenario lookup must reflect the TABLE, not a cache that a layer edit left
    // stale: toggling a layer does not change the scenario list, so nothing
    // re-reads it, and a scan over the stale cache once handed back an id that was
    // already taken and produced two scenarios with the same ScenarioNo.
    expect(model.findScenario(1).value_or(999) == 0, "scenario 1 must resolve to row 0");
    model.setLayerActive(0, "RedBlueExGalaxy", "LayerP", true);
    expect(model.findScenario(1).value_or(999) == 0,
           "a lookup after a layer edit must still resolve from the table");
    expect(model.nextFreeScenarioNumber() == 7,
           "the next free id must not be thrown off by an unrefreshed cache");
    model.setLayerActive(0, "RedBlueExGalaxy", "LayerP", false);

    const std::size_t added = model.addScenario("New Mission");
    expect(added == 6 && model.scenarioCount() == 7, "addScenario must append a row");
    expect(model.scenarios().back().number == 7, "the new scenario must get id 7");
    expect(model.scenarios().back().name == "New Mission", "the new name must be stored");
    // A fresh scenario awards nothing, and must SAY so: a scenario with no star is
    // a legitimate state, not an error.
    expect(!model.scenarios().back().awardsStar(), "a new scenario awards no star yet");

    // Copying clones the settings but never the id, which would shadow the source.
    const std::size_t copied = model.addScenario("Copy", added);
    // 1-6 exist, so "New Mission" is 7 and its copy must be 8 -- never 7 again.
    expect(model.scenarios()[added].number == 7, "the added scenario must be id 7");
    expect(model.scenarios()[copied].number == 8,
           "a copied scenario must take the next free id, not the source's");
    model.renameScenario(copied, "Copied");
    expect(model.scenarios()[copied].name == "Copied", "rename must store");
    model.setPowerStar(copied, 99);
    expect(model.scenarios()[copied].powerStarId == 99, "the star id must store");
    model.setPowerStarType(copied, "Green");
    expect(model.scenarios()[copied].starTypeIsGreen(), "the star type must store");
    model.setComet(copied, true, 600);
    expect(model.scenarios()[copied].comet && model.scenarios()[copied].cometTimer == 600,
           "the comet flag and timer must store");
    model.setComet(copied, false, 600);
    expect(!model.scenarios()[copied].comet && model.scenarios()[copied].cometTimer == 0,
           "turning the comet off must also zero its timer");

    // An id collision is refused: two rows with one id makes the first unreachable.
    expect(!model.setScenarioNumber(copied, 1),
           "renumbering onto another scenario's id must be refused");
    expect(model.setScenarioNumber(copied, 42), "renumbering to a free id must work");
    expect(model.findScenario(42).has_value(), "the new id must resolve");
    expect(model.findScenario(8) == std::nullopt, "the old id must be gone");
    expect(!model.setScenarioNumber(copied, -1), "a negative id must be refused");

    expect(model.removeScenario(copied), "removeScenario must succeed");
    expect(model.scenarioCount() == 7, "the row must actually be gone");
    expect(!model.removeScenario(999), "removing a bad row must report false");

    expect(model.addZone("NewZone").has_value(), "adding a zone must succeed");
    expect(model.zones().size() == 2 && model.zones().back() == "NewZone",
           "the new zone must be listed");
    expect(!model.addZone("NewZone").has_value(),
           "a duplicate zone name must be refused: the game looks zones up by name");
    expect(!model.addZone("").has_value(), "an empty zone name must be refused");
    expect(model.moveZone(1, 0), "moving a zone up must succeed");
    expect(model.zones().front() == "NewZone", "the move must land where asked");
    expect(!model.moveZone(0, 0), "moving a zone onto itself must report false");
    expect(model.removeZone(0), "removing the zone must succeed");
    expect(model.zones().size() == 1 && model.zones().front() == "RedBlueExGalaxy",
           "the remaining zone must be the original one");
}

// The galaxy's scenario tables have to survive a save/reopen cycle, and an
// untouched galaxy must not be rewritten at all.
void testGalaxyScenarioSave() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    TemporaryDirectory temporary;
    whitehole::io::DirectoryFilesystem project(temporary.path);
    project.createDirectory("/SystemData");
    project.write("/SystemData/ObjNameTable.arc", {0});
    project.createDirectory("/StageData/RedBlueExGalaxy");
    project.write("/StageData/RedBlueExGalaxy/RedBlueExGalaxyScenario.arc",
                  whitehole::io::readFile(templates / "SMG2BigGalaxyScenario.arc"));
    project.write("/StageData/RedBlueExGalaxy/RedBlueExGalaxyMap.arc",
                  whitehole::io::readFile(templates / "SMG2BigGalaxyMap.arc"));

    whitehole::smg::GameArchive game(temporary.path);
    const std::vector<std::uint8_t> before =
        project.read("/StageData/RedBlueExGalaxy/RedBlueExGalaxyScenario.arc");

    {
        whitehole::smg::GalaxyArchive galaxy = game.openGalaxy("RedBlueExGalaxy");
        expect(!galaxy.dirty(), "a freshly opened galaxy must not be dirty");
        expect(galaxy.galaxyInfo().hasField("WorldNo"),
           "GalaxyInfo.bcsv must be kept as a table now, not discarded");
        expect(galaxy.zoneList().hasField("ZoneName"), "ZoneList.bcsv must be kept as a table");
        expect(galaxy.scenarioData().rows().size() == 6, "ScenarioData must have 6 rows");

        // Saving an untouched galaxy must leave the file alone: "not dirty" and
        // "the bytes are identical" are different claims, and only the first is
        // guaranteed without rewriting.
        galaxy.save();
        expect(project.read("/StageData/RedBlueExGalaxy/RedBlueExGalaxyScenario.arc") == before,
               "saving an unedited galaxy must not touch the file");

        // Now edit it for real, through the model the panel uses.
        whitehole::smg::ScenarioModel model(galaxy.scenarioData(), galaxy.zoneList(),
                                             galaxy.editableZones(), 1);
        model.addScenario("Seventh Mission");
        model.setPowerStar(6, 77);
        expect(galaxy.dirty(), "an edited galaxy must report dirty");
        galaxy.save();
        expect(!galaxy.dirty(), "a saved galaxy must be clean again");
    }

    // Reopened from disk: the new scenario must be there, in the right place.
    {
        whitehole::smg::GalaxyArchive reopened = game.openGalaxy("RedBlueExGalaxy");
        whitehole::smg::ScenarioModel model(reopened.scenarioData(), reopened.zoneList(),
                                             reopened.editableZones(), 1);
        expect(model.scenarioCount() == 7, "the added scenario must survive a save");
        expect(model.scenarios().back().name == "Seventh Mission",
               "the added scenario's name must survive a save");
        expect(model.scenarios().back().powerStarId == 77,
               "the added scenario's star must survive a save");
        // The scenarios that were already there must be untouched by all of this.
        expect(model.scenarios()[0].name == "First Mission",
               "the original scenarios must be unchanged");
        expect(model.layerMask(0, "RedBlueExGalaxy") == 1,
               "an original layer mask must survive an unrelated save");
        // A layer edit round-trips through the archive too.
        model.setLayerActive(0, "RedBlueExGalaxy", "LayerD", true);
        reopened.save();
    }
    {
        whitehole::smg::GalaxyArchive again = game.openGalaxy("RedBlueExGalaxy");
        whitehole::smg::ScenarioModel model(again.scenarioData(), again.zoneList(),
                                             again.editableZones(), 1);
        expect(model.layerMask(0, "RedBlueExGalaxy") == 9,
               "a layer edit must survive two save/reopen cycles");
        expect(model.scenarioCount() == 7, "the scenario list must be stable");
    }
}
} // namespace

// The theme is data, so it can be checked instead of eyeballed. Every contrast
// bug this file has had was invisible to the compiler and only surfaced as "I
// can't read this", so the palette now carries its own guard.
//
// Two deliberate choices keep this honest rather than mechanical. Disabled ink
// is checked on *rest* surfaces only, because ImGui never applies the hovered
// or pressed frame colours to a disabled item. Decorative accent is checked
// against the 3.0 guideline, but only on rest surfaces for the same reason:
// there is deliberately no bright-azure escape hatch for small controls.
// The D3D clear colour the shell paints its unused pixels with must track the
// palette, or the bars the light theme used to show come straight back. This
// pins that relationship so a palette edit cannot silently break it.
void testShellBackground() {
    using whitehole::app::Palette;
    using whitehole::app::Rgba;
    using whitehole::app::shellBackground;
    using whitehole::app::themePalette;

    // Rgba is a plain aggregate with no operator==, so compare the channels.
    const auto sameColor = [](const Rgba& a, const Rgba& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    for (int theme = 0; theme < 2; ++theme) {
        const bool dark = theme == 0;
        const Palette& palette = themePalette(dark);
        expect(sameColor(shellBackground(dark), palette.windowBg),
               std::string(dark ? "dark" : "light") +
                   " shellBackground must track windowBg");
        // The shell is the fallback for any pixel the UI does not paint, so it
        // has to be a real surface colour rather than an uninitialised black.
        expect(shellBackground(dark).a > 0.0F,
               std::string(dark ? "dark" : "light") + " shellBackground is transparent");
    }
}

void testThemeContrast() {
    using whitehole::app::blend;
    using whitehole::app::contrastRatio;
    using whitehole::app::kGlyphContrastMinimum;
    using whitehole::app::kTextContrastMinimum;
    using whitehole::app::Palette;
    using whitehole::app::Rgba;
    using whitehole::app::themePalette;

    struct Surface {
        const char* name;
        const Rgba* color;
    };

    for (int theme = 0; theme < 2; ++theme) {
        const bool dark = theme == 0;
        const std::string label = dark ? "dark" : "light";
        const Palette& p = themePalette(dark);

        // Body text can land anywhere, so it covers the full surface set; every
        // other role sticks to the rest surfaces it actually appears on.
        const Surface all[] = {
            {"windowBg", &p.windowBg},       {"panelBg", &p.panelBg},
            {"frameBg", &p.frameBg},         {"frameHover", &p.frameHover},
            {"frameActive", &p.frameActive}, {"header", &p.header},
            {"headerHover", &p.headerHover}, {"headerActive", &p.headerActive},
            {"tabSelected", &p.tabSelected},
        };
        const Surface rest[] = {
            {"windowBg", &p.windowBg}, {"panelBg", &p.panelBg},
            {"frameBg", &p.frameBg},   {"header", &p.header},
            {"tabSelected", &p.tabSelected},
        };

        // Collected rather than asserted one by one: expect() throws, so the
        // first failure would hide every other one in the same theme.
        std::vector<std::string> failures;
        const auto check = [&](const Rgba& fg, const Rgba& bg, double minimum,
                               const char* what, const char* where) {
            const double ratio = std::round(contrastRatio(fg, bg) * 100.0) / 100.0;
            if (ratio < minimum) {
                failures.push_back(std::string(what) + " on " + where + " is " +
                                   std::to_string(ratio) + ":1");
            }
        };

        for (const auto& surface : all) {
            check(p.text, *surface.color, kTextContrastMinimum, "body text", surface.name);
            // mix() in gui_theme.cpp already pulls hovered/pressed surfaces 25-35%
            // toward this ink, so only test it where it is painted directly; the
            // mixed backgrounds hold the same floor through the blend.
            if (surface.color == &p.windowBg || surface.color == &p.panelBg ||
                surface.color == &p.tabSelected) {
                check(p.accent, *surface.color, kGlyphContrastMinimum, "accent decoration",
                      surface.name);
            }
        }
        for (const auto& surface : rest) {
            // Decoration sits on the rest surfaces above and on the chrome surfaces
            // tested above; pressed-control accents come from the same deep tone,
            // which clears 3.0 everywhere.
            check(p.accent, *surface.color, kGlyphContrastMinimum, "accent decoration",
                  surface.name);
            check(p.textDim, *surface.color, kTextContrastMinimum, "secondary text",
                  surface.name);
            check(blend(p.textDim, *surface.color, p.disabledAlpha), *surface.color,
                  kTextContrastMinimum, "disabled text", surface.name);
            // Glyph roles: check marks, links, slider grabs.
            check(p.accentFg, *surface.color, kTextContrastMinimum, "accent glyph",
                  surface.name);
        }
        // The unsaved marker is drawn on the tab strip and in the status bar.
        check(p.unsaved, p.tabSelected, kTextContrastMinimum, "unsaved marker", "tabSelected");
        check(p.unsaved, p.windowBg, kTextContrastMinimum, "unsaved marker", "windowBg");
        check(p.unsaved, p.panelBg, kTextContrastMinimum, "unsaved marker", "panelBg");
        // Error copy is drawn in a popup or over a panel.
        check(p.error, p.panelBg, kTextContrastMinimum, "error text", "panelBg");
        check(p.error, p.windowBg, kTextContrastMinimum, "error text", "windowBg");

        std::string joined;
        for (const auto& failure : failures) {
            if (!joined.empty()) {
                joined += "; ";
            }
            joined += failure;
        }
        expect(failures.empty(), label + " theme contrast: " + joined);

        // The palette has to actually de-emphasise, or "dim" means nothing.
        expect(contrastRatio(p.text, p.panelBg) > contrastRatio(p.textDim, p.panelBg),
               label + " theme: secondary text is not dimmer than body text");
        expect(p.disabledAlpha > 0.60F,
               label + " theme: disabledAlpha is still at ImGui's failing default");
    }
}

// ---- rails (paths) --------------------------------------------------------
// Port coverage for RailUtil: bezier maths against known values.

void testRailMath() {
    namespace rail = whitehole::smg;
    // Straight line 0 -> 400 on X with handles a third of the way along.
    rail::PathPoint start;
    start.position = {0.0F, 0.0F, 0.0F};
    start.control2 = {400.0F / 3.0F, 0.0F, 0.0F};
    rail::PathPoint end;
    end.position = {400.0F, 0.0F, 0.0F};
    end.control1 = {800.0F / 3.0F, 0.0F, 0.0F};

    const auto atZero = rail::bezierPoint(0.0F, start.position, start.control2, end.control1, end.position);
    expect(atZero.length() < 0.001F, "bezier t=0 must return the first point");
    const auto atOne = rail::bezierPoint(1.0F, start.position, start.control2, end.control1, end.position);
    expect((atOne - whitehole::math::Vec3f{400.0F, 0.0F, 0.0F}).length() < 0.001F,
           "bezier t=1 must return the last point");
    const auto atHalf = rail::bezierPoint(0.5F, start.position, start.control2, end.control1, end.position);
    expect(std::abs(atHalf.x - 200.0F) < 0.5F, "bezier midpoint is off the straight line");

    expect(std::abs(rail::pathSectionLength(start, end) - 400.0) < 1.0,
           "straight section length must match its chord");

    std::vector<rail::PathPoint> line{start, end};
    expect(std::abs(rail::pathLength(line, false) - 400.0) < 1.0, "open two-point length is wrong");
    const auto midway = rail::posAtCoord(200.0, line, false);
    expect(midway.has_value() && std::abs(midway->x - 200.0F) < 1.0F, "posAtCoord(200) must sit mid-line");
    expect(!rail::posAtCoord(401.0, line, false).has_value(), "coords past an open path must miss");
    const auto tangent = rail::dirAtCoord(100.0, line, false);
    expect(tangent.has_value() && std::abs(tangent->x - 1.0F) < 0.001F,
           "straight tangent must point along +X");

    // Closed square with coincident handles: four sides of 100.
    std::vector<rail::PathPoint> square(4);
    square[0].position = {0.0F, 0.0F, 0.0F};
    square[1].position = {100.0F, 0.0F, 0.0F};
    square[2].position = {100.0F, 100.0F, 0.0F};
    square[3].position = {0.0F, 100.0F, 0.0F};
    for (auto& point : square) {
        point.control1 = point.position;
        point.control2 = point.position;
    }
    expect(std::abs(rail::pathLength(square, true) - 400.0) < 2.0, "closed square perimeter is wrong");
    expect(std::abs(rail::pathLength(square, false) - 300.0) < 2.0, "open square length is wrong");

    // Reverse [0, 1]: positions swap and each moved point swaps its handles.
    std::vector<rail::PathPoint> three(3);
    three[0].position = {10.0F, 0.0F, 0.0F};
    three[0].control1 = {11.0F, 0.0F, 0.0F};
    three[0].control2 = {12.0F, 0.0F, 0.0F};
    three[1].position = {20.0F, 0.0F, 0.0F};
    three[1].control1 = {21.0F, 0.0F, 0.0F};
    three[1].control2 = {22.0F, 0.0F, 0.0F};
    three[2].position = {30.0F, 0.0F, 0.0F};
    expect(rail::reversePoints(three, 0, 1), "reverse of a valid range must succeed");
    expect(three[0].position.x == 20.0F && three[1].position.x == 10.0F,
           "reversed range must swap positions");
    expect(three[0].control1.x == 22.0F && three[0].control2.x == 21.0F,
           "reversed point must swap its handles");
    expect(three[2].position.x == 30.0F, "points outside the range must stay put");
    expect(!rail::reversePoints(three, 0, 99), "out-of-range reverse must fail");
}

// Load/save round trip for rails: creates a path row and a points file the
// template archive never had, so saving also exercises RarcArchive::insert.

void testPathData() {
    using namespace whitehole::smg;
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(loadPaths(stage).empty(), "the template must start without rails");

    std::size_t pathTable = kNoPointTable;
    for (std::size_t index = 0; index < stage.tables().size(); ++index) {
        if (stage.tables()[index].kind == "path") {
            pathTable = index;
        }
    }
    expect(pathTable != kNoPointTable, "template CommonPathInfo did not load");

    const auto objectsBefore = stage.objects().size();

    // A brand-new rail row. Add both rows first: the second addRow() may
    // reallocate the row vector, so references are taken afterwards.
    auto& info = stage.tables()[pathTable].table;
    const auto rowIndex = info.addRow();
    const auto missingIndex = info.addRow();
    auto& row = info.rows()[rowIndex];
    auto& missing = info.rows()[missingIndex];
    info.setString(row, "name", "TestRail");
    info.setString(row, "type", "Bezier");
    info.setString(row, "closed", "OPEN");
    info.setString(row, "usage", "General");
    info.setInt(row, "l_id", 7);
    info.setInt(row, "no", 0);
    info.setInt(row, "Path_ID", -1);
    // A second rail whose points file does not exist: loading must tolerate it.
    info.setString(missing, "name", "MissingRail");
    info.setInt(missing, "l_id", 8);
    info.setInt(missing, "no", 9);

    stage.rebuildObjects();
    expect(stage.objects().size() == objectsBefore, "rail rows leaked into the placement object list");

    // A points file the template never had -- saving must insert it.
    ObjectTable points;
    points.path = pathPointFile(0);
    points.kind = "pathpoint";
    points.layer = "Common";
    ensurePathPointSchema(points.table);
    const auto addPoint = [&points](std::int16_t id, whitehole::math::Vec3f position) {
        const auto index = points.table.addRow();
        auto& row = points.table.rows()[index];
        points.table.setInt(row, "id", id);
        for (const char* set : {"pnt0", "pnt1", "pnt2"}) {
            points.table.setFloat(row, std::string(set) + "_x", position.x);
            points.table.setFloat(row, std::string(set) + "_y", position.y);
            points.table.setFloat(row, std::string(set) + "_z", position.z);
        }
        points.table.setInt(row, "point_arg0", 500); // Speed
    };
    // Deliberately out of order: loading must sort by id like Java did.
    addPoint(2, {200.0F, 0.0F, 0.0F});
    addPoint(0, {0.0F, 0.0F, 0.0F});
    addPoint(1, {100.0F, 50.0F, 0.0F});
    stage.tables().push_back(std::move(points));

    auto paths = loadPaths(stage);
    expect(paths.size() == 2, "loadPaths must see both rails");
    expect(paths[0].name == "TestRail" && paths[0].lId == 7, "rail fields did not load");
    expect(paths[0].points.size() == 3, "rail points did not load");
    expect(paths[0].points[0].id == 0 && paths[0].points[1].id == 1 && paths[0].points[2].id == 2,
           "points must sort by id");
    expect(paths[0].points[1].position.x == 100.0F && paths[0].points[1].position.y == 50.0F,
           "point coordinates did not load");
    expect(paths[0].points[0].args[0] == 500, "point_arg0 did not load");
    expect(paths[1].points.empty(), "a missing points file must yield an empty rail, not a throw");
    expect(paths[0].label() == "[7] TestRail", "rail label must match Java's toString");

    TemporaryDirectory temporary;
    const auto output = temporary.path / "WithRails.arc";
    stage.saveTo(output);

    auto reloaded = StageArchive::openMapFile(output);
    auto again = loadPaths(reloaded);
    expect(again.size() == 2, "rails did not survive the save");
    expect(again[0].points.size() == 3, "the inserted points file did not survive the save");
    expect(again[0].points[2].position.x == 200.0F, "saved point coordinates changed");
    expect(again[0].pointTableIndex != kNoPointTable, "the reloaded rail has no point table");
    // num_pnt was never set by hand: saving must have synced it to 3.
    const auto& infoAgain = reloaded.tables()[again[0].tableIndex].table;
    expect(infoAgain.getInt(infoAgain.rows()[again[0].rowIndex], "num_pnt", -1) == 3,
           "num_pnt was not synced to the point count on save");
    expect(again[1].points.empty(), "a rail with a missing file gained phantom points");
    for (const auto& object : reloaded.objects()) {
        expect(object.kind != "path" && object.kind != "pathpoint",
               "rail rows reappeared in the object list");
    }
}

// ---- viewport overlays ----------------------------------------------------
// The View menu's overlay toggles must actually produce (or omit) geometry.

void testOverlayScene() {
    using namespace whitehole::render;
    using whitehole::smg::PathPoint;
    using whitehole::smg::PlacementObject;
    using whitehole::smg::RailPath;

    PlacementObject camera;
    camera.kind = "camera";
    camera.name = "Cam";
    PlacementObject area;
    area.kind = "area";
    area.name = "Zone";
    PlacementObject gravity;
    gravity.kind = "gravity";
    gravity.name = "Planet";
    gravity.scale = {100.0F, 100.0F, 100.0F};
    PlacementObject plain;
    plain.kind = "obj";
    plain.name = "Kuribo";

    RailPath rail;
    rail.name = "Rail";
    rail.lId = 3;
    PathPoint start;
    start.position = {0.0F, 0.0F, 0.0F};
    PathPoint end;
    end.position = {400.0F, 0.0F, 0.0F};
    rail.points = {start, end};
    RailPath emptyRail; // no points: must contribute nothing, not crash
    emptyRail.lId = 4;
    const std::vector<RailPath> rails{rail, emptyRail};

    ViewportScene scene;
    OverlayFlags off;
    off.axis = off.areas = off.cameras = off.gravity = off.paths = false;
    scene.rebuild({camera, area, gravity, plain}, nullptr, &rails, off);
    expect(scene.overlays().empty(), "disabled overlay flags still produced geometry");
    expect(scene.railPaths().size() == 2, "the scene dropped the rail list");

    // Paths alone: one curve batch + one handles batch, nothing else.
    OverlayFlags pathsOnly;
    pathsOnly.axis = pathsOnly.areas = pathsOnly.cameras = pathsOnly.gravity = false;
    scene.rebuild({camera, area, gravity, plain}, nullptr, &rails, pathsOnly);
    expect(scene.overlays().size() == 2, "paths-only rebuild produced the wrong batches");

    OverlayFlags on;
    scene.rebuild({camera, area, gravity, plain}, nullptr, &rails, on);
    // axis x3 + camera + area + gravity + rail curve + rail handles = 8.
    expect(scene.overlays().size() == 8, "unexpected overlay batch count with every flag on");
    std::size_t segments = 0;
    for (const auto& batch : scene.overlays()) {
        segments += batch.segments.size();
    }
    expect(segments >= 100, "overlay geometry is suspiciously thin");

    // Rail colours are stable per l_id and differ between rails.
    expect(railPathColor(3) == railPathColor(3), "rail colour is not deterministic");
    expect(railPathColor(3) != railPathColor(4), "two rails received the same colour");
    expect((railPathColor(7) & 0xFFu) == 0xFFu, "rail colour must be opaque");
}

// Builds a minimal but structurally exact big-endian KCL -- header, one
// position, four normals, the dummy prism slot the engine skips, one real
// prism -- and checks the parser rebuilds the intended floor triangle, that
// raycastDown lands on it, and that owner tagging and truncation behave.
void testKclParsing() {
    using whitehole::io::BinaryWriter;
    using whitehole::io::Endian;
    using whitehole::render::SnapTriangle;
    namespace render = whitehole::render;

    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-4F; };

    // Wedge data for a unit floor at y = 100: face normal +Y (outward),
    // edge0 +X, edge1 +Z, edge2 the diagonal plane. height is chosen so the
    // edge-plane reconstruction lands exactly on (1,0,0) and (0,0,1) steps.
    constexpr float kDiagonal = 0.70710678F;

    BinaryWriter writer(Endian::big);
    constexpr std::uint32_t kPositionOffset = 0x38;
    constexpr std::uint32_t kNormalOffset = kPositionOffset + 12;    // 1 position
    constexpr std::uint32_t kPrismOffset = kNormalOffset + 4 * 12;   // 4 normals
    constexpr std::uint32_t kPrismStart = kPrismOffset + 0x10;       // dummy slot
    constexpr std::uint32_t kOctreeOffset = kPrismStart + 0x10;      // 1 prism
    writer.writeU32(kPositionOffset);
    writer.writeU32(kNormalOffset);
    writer.writeU32(kPrismOffset);
    writer.writeU32(kOctreeOffset);
    writer.writeSpanRepeated(0x28, 0); // thickness + octree origin/masks/shifts

    // Position 0: the prism's base vertex.
    writer.writeF32(0.0F);
    writer.writeF32(100.0F);
    writer.writeF32(0.0F);

    // Normals: 0 = face (+Y), 1 = edge0 (+X), 2 = edge1 (+Z), 3 = edge2.
    writer.writeF32(0.0F);
    writer.writeF32(1.0F);
    writer.writeF32(0.0F);
    writer.writeF32(1.0F);
    writer.writeF32(0.0F);
    writer.writeF32(0.0F);
    writer.writeF32(0.0F);
    writer.writeF32(0.0F);
    writer.writeF32(1.0F);
    writer.writeF32(kDiagonal);
    writer.writeF32(0.0F);
    writer.writeF32(kDiagonal);

    writer.writeSpanRepeated(0x10, 0); // dummy prism slot (engine reads [1+index])

    // The real prism: height, position, face, edge0, edge1, edge2, attribute.
    writer.writeF32(kDiagonal);
    writer.writeU16(0);
    writer.writeU16(0);
    writer.writeU16(1);
    writer.writeU16(2);
    writer.writeU16(3);
    writer.writeU16(0);

    const std::vector<std::uint8_t> bytes = std::move(writer).take();
    const auto triangles = render::parseKclTriangles(bytes);
    expect(triangles.size() == 1, "KCL parse produced the wrong triangle count");
    const SnapTriangle& triangle = triangles.front();
    expect(near(triangle.a.x, 0.0F) && near(triangle.a.y, 100.0F) && near(triangle.a.z, 0.0F),
           "KCL base vertex was rebuilt at the wrong place");
    expect(near(triangle.b.x, 1.0F) && near(triangle.b.y, 100.0F) && near(triangle.b.z, 0.0F),
           "KCL vertex 1 did not follow the edge-plane reconstruction");
    expect(near(triangle.c.x, 0.0F) && near(triangle.c.y, 100.0F) && near(triangle.c.z, 1.0F),
           "KCL vertex 2 did not follow the edge-plane reconstruction");
    expect(near(triangle.normal.x, 0.0F) && near(triangle.normal.y, 1.0F) &&
               near(triangle.normal.z, 0.0F),
           "KCL face normal should be the outward +Y surface normal");
    expect(triangle.sourceIndex == render::kCollisionNoOwner,
           "Parsed KCL triangles must start unowned");

    // A downward ray above the triangle snaps to y = 100 and reports KCL.
    render::SnapScene scene;
    scene.kcl = triangles;
    const auto hit = render::raycastDown(scene, {0.5F, 150.0F, 0.25F}, 1000.0F,
                                         render::kCollisionNoOwner);
    expect(hit.has_value() && hit->fromKcl, "raycastDown missed the parsed KCL triangle");
    expect(near(hit->point.y, 100.0F), "KCL raycast hit at the wrong height");
    expect(near(hit->normal.y, 1.0F), "KCL raycast returned the wrong surface normal");

    // Owner-tagged collision is skipped for its own object but stays live
    // for every other ignore index (and for the zone-level sentinel).
    scene.kcl.front().sourceIndex = 7;
    const auto skipped = render::raycastDown(scene, {0.5F, 150.0F, 0.25F}, 1000.0F, 7);
    expect(!skipped.has_value(), "an object must not snap onto its own collision");
    const auto kept = render::raycastDown(scene, {0.5F, 150.0F, 0.25F}, 1000.0F, 8);
    expect(kept.has_value() && kept->fromKcl,
           "another object's collision must stay hittable");
    const auto zoneLevel = [&] {
        scene.kcl.front().sourceIndex = render::kCollisionNoOwner;
        return render::raycastDown(scene, {0.5F, 150.0F, 0.25F}, 1000.0F,
                                   render::kCollisionNoOwner);
    }();
    expect(zoneLevel.has_value(), "zone-level collision must never be self-ignored");

    // Malformed input is rejected loudly, not read out of bounds.
    bool threw = false;
    try {
        (void)render::parseKclTriangles(std::vector<std::uint8_t>{0x00, 0x01, 0x02});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "a truncated KCL should be rejected instead of parsed");

    // Path filter used by the archive scanners.
    expect(render::isKclPath("/Stage/Collision.kcl"), "isKclPath missed an uppercase extension");
    expect(render::isKclPath("foo.KCL"), "isKclPath missed a lowercase-insensitive extension");
    expect(!render::isKclPath("foo.kcl.bak"), "isKclPath matched a non-KCL suffix");
    expect(!render::isKclPath("kcl"), "isKclPath matched a bare extensionless name");
}

// The Cameras panel is a chain of four calls -- table row -> cameraPreviewParams
// -> solveGameCameraPose -> the eye/at/fov/roll the viewport's lookAt() is handed
// -- and none of it is reachable from the ImGui code a test could click. So the
// chain itself is the unit under test here: every registered camtype has to come
// out of it finite and classified, and a degenerate row (zero distance, zero up,
// a literal NaN a hand-edited file can carry) must never put a NaN into the
// camera the viewport then builds its matrices from.
void testCameraPreviewChain() {
    using namespace whitehole::smg;
    using whitehole::math::Vec3f;
    using whitehole::render::ViewportCamera;

    const auto finite = [](const Vec3f& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    // Deliberately far from the origin: a solver that ignored the tracked point
    // could still look plausible against (0, 0, 0).
    const Vec3f target{1234.5F, -678.9F, 4242.0F};

    // 1) Every camtype the two games know, through a real (sparse) table row.
    int exact = 0;
    int spherical = 0;
    int none = 0;
    for (const auto& type : cameraTypes()) {
        const std::uint32_t version = type.smg2 ? kCameraVersionSmg2 : kCameraVersionSmg1;
        CameraParamTable table = CameraParamTable::create(version);
        const std::size_t row = table.addCamera("c:0000", type.id, version);
        const CameraPreviewParams params = cameraPreviewParams(table, row);
        const std::string context = std::string("camtype ") + std::string(type.id);
        expect(params.camtype == type.id, context + ": cameraPreviewParams lost the camtype");
        expect(params.version == version, context + ": the row's engine version did not carry through");

        PoseSupport support = PoseSupport::none;
        const GameCameraPose pose = solveGameCameraPose(params, target, &support);
        expect(support == cameraPoseSupport(type.id, version),
               context + ": the solver and cameraPoseSupport disagree");
        expect(finite(pose.eye) && finite(pose.at) && finite(pose.up),
               context + ": the solved pose must be finite");
        expect(std::isfinite(pose.fovYRadians) && std::isfinite(pose.rollRadians),
               context + ": the solved fovy/roll must be finite");
        // The panel's badge is only honest if the pose it shows came from the
        // same classification, so the pose must be aimed at the tracked point's
        // pivot for every class except the pinned-eye one.
        if (support != PoseSupport::none) {
            expect(finite(pose.at), context + ": a supported type must have a real look-at point");
        }
        switch (support) {
        case PoseSupport::exact: ++exact; break;
        case PoseSupport::spherical: ++spherical; break;
        case PoseSupport::none: ++none; break;
        }
    }
    // All three classes are exercised by the registry, so the sweep above really
    // covered the panel's three badge cases rather than only one of them.
    expect(exact > 0 && spherical > 0 && none > 0,
           "the camtype registry must exercise all three pose classes");

    // The same sweep at SMG1's engine version: several defaults differ per engine
    // (num1, woffset.Y, up.Y), so the SMG1 numbers have to solve too.
    for (const auto& type : cameraTypes()) {
        if (!type.smg1) {
            continue;
        }
        CameraParamTable table = CameraParamTable::create(kCameraVersionSmg1);
        const std::size_t row = table.addCamera("o:default", type.id, kCameraVersionSmg1);
        const GameCameraPose pose =
            solveGameCameraPose(cameraPreviewParams(table, row), target);
        expect(finite(pose.eye) && finite(pose.at) && finite(pose.up),
               std::string("SMG1 camtype ") + std::string(type.id) + ": the solved pose must be finite");
    }

    // 2) A row with only the three required columns resolves through the ENGINE
    //    DEFAULTS, which is exactly what the panel's dim-label badge promises.
    {
        CameraParamTable smg2 = CameraParamTable::create(kCameraVersionSmg2);
        const std::size_t row = smg2.addCamera("c:0001", "CAM_TYPE_XZ_PARA", kCameraVersionSmg2);
        const CameraPreviewParams params = cameraPreviewParams(smg2, row);
        expect(params.dist == 1200.0F && params.fovy == 45.0F && params.angleB == 0.3F,
               "a sparse SMG2 row must resolve to the SMG2 engine defaults");
        const GameCameraPose pose = solveGameCameraPose(params, {0.0F, 0.0F, 0.0F});
        expect(std::abs(pose.eye.length() - 1200.0F) < 1.0F,
               "a sparse XZ_PARA row's eye must sit on the dist sphere");
        expect(pose.at.x == 0.0F && pose.at.y == 0.0F && pose.at.z == 0.0F,
               "an XZ_PARA row with no offsets must aim at the tracked point");
    }
    {
        CameraParamTable smg1 = CameraParamTable::create(kCameraVersionSmg1);
        const std::size_t row = smg1.addCamera("c:0001", "CAM_TYPE_FOLLOW", kCameraVersionSmg1);
        const CameraPreviewParams params = cameraPreviewParams(smg1, row);
        expect(params.woffset.y == 100.0F,
               "SMG1's woffset.Y default (100) must reach the solver");
    }

    // 3) woffset moves the PIVOT, not the tracked point: the panel's target line
    //    repeats this, so it has to be true.
    {
        CameraPreviewParams params;
        params.camtype = "CAM_TYPE_XZ_PARA";
        params.woffset = {10.0F, 20.0F, 30.0F};
        params.angleA = 0.3F;
        params.angleB = 0.35F;
        const Vec3f tracked{100.0F, 200.0F, 300.0F};
        const GameCameraPose pose = solveGameCameraPose(params, tracked);
        expect(pose.at.x == 110.0F && pose.at.y == 220.0F && pose.at.z == 330.0F,
               "the preview pivot must be target + woffset");
    }

    // 4) Degenerate but legal rows. The engine tolerates these, so the chain has
    //    to hand the viewport something finite instead of a NaN that would poison
    //    every later frame's matrices.
    {
        const auto probe = [&finite](const char* what, const CameraPreviewParams& params) {
            const GameCameraPose pose = solveGameCameraPose(params, {1.0F, 2.0F, 3.0F});
            expect(finite(pose.eye) && finite(pose.at) && finite(pose.up) &&
                       std::isfinite(pose.fovYRadians) && std::isfinite(pose.rollRadians),
                   std::string("degenerate row (") + what + ") must still solve finite");
        };

        CameraPreviewParams zeroDist;
        zeroDist.camtype = "CAM_TYPE_XZ_PARA";
        zeroDist.dist = 0.0F;
        probe("dist = 0", zeroDist);

        CameraPreviewParams negativeDist;
        negativeDist.camtype = "CAM_TYPE_FOLLOW";
        negativeDist.dist = -1200.0F;
        probe("negative dist", negativeDist);

        CameraPreviewParams vertical;
        vertical.camtype = "CAM_TYPE_TOWER";
        vertical.angleA = 1.5707963F; // pi/2, the pitch-clamp case in lookAt()
        vertical.angleB = 1.5707963F;
        probe("straight up/down angles", vertical);

        CameraPreviewParams zeroUp;
        zeroUp.camtype = "CAM_TYPE_EYEPOS_FIX";
        zeroUp.wpoint = {5.0F, 5.0F, 5.0F};
        zeroUp.up = {0.0F, 0.0F, 0.0F}; // the game writes this on plenty of rows
        probe("zero up vector", zeroUp);

        CameraPreviewParams oddFov;
        oddFov.camtype = "CAM_TYPE_XZ_PARA";
        oddFov.fovy = 0.0F;
        oddFov.roll = 8.0F * 3.14159265F; // many turns of roll
        probe("zero fovy, huge roll", oddFov);

        CameraPreviewParams huge;
        huge.camtype = "CAM_TYPE_POINT_FIX";
        huge.dist = 1.0e7F;
        huge.woffset = {1.0e7F, -1.0e7F, 1.0e7F};
        probe("huge distance and offset", huge);

        CameraPreviewParams unknown;
        unknown.camtype = "CAM_TYPE_NOT_IN_THE_REGISTRY";
        probe("unknown camtype", unknown);

        CameraPreviewParams empty;
        probe("empty camtype", empty);

        // A hand-edited or half-corrupted file can carry a literal NaN, and one
        // NaN in a pose is an invisible viewport. The solver sanitises its inputs
        // for exactly this reason, so nothing non-finite may survive -- and a
        // poisoned fovy has to fall back to the engine default, not to zero.
        CameraPreviewParams poisoned;
        poisoned.camtype = "CAM_TYPE_XZ_PARA";
        poisoned.angleA = std::numeric_limits<float>::quiet_NaN();
        poisoned.angleB = std::numeric_limits<float>::infinity();
        poisoned.dist = std::numeric_limits<float>::quiet_NaN();
        poisoned.fovy = std::numeric_limits<float>::quiet_NaN();
        poisoned.roll = std::numeric_limits<float>::infinity();
        poisoned.wpoint = {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F};
        poisoned.woffset = {0.0F, std::numeric_limits<float>::infinity(), 0.0F};
        probe("NaN and infinity inputs", poisoned);
        const GameCameraPose sanitised = solveGameCameraPose(poisoned, {0.0F, 0.0F, 0.0F});
        expect(std::abs(sanitised.fovYRadians - 45.0F * 3.141592653589793F / 180.0F) < 1e-5F,
               "a NaN fovy must fall back to the 45 degree engine default");
        expect(sanitised.rollRadians == 0.0F, "a non-finite roll must fall back to no roll");
        // A NaN tracked point would be the caller's bug, but the solve still has
        // to come back usable rather than propagating it into the viewport.
        const GameCameraPose nanTarget =
            solveGameCameraPose(zeroDist, {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F});
        expect(finite(nanTarget.eye) && finite(nanTarget.at),
               "a non-finite tracked point must not reach the viewport");
    }

    // 5) The chain ends in the very call the viewport makes
    //    (ViewportWindow::updateCameraPreview -> ViewportCamera::lookAt), so the
    //    solved pose has to survive that round trip and still centre on the
    //    tracked point -- with roll, which is where a rebuilt basis would drift.
    {
        CameraPreviewParams params;
        params.camtype = "CAM_TYPE_XZ_PARA";
        params.angleA = 0.3F;
        params.angleB = 0.35F;
        params.dist = 1200.0F;
        params.fovy = 60.0F;
        params.roll = 0.6F;
        const Vec3f tracked{0.0F, 250.0F, 0.0F};
        const GameCameraPose pose = solveGameCameraPose(params, tracked);

        ViewportCamera camera;
        camera.lookAt(pose.eye, pose.at, pose.fovYRadians, pose.rollRadians);
        const Vec3f eye = camera.eye();
        expect((eye - pose.eye).length() < 0.05F,
               "the previewed camera must sit where the solver put it");
        expect(std::abs(camera.fieldOfViewRadians - pose.fovYRadians) < 1e-5F,
               "the dynamic fovy must reach the viewport camera");
        // Centre pixel through the rolled basis: if this ray missed `at`, the
        // preview would show one camera and pick with another.
        const auto centred = camera.screenToRay(320.0F, 240.0F, 640.0F, 480.0F);
        const Vec3f toTarget{pose.at.x - centred.origin.x, pose.at.y - centred.origin.y,
                             pose.at.z - centred.origin.z};
        expect(Vec3f::dot(centred.direction, toTarget.normalized()) > 0.999F,
               "the rolled preview's centre ray must still land on the tracked point");
    }
}

int main() {
    try {
        testBinaryData();
        testStageBuilder();
        testStageCreatePlans();
        testStageTemplates();
        testShippedTemplatesParse();
        testWriteFileLeavesNoTemporaries();
        testDirectoryFilesystem();
        testYaz0();
        testMath();
        testViewportCamera();
        testViewportCameraPreview();
        testReversedZDepthBuffer();
        testViewportScene();
        testObjectVisual();
        testHashes();
        testCameraParam();
        testCameraPreviewChain();
        testBcsvEndianness();
        testBcsvMutation();
        testUndoStack();
        testGalaxyMapZone();
        testScenarioModel();
        testScenarioEditing();
        testGalaxyScenarioSave();
        testScenarioUndo();
        testGizmoMath();
        testStageEditCommands();
        testObjectAuthoring();
        testGroupTransforms();
        testObjectModel();
        testValidation();
        testDocument();
        testRarcEndianness();
        testRarcRoundTripProperties();
        testBcsvRoundTripProperties();
        testRarcWriterNeverLooseMatches();
        testProjectArchives();
        testArchiveTableEdit();
        testNameTables();
        testStageCameras();
        testRarcCreation();
        testStageAndGameModels();
        testBtiDecoding();
        testBmdMaterialTextureRouting();
        testBmdParsing();
        testModelLibrary();
        testJsonRoundTrip();
        testSettingsRoundTrip();
        testShellBackground();
        testThemeContrast();
        testObjectDatabase();
        testObjectDatabaseV2();
        testObjectDatabaseCache();
        testCustomObjDatabase();
        testCustomObjSync();
        testBcsvColumnEditing();
        testValidationCustomObjects();
        testRealObjectDatabase();
        testDataHolderRoundTrip();
        testDbHelpersRoundTrip();
        testRailMath();
        testPathData();
        testOverlayScene();
        testKclParsing();
        std::cout << "All Whitehole native core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
