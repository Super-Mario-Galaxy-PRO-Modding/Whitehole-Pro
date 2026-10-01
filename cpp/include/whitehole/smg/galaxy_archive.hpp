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
    // True when this galaxy has its own map archive (<galaxy>Map.arc / .arc).
    // Every SMG galaxy ships one: it is the galaxy map itself, a real zone with
    // its own CameraParam.bcam, and it is NOT listed in ZoneList.bcsv.
    [[nodiscard]] bool hasMapZone() const;
    // What the editor offers to open: the galaxy's own map zone FIRST, then the
    // ZoneList zones. A galaxy map is the entry point, so it belongs on top.
    [[nodiscard]] std::vector<std::string> editableZones() const;
    [[nodiscard]] const BcsvTable& scenarioData() const noexcept { return scenarioData_; }
    // Opens a zone from editableZones(). Accepts the galaxy's own name as well
    // as every ZoneList name, so the invariant "openable == listed" holds.
    [[nodiscard]] StageArchive openZone(std::string_view zoneName) const;

private:
    io::DirectoryFilesystem* filesystem_;
    std::string name_;
    int gameType_{0};
    std::vector<std::string> zones_;
    BcsvTable scenarioData_;
};

} // namespace whitehole::smg
