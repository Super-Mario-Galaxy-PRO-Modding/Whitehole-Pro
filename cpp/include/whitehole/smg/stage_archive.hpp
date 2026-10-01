#pragma once

#include "whitehole/io/directory_filesystem.hpp"
#include "whitehole/io/rarc.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/camera_param.hpp"
#include "whitehole/smg/placement.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace whitehole::smg {

struct ObjectTable {
    std::string path;
    std::string kind;
    std::string layer;
    BcsvTable table;
};

// One of the layered JMap files a stage is built from (Java's
// STAGE_MAP_JMP_FILES). Shared with the loader and with the zone/galaxy builder
// so the set of files a zone is EXPECTED to have cannot drift from the set the
// editor reads -- a builder that created a different list would make zones the
// loader silently ignores.
struct LayeredTableSpec {
    std::string_view folder;
    std::string_view file;
    std::string_view kind;
    // 0 = both games, 1 = SMG1 only, 2 = SMG2 only. SoundInfo and ChildObjInfo
    // are SMG1-only, and creating them for SMG2 would add files the game does not
    // expect to find.
    int gameType;
};

// Every layered table, in the same order the loader walks them.
[[nodiscard]] const std::vector<LayeredTableSpec>& stageLayeredTables();

// Workspace-relative path of a stage's map archive. SMG2 keeps each zone in
// its own folder (/StageData/<stage>/<stage>Map.arc); SMG1 flattens them into
// /StageData/<stage>.arc. Shared rather than duplicated so GalaxyArchive's
// "does this galaxy have a map zone" check and StageArchive::open() can never
// disagree about where a zone actually lives.
[[nodiscard]] std::string stageMapFilesystemPath(std::string_view stageName, int gameType);

class StageArchive {
public:
    [[nodiscard]] static StageArchive openMapFile(const std::filesystem::path& path, int gameType = 2);
    [[nodiscard]] static StageArchive open(io::DirectoryFilesystem& filesystem, std::string_view stageName,
                                           int gameType);

    [[nodiscard]] const std::string& stageName() const noexcept { return stageName_; }
    [[nodiscard]] const std::filesystem::path& sourcePath() const noexcept { return sourcePath_; }
    [[nodiscard]] const std::vector<ObjectTable>& tables() const noexcept { return tables_; }
    [[nodiscard]] std::vector<ObjectTable>& tables() noexcept { return tables_; }
    [[nodiscard]] const std::vector<PlacementObject>& objects() const noexcept { return objects_; }
    [[nodiscard]] std::vector<PlacementObject>& objects() noexcept { return objects_; }

    [[nodiscard]] int gameType() const noexcept { return gameType_; }

    // The zone's camera table (/Stage/camera/CameraParam.bcam). Always valid:
    // an archive without the file yields an empty table the editor can fill.
    [[nodiscard]] const CameraParamTable& cameraParams() const noexcept { return cameraParams_; }
    [[nodiscard]] CameraParamTable& cameraParams() noexcept { return cameraParams_; }
    // Replaces the camera table; undo restores byte snapshots through this.
    void setCameraParams(CameraParamTable table) noexcept { cameraParams_ = std::move(table); }
    // Endianness of the loaded archive (retail SMG1/SMG2 are big-endian), so a
    // camera snapshot can be re-parsed exactly as the archive stores it.
    [[nodiscard]] io::Endian endian() const noexcept {
        return archive_.has_value() ? archive_->endian() : io::Endian::big;
    }
    // Engine version the editor stamps on new camera rows for this game.
    [[nodiscard]] std::uint32_t cameraDefaultVersion() const noexcept {
        return cameraVersionForGame(gameType_);
    }

    // Re-derives the placement list from the current tables. Undo/redo mutates
    // table rows directly, so call this afterwards to keep objects() in step.
    void rebuildObjects();
    // Writes one placement object's name and transform into its table row.
    // Out-of-range indices are ignored.
    void writeObject(const PlacementObject& object);
    // Reads the placement object at a table row. Throws std::out_of_range when
    // the table or row does not exist.
    [[nodiscard]] PlacementObject readObject(std::size_t tableIndex, std::size_t rowIndex) const;

    void applyEdits();
    void save();
    void saveTo(const std::filesystem::path& path);

private:
    void loadFromArchive();
    // string_view, not string: the table specs are string_views, and taking them by
    // value would copy every one of them for nothing.
    void loadTable(std::string_view path, std::string_view kind, std::string_view layer);
    // Writes the camera table back into the archive (no-op for an archive
    // that never carried one and still has no cameras).
    void writeCameraParams();

    io::DirectoryFilesystem* filesystem_{nullptr};
    std::string stageName_;
    std::filesystem::path sourcePath_;
    std::string filesystemPath_;
    int gameType_{2};
    std::optional<io::RarcArchive> archive_;
    std::vector<ObjectTable> tables_;
    std::vector<PlacementObject> objects_;
    CameraParamTable cameraParams_{};
    std::string cameraTablePath_; // as stored in the archive (SMG1 is lowercase)
};

} // namespace whitehole::smg
