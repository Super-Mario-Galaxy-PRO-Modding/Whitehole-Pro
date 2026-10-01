#include "whitehole/smg/galaxy_archive.hpp"

#include "whitehole/io/rarc.hpp"

#include <algorithm>
#include <stdexcept>

namespace whitehole::smg {

GalaxyArchive::GalaxyArchive(io::DirectoryFilesystem& filesystem, std::string name, int gameType)
    : filesystem_(&filesystem), name_(std::move(name)), gameType_(gameType) {
    const auto scenarioPath = "/StageData/" + name_ + "/" + name_ + "Scenario.arc";
    if (!filesystem_->fileExists(scenarioPath)) {
        throw std::runtime_error("Galaxy scenario archive is missing: " + scenarioPath);
    }
    const auto archive = io::RarcArchive(filesystem_->read(scenarioPath));
    const auto zoneTable = BcsvTable(archive.read("ZoneList.bcsv"), archive.endian());
    for (const auto& row : zoneTable.rows()) {
        zones_.push_back(zoneTable.getString(row, "ZoneName"));
    }
    if (archive.fileExists("ScenarioData.bcsv")) {
        scenarioData_ = BcsvTable(archive.read("ScenarioData.bcsv"), archive.endian());
    }
}

bool GalaxyArchive::hasMapZone() const {
    return filesystem_->fileExists(stageMapFilesystemPath(name_, gameType_));
}

std::vector<std::string> GalaxyArchive::editableZones() const {
    // A ZoneList is not supposed to name the galaxy itself, but a hand-made or
    // template one can (the bundled SMG2 big-galaxy template does exactly that).
    // Prepending unconditionally would then list the same file twice, so the map
    // zone is only added when the ZoneList does not already carry it.
    const bool zoneListHasGalaxy =
        std::find(zones_.begin(), zones_.end(), name_) != zones_.end();
    std::vector<std::string> list;
    if (hasMapZone() && !zoneListHasGalaxy) {
        // The galaxy map is not in ZoneList.bcsv, so without this the editor
        // could never reach the galaxy map zone's objects or CameraParam.bcam.
        list.push_back(name_);
    }
    list.insert(list.end(), zones_.begin(), zones_.end());
    return list;
}

StageArchive GalaxyArchive::openZone(std::string_view zoneName) const {
    const bool isGalaxyMap = zoneName == name_;
    if (!isGalaxyMap && std::find(zones_.begin(), zones_.end(), zoneName) == zones_.end()) {
        throw std::runtime_error("Zone is not part of this galaxy: " + std::string(zoneName));
    }
    return StageArchive::open(*filesystem_, zoneName, gameType_);
}

} // namespace whitehole::smg
