#include "whitehole/edit/authoring.hpp"

#include "whitehole/edit/commands.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

namespace whitehole::edit {
namespace {

// Java's per-type constructors (LevelObj / MapPartObj / StartObj / ...) wrote
// this exact "unset" sentinel set by hand. Fields the open table does not have
// are skipped, so the list stays harmless for tables that never carry them.
constexpr std::string_view kNoneFields[] = {
    "Obj_arg0",        "Obj_arg1",      "Obj_arg2",       "Obj_arg3",
    "Obj_arg4",        "Obj_arg5",      "Obj_arg6",       "Obj_arg7",
    "SW_APPEAR",       "SW_DEAD",       "SW_A",           "SW_B",
    "SW_SLEEP",        "SW_AWAKE",      "SW_PARAM",       "GeneratorID",
    "Obj_ID",          "MapParts_ID",   "ParentId",       "GroupId",
    "ClippingGroupId", "ViewGroupId",   "DemoGroupId",    "CastId",
    "ShapeModelNo",    "CommonPath_ID", "CameraSetId",    "FarClip",
};

// Java's constructors also reset ParamScale to 1, because a zero scale would
// make the new object invisible.
constexpr std::string_view kUnitScaleFields[] = {"ParamScale"};

// Case-insensitive comparison, in one place so the mapping tables below stay
// readable.
bool sameName(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        const auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

// Database placement list <-> native table kind. The lists are the game's own
// BCSV file names, which is what objectdb.json stores in ListSMG1/ListSMG2.
struct ListMapping {
    std::string_view list;
    std::string_view kind;
};

constexpr ListMapping kListMappings[] = {
    {"StageObjInfo", "stage"},      {"AreaObjInfo", "area"},
    {"ObjInfo", "obj"},             {"CameraCubeInfo", "camera"},
    {"PlanetObjInfo", "gravity"},   {"DemoObjInfo", "cutscene"},
    {"MapPartsInfo", "mappart"},    {"StartInfo", "start"},
    {"GeneralPosInfo", "position"}, {"DebugMoveInfo", "debug"},
    {"SoundInfo", "sound"},         {"ChildObjInfo", "child"},
    {"CommonPathInfo", "path"},
};

} // namespace

std::string_view kindForList(std::string_view list) noexcept {
    for (const auto& mapping : kListMappings) {
        if (sameName(mapping.list, list)) {
            return mapping.kind;
        }
    }
    return {};
}

std::string_view listForKind(std::string_view kind) noexcept {
    for (const auto& mapping : kListMappings) {
        if (sameName(mapping.kind, kind)) {
            return mapping.list;
        }
    }
    return {};
}

std::vector<PlacementTarget> placementTargets(const smg::StageArchive& stage,
                                             std::string_view kind) {
    std::vector<PlacementTarget> targets;
    const auto& tables = stage.tables();
    targets.reserve(tables.size());
    for (std::size_t index = 0; index < tables.size(); ++index) {
        if (!kind.empty() && !sameName(tables[index].kind, kind)) {
            continue;
        }
        PlacementTarget target;
        target.tableIndex = index;
        target.kind = tables[index].kind;
        target.layer = tables[index].layer;
        target.rowCount = tables[index].table.rows().size();
        targets.push_back(std::move(target));
    }
    return targets;
}

std::int32_t nextFreeId(const smg::StageArchive& stage, std::string_view kind,
                        std::string_view field) {
    if (field.empty()) {
        return 0;
    }
    // These ids only have to be unique inside one placement list, so scan every
    // table of this kind (all layers) and take the smallest free number.
    std::vector<bool> used;
    std::size_t highest = 0;
    bool foundField = false;
    for (const auto& table : stage.tables()) {
        if (!kind.empty() && !sameName(table.kind, kind)) {
            continue;
        }
        const auto fieldIndex = table.table.fieldIndex(field);
        if (!fieldIndex) {
            continue;
        }
        foundField = true;
        for (const auto& row : table.table.rows()) {
            if (*fieldIndex >= row.values.size()) {
                continue;
            }
            const auto* stored = std::get_if<std::int32_t>(&row.values[*fieldIndex]);
            if (stored == nullptr || *stored < 0) {
                continue; // unset (-1) ids never claim a slot
            }
            const auto value = static_cast<std::size_t>(*stored);
            if (value + 1 > used.size()) {
                used.resize(value + 1, false);
            }
            used[value] = true;
            highest = std::max(highest, value);
        }
    }
    if (!foundField) {
        return 0; // the list has no such id field: nothing to avoid
    }
    for (std::size_t candidate = 0; candidate < used.size(); ++candidate) {
        if (!used[candidate]) {
            return static_cast<std::int32_t>(candidate);
        }
    }
    return static_cast<std::int32_t>(highest + 1);
}

std::optional<CreatedObject> createObject(smg::StageArchive& stage, UndoStack& stack,
                                         std::size_t tableIndex, const NewObject& object,
                                         std::string label) {
    if (tableIndex >= stage.tables().size()) {
        return std::nullopt;
    }
    const auto& table = stage.tables()[tableIndex].table;
    if (!table.fieldIndex("name")) {
        return std::nullopt; // not a placement list
    }

    // Start every field at the schema's zero value, then write the fields this
    // row actually needs. insertRow() coerces to each field's declared type, so
    // a byte-wide id stays a byte instead of silently widening the table.
    std::vector<smg::BcsvValue> values;
    values.reserve(table.fields().size());
    for (const auto& field : table.fields()) {
        values.push_back(smg::defaultValueFor(field.type));
    }
    const auto set = [&](std::string_view field, smg::BcsvValue value) {
        if (const auto index = table.fieldIndex(field)) {
            values[*index] = std::move(value);
        }
    };

    set("name", object.name);
    set("pos_x", object.position.x);
    set("pos_y", object.position.y);
    set("pos_z", object.position.z);
    set("dir_x", object.rotation.x);
    set("dir_y", object.rotation.y);
    set("dir_z", object.rotation.z);
    set("scale_x", object.scale.x);
    set("scale_y", object.scale.y);
    set("scale_z", object.scale.z);

    // Java assigned these through ObjIdUtil right after building the row; only
    // one of the two fields exists in a given list.
    const std::string_view kind = stage.tables()[tableIndex].kind;
    set("l_id", nextFreeId(stage, kind, "l_id"));
    set("MarioNo", nextFreeId(stage, kind, "MarioNo"));

    for (const auto field : kNoneFields) {
        set(field, std::int32_t{-1});
    }
    for (const auto field : kUnitScaleFields) {
        set(field, 1.0F);
    }

    std::string undoLabel = label.empty() ? "Add " + object.name : std::move(label);
    const auto rowIndex = addObject(stage, stack, tableIndex, std::move(values),
                                    std::move(undoLabel));
    const auto objectIndex = objectIndexAt(stage, tableIndex, rowIndex);
    if (!objectIndex) {
        return std::nullopt;
    }
    return CreatedObject{*objectIndex, tableIndex, rowIndex};
}

std::optional<std::size_t> objectIndexAt(const smg::StageArchive& stage, std::size_t tableIndex,
                                        std::size_t rowIndex) {
    const auto& objects = stage.objects();
    for (std::size_t index = 0; index < objects.size(); ++index) {
        if (objects[index].tableIndex == tableIndex && objects[index].rowIndex == rowIndex) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<CreatedObject> duplicateObject(smg::StageArchive& stage, UndoStack& stack,
                                            std::size_t objectIndex, const math::Vec3f& offset,
                                            std::string label) {
    if (objectIndex >= stage.objects().size()) {
        return std::nullopt;
    }
    // Copied by value: addObject() rebuilds objects(), which would dangle a
    // reference into the old placement vector.
    const smg::PlacementObject source = stage.objects()[objectIndex];
    if (source.tableIndex >= stage.tables().size()) {
        return std::nullopt;
    }
    auto& table = stage.tables()[source.tableIndex].table;
    if (source.rowIndex >= table.rows().size()) {
        return std::nullopt;
    }

    // Built from the source row rather than a blind byte copy so the copy gets
    // its own id and the requested offset; every other parameter is inherited,
    // which is what "duplicate" means to an author.
    auto values = table.rows()[source.rowIndex].values;
    const auto set = [&](std::string_view field, smg::BcsvValue value) {
        if (const auto index = table.fieldIndex(field)) {
            values[*index] = std::move(value);
        }
    };
    set("pos_x", source.position.x + offset.x);
    set("pos_y", source.position.y + offset.y);
    set("pos_z", source.position.z + offset.z);

    const std::string_view kind = stage.tables()[source.tableIndex].kind;
    if (table.fieldIndex("l_id")) {
        set("l_id", nextFreeId(stage, kind, "l_id"));
    }
    if (table.fieldIndex("MarioNo")) {
        set("MarioNo", nextFreeId(stage, kind, "MarioNo"));
    }

    std::string undoLabel = label.empty() ? "Duplicate " + source.name : std::move(label);
    // Immediately after the original, so the object list keeps the pair together.
    const auto rowIndex = addObject(stage, stack, source.tableIndex, std::move(values),
                                    std::move(undoLabel), source.rowIndex + 1);
    const auto newIndex = objectIndexAt(stage, source.tableIndex, rowIndex);
    if (!newIndex) {
        return std::nullopt;
    }
    return CreatedObject{*newIndex, source.tableIndex, rowIndex};
}

bool deleteObject(smg::StageArchive& stage, UndoStack& stack, std::size_t objectIndex,
                  std::string label) {
    if (objectIndex >= stage.objects().size()) {
        return false;
    }
    // Copy the location and name out first: removeObject() rebuilds objects(),
    // so the reference would dangle a line later.
    const std::size_t tableIndex = stage.objects()[objectIndex].tableIndex;
    const std::size_t rowIndex = stage.objects()[objectIndex].rowIndex;
    const std::string name = stage.objects()[objectIndex].name;
    std::string undoLabel = label.empty() ? "Delete " + name : std::move(label);
    return removeObject(stage, stack, tableIndex, rowIndex, std::move(undoLabel));
}

std::size_t deleteObjects(smg::StageArchive& stage, UndoStack& stack,
                          std::vector<std::size_t> objectIndices, std::string label) {
    struct Target {
        std::size_t tableIndex;
        std::size_t rowIndex;
    };
    std::vector<Target> targets;
    targets.reserve(objectIndices.size());
    for (const auto index : objectIndices) {
        if (index >= stage.objects().size()) {
            continue; // stale selection entry: skip rather than delete a neighbour
        }
        targets.push_back(
            Target{stage.objects()[index].tableIndex, stage.objects()[index].rowIndex});
    }
    if (targets.empty()) {
        return 0;
    }
    // Remove bottom-up, so an earlier removal can never shift a later target out
    // from under it. De-duplicating first keeps a repeated index from deleting
    // the wrong row (the second attempt would hit row+1 of the shrunk table).
    std::sort(targets.begin(), targets.end(), [](const Target& lhs, const Target& rhs) {
        if (lhs.tableIndex != rhs.tableIndex) {
            return lhs.tableIndex > rhs.tableIndex;
        }
        return lhs.rowIndex > rhs.rowIndex;
    });
    targets.erase(std::unique(targets.begin(), targets.end(),
                              [](const Target& lhs, const Target& rhs) {
                                  return lhs.tableIndex == rhs.tableIndex &&
                                         lhs.rowIndex == rhs.rowIndex;
                              }),
                  targets.end());

    const std::string undoLabel = label.empty() ? "Delete objects" : std::move(label);
    // One command per row, already applied, grouped so the whole selection comes
    // back with a single Ctrl+Z.
    auto group = std::make_unique<UndoMultiEntry>(undoLabel);
    std::size_t removed = 0;
    for (const auto& target : targets) {
        if (target.tableIndex >= stage.tables().size()) {
            continue;
        }
        auto& table = stage.tables()[target.tableIndex].table;
        if (target.rowIndex >= table.rows().size()) {
            continue;
        }
        auto values = table.rows()[target.rowIndex].values;
        table.removeRow(target.rowIndex);
        group->add(std::make_unique<RemoveObjectCommand>(stage, target.tableIndex,
                                                        target.rowIndex, std::move(values),
                                                        undoLabel));
        ++removed;
    }
    if (removed == 0) {
        return 0;
    }
    stage.rebuildObjects();
    stack.push(std::move(group));
    return removed;
}

namespace {

// The valid placement rows for `objectIndices`, de-duplicated and in front-to-
// back order. Everything group-sized funnels through this so no caller can
// accidentally move one object twice or address a row a previous edit removed.
std::vector<std::size_t> resolveSelection(const smg::StageArchive& stage,
                                          const std::vector<std::size_t>& objectIndices) {
    std::vector<std::size_t> resolved;
    resolved.reserve(objectIndices.size());
    for (const auto index : objectIndices) {
        if (index < stage.objects().size()) {
            resolved.push_back(index);
        }
    }
    std::sort(resolved.begin(), resolved.end());
    resolved.erase(std::unique(resolved.begin(), resolved.end()), resolved.end());
    return resolved;
}

// Applies `edit` to every resolved object and records the whole thing as one
// TransformCommand. Nothing is pushed while the edit runs, so a caller holding
// a group capture (see UndoStack::beginGroup) folds this into it.
bool runGroupTransform(smg::StageArchive& stage, UndoStack& stack,
                       const std::vector<std::size_t>& objectIndices,
                       const std::function<void(smg::PlacementObject&)>& edit,
                       std::string label, const char* action) {
    const auto resolved = resolveSelection(stage, objectIndices);
    if (resolved.empty()) {
        return false;
    }
    std::vector<smg::PlacementObject> before;
    before.reserve(resolved.size());
    std::vector<smg::PlacementObject> after;
    after.reserve(resolved.size());
    for (const auto index : resolved) {
        before.push_back(stage.objects()[index]);
        auto updated = before.back();
        edit(updated);
        after.push_back(updated);
    }
    if (label.empty()) {
        label = std::string(action) + " " + std::to_string(resolved.size()) +
                (resolved.size() == 1 ? " object" : " objects");
    }
    return applyTransform(stage, stack, std::move(before), std::move(after), std::move(label));
}

} // namespace

bool translateObjects(smg::StageArchive& stage, UndoStack& stack,
                      const std::vector<std::size_t>& objectIndices, const math::Vec3f& delta,
                      std::string label) {
    return runGroupTransform(
        stage, stack, objectIndices,
        [&](smg::PlacementObject& object) {
            object.position = {object.position.x + delta.x, object.position.y + delta.y,
                               object.position.z + delta.z};
        },
        std::move(label), "Move");
}

bool rotateObjects(smg::StageArchive& stage, UndoStack& stack,
                   const std::vector<std::size_t>& objectIndices, const math::Vec3f& degrees,
                   std::string label) {
    return runGroupTransform(
        stage, stack, objectIndices,
        [&](smg::PlacementObject& object) {
            object.rotation = {object.rotation.x + degrees.x, object.rotation.y + degrees.y,
                               object.rotation.z + degrees.z};
        },
        std::move(label), "Rotate");
}

bool scaleObjects(smg::StageArchive& stage, UndoStack& stack,
                  const std::vector<std::size_t>& objectIndices, const math::Vec3f& factors,
                  std::string label) {
    return runGroupTransform(
        stage, stack, objectIndices,
        [&](smg::PlacementObject& object) {
            const auto clampFactor = [](float factor) {
                return std::clamp(factor, 0.001F, 1000.0F);
            };
            object.scale = {object.scale.x * clampFactor(factors.x),
                            object.scale.y * clampFactor(factors.y),
                            object.scale.z * clampFactor(factors.z)};
        },
        std::move(label), "Scale");
}

} // namespace whitehole::edit
