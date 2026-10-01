#include "whitehole/smg/galaxy_archive.hpp"

#include "whitehole/io/rarc.hpp"

#include <algorithm>
#include <stdexcept>

namespace whitehole::smg {

GalaxyArchive::GalaxyArchive(io::DirectoryFilesystem& filesystem, std::string name, int gameType)
    : filesystem_(&filesystem), name_(std::move(name)), gameType_(gameType) {
    if (!filesystem_->fileExists(scenarioPath())) {
        throw std::runtime_error("Galaxy scenario archive is missing: " + scenarioPath());
    }
    io::RarcArchive archive(filesystem_->read(scenarioPath()));
    endian_ = archive.endian();
    wasCompressed_ = archive.wasCompressed();

    // All three tables are optional in principle, but a galaxy without
    // ZoneList/ScenarioData is not a galaxy the editor can do anything with, so a
    // malformed one is a hard error rather than a silently empty editor.
    zoneList_ = BcsvTable(archive.read("ZoneList.bcsv"), archive.endian());
    for (const auto& row : zoneList_.rows()) {
        zones_.push_back(zoneList_.getString(row, "ZoneName"));
    }
    if (archive.fileExists("ScenarioData.bcsv")) {
        scenarioData_ = BcsvTable(archive.read("ScenarioData.bcsv"), archive.endian());
    }
    hasGalaxyInfo_ = archive.fileExists("GalaxyInfo.bcsv");
    if (hasGalaxyInfo_) {
        galaxyInfo_ = BcsvTable(archive.read("GalaxyInfo.bcsv"), archive.endian());
    }
    snapshotOriginals();
}

std::string GalaxyArchive::scenarioPath() const {
    return "/StageData/" + name_ + "/" + name_ + "Scenario.arc";
}

void GalaxyArchive::snapshotOriginals() {
    zoneListOriginal_ = zoneList_.serialize();
    scenarioDataOriginal_ = scenarioData_.serialize();
    galaxyInfoOriginal_ = galaxyInfo_.serialize();
}

bool GalaxyArchive::dirty() const {
    return zoneList_.serialize() != zoneListOriginal_ ||
           scenarioData_.serialize() != scenarioDataOriginal_ ||
           galaxyInfo_.serialize() != galaxyInfoOriginal_;
}

void GalaxyArchive::save() {
    if (filesystem_ == nullptr) {
        throw std::runtime_error("This galaxy has no workspace to save into");
    }
    // An untouched galaxy is not rewritten. Serialising a BCSV can append
    // alignment padding, so "we did not change it" and "the bytes are identical"
    // are not the same claim -- leaving the file alone is the only way to be
    // certain the game still reads exactly what it read before.
    if (!dirty()) {
        return;
    }
    io::RarcArchive archive(filesystem_->read(scenarioPath()));
    // insert() adds a new file or replaces an existing one, keeping the stored
    // casing for a new entry so it reads like its siblings.
    archive.insert("ScenarioData.bcsv", scenarioData_.serialize());
    if (!zoneList_.rows().empty() || zoneList_.hasField("ZoneName")) {
        archive.insert("ZoneList.bcsv", zoneList_.serialize());
    }
    // Never create GalaxyInfo.bcsv: only a galaxy that shipped one gets it back.
    if (hasGalaxyInfo_) {
        archive.insert("GalaxyInfo.bcsv", galaxyInfo_.serialize());
    }
    filesystem_->write(scenarioPath(), archive.serialize(wasCompressed_));
    snapshotOriginals();
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
