#pragma once

#include "whitehole/io/directory_filesystem.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

class GalaxyArchive {
public:
    GalaxyArchive(io::DirectoryFilesystem& filesystem, std::string name, int gameType);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    // The zones this galaxy's ZoneList.bcsv names. This stays ZoneList-only on
    // purpose: area-limit validation counts objects per SCENARIO, so adding the
    // galaxy's own map zone here would quietly change those numbers.
    [[nodiscard]] const std::vector<std::string>& zones() const noexcept { return zones_; }
    // The three tables of <Galaxy>Scenario.arc, kept as tables so the scenario
    // editor can edit them. ZoneList used to be collapsed into zones() and thrown
    // away, which left nothing to save; every one of these is still the game's
    // own bytes, untouched, until an editor actually writes to it.
    [[nodiscard]] const BcsvTable& zoneList() const noexcept { return zoneList_; }
    [[nodiscard]] const BcsvTable& scenarioData() const noexcept { return scenarioData_; }
    [[nodiscard]] const BcsvTable& galaxyInfo() const noexcept { return galaxyInfo_; }
    [[nodiscard]] BcsvTable& scenarioData() noexcept { return scenarioData_; }
    [[nodiscard]] BcsvTable& zoneList() noexcept { return zoneList_; }
    // Endianness the tables were read with, so an undo snapshot can be re-parsed
    // exactly as the archive stores it (retail SMG1/SMG2 are big-endian).
    [[nodiscard]] io::Endian endian() const noexcept { return endian_; }
    // Replaces a table from an undo snapshot. zoneList() also rebuilds zones_,
    // because that name list is a cache of the table and leaving it stale would
    // make the Project panel disagree with what a save would write.
    void setScenarioData(BcsvTable table);
    void setZoneList(BcsvTable table);
    // True when this galaxy has its own map archive (<galaxy>Map.arc / .arc).
    // Every SMG galaxy ships one: it is the galaxy map itself, a real zone with
    // its own CameraParam.bcam, and it is NOT listed in ZoneList.bcsv.
    [[nodiscard]] bool hasMapZone() const;
    // What the editor offers to open: the galaxy's own map zone FIRST, then the
    // ZoneList zones. A galaxy map is the entry point, so it belongs on top.
    [[nodiscard]] std::vector<std::string> editableZones() const;
    // True when any of the three tables differs from what was read, so the GUI
    // can show the galaxy as unsaved without guessing. Compares the tables as the
    // editor holds them, not the file, because that is what a save would write.
    [[nodiscard]] bool dirty() const;
    // Writes the scenario archive back into the workspace, recompressing it the
    // way it arrived. A galaxy that was never edited is not rewritten at all:
    // an untouched table serialises to the same bytes, and leaving the file
    // alone is the only way to be certain of that.
    void save();
    // Opens a zone from editableZones(). Accepts the galaxy's own name as well
    // as every ZoneList name, so the invariant "openable == listed" holds.
    [[nodiscard]] StageArchive openZone(std::string_view zoneName) const;

private:
    // Rebuilds zones_ from zoneList_, and the "as read" byte snapshots that
    // dirty() compares against.
    void snapshotOriginals();
    void rebuildZones();
    // The path of the scenario archive inside the workspace, and whether it was
    // Yaz0 compressed on the way in (so save() can put it back the same way).
    [[nodiscard]] std::string scenarioPath() const;

    io::DirectoryFilesystem* filesystem_;
    std::string name_;
    int gameType_{0};
    std::vector<std::string> zones_;
    BcsvTable zoneList_;
    BcsvTable scenarioData_;
    BcsvTable galaxyInfo_;
    // The bytes each table serialised to when it was read. dirty() re-serialises
    // and compares; save() never needs these.
    std::vector<std::uint8_t> zoneListOriginal_;
    std::vector<std::uint8_t> scenarioDataOriginal_;
    std::vector<std::uint8_t> galaxyInfoOriginal_;
    io::Endian endian_{io::Endian::big};
    bool wasCompressed_{true};
    // Which of the three tables the archive actually carried. A galaxy that
    // ships no GalaxyInfo.bcsv must not gain one just because we saved.
    bool hasGalaxyInfo_{false};
};

} // namespace whitehole::smg
