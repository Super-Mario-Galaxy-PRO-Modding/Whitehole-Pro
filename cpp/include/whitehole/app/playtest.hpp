#pragma once

// "Test in Dolphin": hand a workspace edit to the game, without the modder
// having to know what a Riivolution patch is.
//
// WHY THIS EXISTS. Everything else in the editor stops at "the file on disk is
// correct". A modder who has just moved a platform still cannot see it, because
// an extracted game folder is not something any emulator can boot: it has to be
// layered back over the disc. The community's answer is a Riivolution patch XML,
// and writing one by hand is a wall of unfamiliar XML where every mistake simply
// does nothing -- no error, no message, the game just boots unpatched. So the
// editor writes it, and writes it in the one shape Dolphin actually looks for.
//
// THE FORMAT IS NOT GUESSED. Every rule below was read out of the emulator that
// consumes it (Dolphin's Source/Core/DiscIO/RiivolutionParser.cpp and
// RiivolutionPatcher.cpp) and out of the Riivolution patch-format wiki:
//
//   * Dolphin scans `<root>/riivolution/**/*.xml`, where `root` is the folder
//     configured in Dolphin's Riivolution dialog -- `<Dolphin>/Load/Riivolution`
//     for a normal install. So the XML belongs in `Load/Riivolution/riivolution/`.
//   * An `external` path that STARTS WITH A SLASH resolves against that same
//     root; anything else resolves against the folder the XML itself is in.
//     The patch below therefore writes `external="/<name>/files"`, which is
//     root-relative and therefore unambiguous, whatever Dolphin's "Root" field
//     is set to as long as it points at `Load/Riivolution`.
//   * `<wiidisc version="1">` is mandatory: the parser rejects any other value
//     (RiivolutionParser.cpp: `if (disc.m_version != 1) return std::nullopt;`),
//     which is exactly the silent failure this module exists to prevent.
//   * A `<folder>` patch replaces every file that exists in the external folder
//     and, with `create="true"`, adds files the disc does not have. That second
//     half is what makes a zone created from scratch in Whitehole reachable at
//     all -- the disc has no such file to replace.
//
// THE DISC ID IS STATED, NOT DETECTED. Riivolution matches a patch to the disc
// by its 6-character id (SB4E01...), and an extracted game folder has no disc
// header left to read it from, so the author picks their game and region. A
// wrong id is another silent no-op, so the picker offers the real ids and the
// CLI takes one openly.
//
// HONEST SCOPE: like stage_builder, this is verified against Whitehole Pro's own
// readers and Dolphin's parser source. No patch produced here has been run
// inside Dolphin from this machine.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::app {

// The workspace IS the disc's `/files` tree: GameArchive probes "/StageData" at
// the workspace root, and that folder is `files/` on the disc. Exposed because
// the disc path of every exported file is built from it.
inline constexpr std::string_view kWorkspaceDiscRoot = "/files";

// The mod folder name a fresh export uses. A plain, recognisable default; the
// author can rename it, and every path in the XML follows the name.
inline constexpr std::string_view kDefaultPlaytestName = "WhiteholePro";

// The four retail releases of each game, as Riivolution spells them. Ordered
// USA, Europe, Japan, Korea -- the order the pickers show.
[[nodiscard]] std::vector<std::string> knownDiscIds(int gameType);
// "SB4E01" for gameType 2 + 'E'. Throws std::runtime_error for a game/region
// pair that has no retail disc, rather than returning something that would
// silently never match.
[[nodiscard]] std::string discIdFor(int gameType, char region);
// "USA (NTSC-U)", for a combo label. Empty for an unknown region letter.
[[nodiscard]] std::string regionLabel(char region);
// The region letter of a known id ("SB4E01" -> 'E'), or '\0'. Lets a caller
// round-trip an id typed by hand.
[[nodiscard]] char regionOfDiscId(std::string_view discId) noexcept;

// One file to send to the disc: where it comes from on the host, and where it
// goes inside the game's own filesystem.
struct PlaytestFile {
    std::filesystem::path source;  // absolute host path of the file to copy
    std::string relativePath;      // workspace-relative, e.g. "/StageData/A/A.arc"
    std::string discPath;          // kWorkspaceDiscRoot + relativePath
    std::string targetPath;        // "<name>/files" + relativePath, XML-relative
};

// Everything an export would write, computed WITHOUT writing any of it.
//
// The same plan/apply split the create dialog uses (BLUEPRINT section 17): a dry
// run has to name every file before touching the disk, and the dialog has to
// show the author what is about to happen. A bool flag on apply() could not do
// either, and a second implementation of the path rules would drift.
struct PlaytestPlan {
    std::filesystem::path sdRoot;    // <Dolphin>/Load/Riivolution
    std::filesystem::path xmlPath;   // <sdRoot>/riivolution/<name>.xml
    std::filesystem::path modFolder; // <sdRoot>/<name>, holding files/
    std::string name;
    std::string gameId;
    std::vector<PlaytestFile> files;

    // The copied files first, the XML last, so a failure mid-copy never leaves a
    // patch pointing at files that are not there yet.
    [[nodiscard]] std::vector<std::string> filesWritten() const;
};

// Plans an export. `relativePaths` are workspace-relative (a leading slash is
// fine) and may be given as absolute host paths inside the workspace instead.
//
// Throws std::runtime_error, naming what is wrong, for: an empty file list, a
// name that is not usable as a folder name, a file that is not inside the
// workspace, or a workspace-relative path with no file behind it. Refusing here
// is the point -- a plan that quietly skipped a missing file would export a
// patch that half-applies.
[[nodiscard]] PlaytestPlan planPlaytest(const std::filesystem::path& sdRoot, std::string_view name,
                                        std::string_view gameId,
                                        const std::filesystem::path& workspaceRoot,
                                        const std::vector<std::string>& relativePaths,
                                        std::string_view discRoot = kWorkspaceDiscRoot);

// The Riivolution XML for a plan. Pure, so a test can pin every attribute that
// Dolphin's parser depends on without touching a disk.
[[nodiscard]] std::string riivolutionXml(const PlaytestPlan& plan);

// Writes the plan: the copied files first, the XML last.
//
// Removes files left in the plan's own `files` tree by an earlier export of
// different files, because a folder patch applies EVERY file it finds there -- a
// stale zone from last week's export would keep coming back with no explanation.
// Only files under the plan's own mod folder are ever removed.
void applyPlaytest(const PlaytestPlan& plan);

// Where Dolphin keeps Riivolution patches for a normal (non-portable) install:
// %APPDATA%/Dolphin Emulator/Load/Riivolution. Empty when APPDATA is unset.
[[nodiscard]] std::filesystem::path defaultDolphinSdRoot();
// The first of the usual locations that exists, else defaultDolphinSdRoot() so
// the dialog can still say where the export WOULD go.
[[nodiscard]] std::filesystem::path detectDolphinSdRoot();

// The exact clicks that turn a written patch into a running game, for the dialog
// and the CLI to print. Kept in one place so the two cannot disagree.
[[nodiscard]] std::string dolphinLaunchSteps();

} // namespace whitehole::app
