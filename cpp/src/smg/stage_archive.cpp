#include "whitehole/smg/stage_archive.hpp"

#include "whitehole/io/binary_file.hpp"
#include "whitehole/smg/hash.hpp"
#include "whitehole/smg/path.hpp"
#include "whitehole/util/text.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace whitehole::smg {
namespace {

struct LayeredTable {
    const char* folder;
    const char* file;
    const char* kind;
    int gameType; // 0 both, 1 SMG1, 2 SMG2
};

constexpr std::array<LayeredTable, 12> kLayeredTables{{
    {"Placement", "StageObjInfo", "stage", 0},
    {"Placement", "AreaObjInfo", "area", 0},
    {"Placement", "ObjInfo", "obj", 0},
    {"Placement", "CameraCubeInfo", "camera", 0},
    {"Placement", "PlanetObjInfo", "gravity", 0},
    {"Placement", "DemoObjInfo", "cutscene", 0},
    {"MapParts", "MapPartsInfo", "mappart", 0},
    {"Start", "StartInfo", "start", 0},
    {"GeneralPos", "GeneralPosInfo", "position", 0},
    {"Debug", "DebugMoveInfo", "debug", 0},
    {"Placement", "SoundInfo", "sound", 1},
    {"ChildObj", "ChildObjInfo", "child", 1},
}};

std::string mapFilesystemPath(std::string_view stageName, int gameType) {
    if (gameType == 1) {
        return "/StageData/" + std::string(stageName) + ".arc";
    }
    return "/StageData/" + std::string(stageName) + "/" + std::string(stageName) + "Map.arc";
}

} // namespace

void StageArchive::loadTable(std::string_view path, std::string kind, std::string layer) {
    if (!archive_ || !archive_->fileExists(path)) {
        return;
    }
    // One table per file: two path rows sharing a `no` must not register the
    // same points file twice (the second registration would clobber the first
    // on save).
    for (const auto& existing : tables_) {
        if (util::equalIgnoreCase(existing.path, path)) {
            return;
        }
    }
    try {
        ObjectTable table;
        table.path = std::string(path);
        table.kind = std::move(kind);
        table.layer = std::move(layer);
        table.table = BcsvTable(archive_->read(path), archive_->endian());
        tables_.push_back(std::move(table));
    } catch (const std::exception&) {
        // Empty or non-JMap placeholders are skipped so a zone can still open.
    }
}

PlacementObject StageArchive::readObject(std::size_t tableIndex, std::size_t rowIndex) const {
    if (tableIndex >= tables_.size()) {
        throw std::out_of_range("Stage table index is out of range");
    }
    const auto& table = tables_[tableIndex];
    if (rowIndex >= table.table.rows().size()) {
        throw std::out_of_range("Stage table row index is out of range");
    }
    PlacementObject object;
    object.tableIndex = tableIndex;
    object.rowIndex = rowIndex;
    object.kind = table.kind;
    object.layer = table.layer;
    const auto& entry = table.table.rows()[rowIndex];
    object.name = table.table.getStringById(entry, jmapHash("name"));
    object.position = {table.table.getFloatById(entry, jmapHash("pos_x")),
                       table.table.getFloatById(entry, jmapHash("pos_y")),
                       table.table.getFloatById(entry, jmapHash("pos_z"))};
    object.rotation = {table.table.getFloatById(entry, jmapHash("dir_x")),
                       table.table.getFloatById(entry, jmapHash("dir_y")),
                       table.table.getFloatById(entry, jmapHash("dir_z"))};
    object.scale = {table.table.getFloatById(entry, jmapHash("scale_x"), 1.0F),
                    table.table.getFloatById(entry, jmapHash("scale_y"), 1.0F),
                    table.table.getFloatById(entry, jmapHash("scale_z"), 1.0F)};
    return object;
}

void StageArchive::rebuildObjects() {
    // Objects are a projection of the tables: table `t` (in insertion order)
    // contributes its rows in order, which is exactly how they were loaded.
    // Rails live in their own list (loadPaths) exactly like Java, so the two
    // path kinds never surface as placeable objects.
    objects_.clear();
    for (std::size_t tableIndex = 0; tableIndex < tables_.size(); ++tableIndex) {
        const auto& table = tables_[tableIndex];
        if (table.kind == "path" || table.kind == "pathpoint") {
            continue;
        }
        const auto rowCount = table.table.rows().size();
        objects_.reserve(objects_.size() + rowCount);
        for (std::size_t row = 0; row < rowCount; ++row) {
            objects_.push_back(readObject(tableIndex, row));
        }
    }
}

void StageArchive::writeObject(const PlacementObject& object) {
    if (object.tableIndex >= tables_.size()) {
        return;
    }
    auto& table = tables_[object.tableIndex];
    if (object.rowIndex >= table.table.rows().size()) {
        return;
    }
    auto& row = table.table.rows()[object.rowIndex];
    table.table.setString(row, "name", object.name);
    table.table.setFloat(row, "pos_x", object.position.x);
    table.table.setFloat(row, "pos_y", object.position.y);
    table.table.setFloat(row, "pos_z", object.position.z);
    table.table.setFloat(row, "dir_x", object.rotation.x);
    table.table.setFloat(row, "dir_y", object.rotation.y);
    table.table.setFloat(row, "dir_z", object.rotation.z);
    table.table.setFloat(row, "scale_x", object.scale.x);
    table.table.setFloat(row, "scale_y", object.scale.y);
    table.table.setFloat(row, "scale_z", object.scale.z);
}

