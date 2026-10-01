#pragma once

// Creating zones and galaxies from nothing (Java's StageHelper.createZone /
// createGalaxy, reimplemented against the game's own on-disk layout).
//
// WHAT IS CREATED, and why it is derived rather than transcribed. Java builds
// each JMap file by hand through thirteen populateJMapFields* functions --
// ObjInfo alone is 38 fields. Every one of those field lists is also present,
// exactly right, in the template archives that ship in data/templates: a
// template IS a real game file with the right schema and (usually) no rows. So
// the SCHEMA comes from the template the caller passes in, and this file
// transcribes nothing. The scenario tables (ScenarioData / ZoneList /
// GalaxyInfo) need no template at all: ScenarioModel already owns every column
// and the SMG1/SMG2 split, so the builder asks it rather than restating it.
//
// A created zone gets the template's SCHEMA with its rows cleared -- including
// Common/StartInfo, which gets exactly one row: the spawn point at the origin,
// because a zone with no spawn point is unplayable. That is what a "bare
// minimum" zone is, and it is why a template is required even for one: without
// a real game file there is no trustworthy schema to write.
//
// HONEST SCOPE: everything here is verified against Whitehole Pro's own reader
// (StageArchive / GalaxyArchive re-open what this writes). No retail copy of
// SMG1/SMG2 has been run against it. "The game accepts this" is UNVERIFIED.

#include "whitehole/io/directory_filesystem.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// The layers a created zone can be given. "Common" is always on and owns no bit
// (scenarioLayerBit), so it is added implicitly rather than asked for.
[[nodiscard]] std::vector<std::string> allStageLayerNames();

// The layer list create* would actually build, from the caller's request: "Common"
// first and always present, duplicates collapsed, and anything that is not a real
// layer REJECTED (scenarioLayerBit < 0) rather than silently dropped.
//
// Exposed because the layer pickers -- the CLI's --layer flags and the GUI's
// dialog -- must show the truth, not the request. A checkerboard that silently
// ignores "LayerZ" is how a zone ends up missing a layer the author believed in.
[[nodiscard]] std::vector<std::string> normalizeStageLayers(const std::vector<std::string>& layers);

// What one create* call wrote, so the CLI and the GUI can report it and the
// tests can assert on it. Paths are workspace-relative, as everywhere else.
struct CreatedZone {
    std::string name;
    std::string mapPath;                 // e.g. /StageData/Foo/FooMap.arc
    std::vector<std::string> layers;     // as written, "Common" first
    std::vector<std::string> layerFiles;  // every JMap file written, for the summary
};

// Everything a create would write, computed WITHOUT writing any of it.
//
// This is the plan/apply split, and it exists for two reasons that both need the
// same thing. A `--dry-run` has to print every file before touching the disk, and
// the GUI's confirm step has to show the author what is about to happen -- the
// Scenarios panel's existing rule is "show what will happen, then happen it".
//
// It is deliberately planStageZone/planGalaxy rather than a bool flag on
// create*: a flag that suppressed the writes would still build every archive and
// every BCSV, so a "dry run" would cost exactly what the real thing costs and
// would still be able to fail on the thing it promised to be previewing.
//
// A plan is only trustworthy if it cannot drift from what create* does, so
// createStageZone/createGalaxy BUILD THEIR PLAN THROUGH THESE and then apply it.
// The two never compute the file list independently.
struct StageCreatePlan {
    std::string name;
    std::string mapPath;                    // the zone's map archive
    std::vector<std::string> layers;        // "Common" first
    std::vector<std::string> layerFiles;    // every JMap path INSIDE the map archive
    // Empty for a plain zone. A galaxy also writes these.
    std::string scenarioPath;
    std::vector<std::string> extraZones;

    [[nodiscard]] bool forGalaxy() const noexcept { return !scenarioPath.empty(); }
    // Every file the apply will put on disk, in the order it writes them. This is
    // what a dry run prints and what the GUI's confirm step lists.
    [[nodiscard]] std::vector<std::string> filesWritten() const;
};

// Validates and resolves a create into a plan. Throws std::runtime_error naming
// what went wrong (unsafe name, no layers, a name that is not a layer, a template
// that carries no schemas) -- exactly as create* does, because a plan that
// cannot be applied is not a plan.
[[nodiscard]] StageCreatePlan planStageZone(std::string_view name,
                                            const std::vector<std::string>& layers,
                                            const std::vector<std::uint8_t>& schemaTemplate,
                                            int gameType);

// The same for a galaxy, including the scenario archive's path and the zones it
// will link. It does NOT check whether the galaxy already exists: that is a
// question about the destination, and this function never looks at it -- which is
// what lets the same plan be previewed anywhere.
[[nodiscard]] StageCreatePlan planGalaxy(std::string_view name,
                                         const std::vector<std::string>& extraZones,
                                         const std::vector<std::string>& layers,
                                         const std::vector<std::uint8_t>& schemaTemplate,
                                         int gameType);

// Creates <name>'s map archive and writes it into the workspace.
//
// `schemaTemplate` is the bytes of a template MAP archive (data/templates/*.arc)
// and is REQUIRED: it is the schema source. Its own contents are not copied
// beyond the schemas, so a template is never modified and never leaks objects
// into the new zone.
//
// Refuses an existing zone, an unsafe name, an empty layer list, and a template
// that is not a map archive. Throws std::runtime_error with a message naming
// what went wrong -- this runs from a UI button and a CLI alike.
void createStageZone(io::DirectoryFilesystem& filesystem, std::string_view name,
                     const std::vector<std::string>& layers,
                     const std::vector<std::uint8_t>& schemaTemplate, int gameType,
                     CreatedZone* report = nullptr);

// Creates a galaxy: the galaxy's own map zone, the zone links in its
// Placement/Common/StageObjInfo, and <name>Scenario.arc carrying ScenarioData,
// ZoneList and (SMG2 only) GalaxyInfo.
//
// `extraZones` are the galaxy's other zones. They get a ScenarioData column and
// a StageObjInfo link, but NO map archive is created for them: making a zone's
// files is createStageZone's job, and silently inventing a map for a name the
// caller only mentioned would produce a galaxy whose zones do not open.
//
// The ZoneList gets ONLY the galaxy's own name. See the note in BLUEPRINT
// section 15 for why an extra zone is not added to it automatically.
void createGalaxy(io::DirectoryFilesystem& filesystem, std::string_view name,
                  const std::vector<std::string>& extraZones,
                  const std::vector<std::string>& layers,
                  const std::vector<std::uint8_t>& schemaTemplate, int gameType);

// Where a galaxy's scenario archive lives: /StageData/<Galaxy>/<Galaxy>Scenario.arc
//
// This is deliberately NOT game-dependent, which is the whole reason it is its
// own function. SMG1 MAP zones are flat (/StageData/<zone>.arc) while SMG2 ones
// get a folder, so a reader reasonably expects stageMapFilesystemPath()'s answer
// to differ per game -- but Java's createGalaxy writes the scenario archive in
// the SMG2 folder shape for BOTH games, lowercasing only the ROOT name for SMG1
// (StageHelper.java:293). An earlier version took a `gameType` and threw it
// away, which is a trap: it reads like the answer changes per game and it does
// not. `galaxyScenarioPath()` was then a second name for this same function.
// One function, no parameter, documented.
//
// Exposed because the CLI reports the path and the tests assert on it, and the
// Java carries these rules in two places that must not drift.
[[nodiscard]] std::string scenarioArchivePath(std::string_view galaxyName);

} // namespace whitehole::smg