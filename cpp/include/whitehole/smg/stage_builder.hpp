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

// What one create* call wrote, so the CLI and the GUI can report it and the
// tests can assert on it. Paths are workspace-relative, as everywhere else.
struct CreatedZone {
    std::string name;
    std::string mapPath;                 // e.g. /StageData/Foo/FooMap.arc
    std::vector<std::string> layers;     // as written, "Common" first
    std::vector<std::string> layerFiles;  // every JMap file written, for the summary
};

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

// The path a created zone's map archive lives at, and the name of the scenario
// archive for a galaxy. Exposed because the CLI reports them and the tests use
// them; the Java has these rules in two places and they must not drift.
[[nodiscard]] std::string zoneScenarioPath(std::string_view galaxyName, int gameType);
[[nodiscard]] std::string galaxyScenarioPath(std::string_view galaxyName);

} // namespace whitehole::smg