void StageArchive::loadFromArchive() {
    tables_.clear();
    objects_.clear();
    if (!archive_) {
        return;
    }
    for (const auto& spec : kLayeredTables) {
        if (spec.gameType != 0 && spec.gameType != gameType_) {
            continue;
        }
        const auto folder = std::string("/Stage/jmp/") + spec.folder;
        for (const auto& layer : archive_->directories(folder)) {
            const auto path = folder + "/" + layer + "/" + spec.file;
            loadTable(path, spec.kind, layer);
        }
    }
    loadTable("/Stage/jmp/Path/CommonPathInfo", "path", "Common");
    // Point tables are addressed by each path row's `no` field. Collect every
    // reference first, then load: loadTable pushes to tables_ (which can
    // reallocate), so no table reference may be live across those calls.
    // loadTable also skips paths already present, so two rows sharing one
    // file share one table.
    std::vector<std::int32_t> pointFiles;
    for (std::size_t tableIndex = 0; tableIndex < tables_.size(); ++tableIndex) {
        const auto& table = tables_[tableIndex];
        if (table.kind != "path") {
            continue;
        }
        for (const auto& row : table.table.rows()) {
            const auto no = table.table.getInt(row, "no", -1);
            if (no >= 0) {
                pointFiles.push_back(no);
            }
        }
    }
    for (const auto no : pointFiles) {
        loadTable(pathPointFile(no), "pathpoint", "Common");
    }
    rebuildObjects();
}

StageArchive StageArchive::openMapFile(const std::filesystem::path& path, int gameType) {
    StageArchive stage;
    stage.gameType_ = gameType;
    stage.stageName_ = path.stem().string();
    stage.sourcePath_ = path;
    stage.archive_ = io::RarcArchive::open(path);
    stage.loadFromArchive();
    return stage;
}

StageArchive StageArchive::open(io::DirectoryFilesystem& filesystem, std::string_view stageName, int gameType) {
    StageArchive stage;
    stage.filesystem_ = &filesystem;
    stage.gameType_ = gameType;
    stage.stageName_ = std::string(stageName);
    stage.filesystemPath_ = mapFilesystemPath(stageName, gameType);
    if (!filesystem.fileExists(stage.filesystemPath_)) {
        throw std::runtime_error("Stage map archive is missing: " + stage.filesystemPath_);
    }
    stage.sourcePath_ = filesystem.root() / stage.filesystemPath_.substr(1);
    stage.archive_ = io::RarcArchive(filesystem.read(stage.filesystemPath_));
    stage.loadFromArchive();
    return stage;
}

void StageArchive::applyEdits() {
    for (const auto& object : objects_) {
        writeObject(object);
    }
    // Every path row's num_pnt mirrors its point table right before saving, so
    // a stale count can never ship (Java did the same in PathObj.save()).
    for (std::size_t tableIndex = 0; tableIndex < tables_.size(); ++tableIndex) {
        auto& table = tables_[tableIndex];
        if (table.kind != "path" || !table.table.hasField("num_pnt")) {
            continue;
        }
        for (auto& row : table.table.rows()) {
            const auto no = table.table.getInt(row, "no", -1);
            std::int32_t count = 0;
            if (no >= 0) {
                const auto pointIndex = pathPointTableIndex(*this, no);
                if (pointIndex != kNoPointTable) {
                    count = static_cast<std::int32_t>(tables_[pointIndex].table.rows().size());
                }
            }
            table.table.setInt(row, "num_pnt", count);
        }
    }
    // Java's PathPointObj.save() wrote each point's id to its index within the
    // path's id-sorted point list, so the file that lands on disk is sorted by
    // id first (pairing intact) and only then normalized to 0..n-1. Renumbering
    // in raw row order without the sort would silently re-pair ids with the
    // wrong positions whenever a hand-made file was not already sorted, which
    // changes the rail's traversal order in game.
    for (auto& table : tables_) {
        if (table.kind != "pathpoint" || !table.table.hasField("id")) {
            continue;
        }
        auto& rows = table.table.rows();
        std::stable_sort(rows.begin(), rows.end(),
                         [&table](const BcsvRow& left, const BcsvRow& right) {
                             return table.table.getInt(left, "id", 0) <
                                    table.table.getInt(right, "id", 0);
                         });
        for (std::size_t index = 0; index < rows.size(); ++index) {
            table.table.setInt(rows[index], "id", static_cast<std::int32_t>(index));
        }
    }
}

void StageArchive::saveTo(const std::filesystem::path& path) {
    applyEdits();
    if (!archive_) {
        throw std::runtime_error("No map archive is loaded");
    }
    for (const auto& table : tables_) {
        auto bytes = table.table.serialize();
        // insert() writes brand-new files (a path created since the archive
        // was opened) and falls back to replace() for the ones already there.
        archive_->insert(table.path, std::move(bytes));
    }
    io::writeFile(path, archive_->serialize(archive_->wasCompressed()));
    sourcePath_ = path;
}

void StageArchive::save() {
    if (filesystem_ != nullptr && !filesystemPath_.empty()) {
        applyEdits();
        if (!archive_) {
            throw std::runtime_error("No map archive is loaded");
        }
        for (const auto& table : tables_) {
            auto bytes = table.table.serialize();
            archive_->insert(table.path, std::move(bytes));
        }
        filesystem_->write(filesystemPath_, archive_->serialize(archive_->wasCompressed()));
        return;
    }
    if (sourcePath_.empty()) {
        throw std::runtime_error("No destination is available for this stage");
    }
    saveTo(sourcePath_);
}

} // namespace whitehole::smg
