#include "whitehole/edit/rails.hpp"

#include "whitehole/edit/commands.hpp"
#include "whitehole/smg/path.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace whitehole::edit {
namespace {

// Keeps point ids equal to row order (Java's PathPointObj.save contract), so
// the id-sorted load can never disagree with the table.
void renumberPointIds(smg::BcsvTable& table) {
    if (!table.hasField("id")) {
        return;
    }
    auto& rows = table.rows();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        table.setInt(rows[index], "id", static_cast<std::int32_t>(index));
    }
}

// A values vector aligned to the table's fields, defaulted per declared type,
// with a name-keyed setter for the fields this edit cares about.
std::vector<smg::BcsvValue> alignedRow(const smg::BcsvTable& table) {
    std::vector<smg::BcsvValue> values;
    values.reserve(table.fields().size());
    for (const auto& field : table.fields()) {
        values.push_back(smg::defaultValueFor(field.type));
    }
    return values;
}

void setNamed(const smg::BcsvTable& table, std::vector<smg::BcsvValue>& values, std::string_view name,
              smg::BcsvValue value) {
    if (const auto index = table.fieldIndex(name); index && *index < values.size()) {
        values[*index] = std::move(value);
    }
}

} // namespace

std::optional<std::size_t> createPath(smg::StageArchive& stage, UndoStack& stack, std::string label) {
    if (label.empty()) {
        label = "New path";
    }
    // Locate the CommonPathInfo table, creating it (schema and all) when the
    // zone never had one -- save() inserts the missing file via RarcArchive.
    std::size_t pathTableIndex = static_cast<std::size_t>(-1);
    for (std::size_t index = 0; index < stage.tables().size(); ++index) {
        if (stage.tables()[index].kind == "path") {
            pathTableIndex = index;
            break;
        }
    }
    if (pathTableIndex == static_cast<std::size_t>(-1)) {
        smg::ObjectTable table;
        table.path = "/Stage/jmp/Path/CommonPathInfo";
        table.kind = "path";
        table.layer = "Common";
        stage.tables().push_back(std::move(table));
        pathTableIndex = stage.tables().size() - 1;
        auto& schema = stage.tables()[pathTableIndex].table;
        schema.ensureField("name", smg::BcsvType::stringOffset);
        schema.ensureField("type", smg::BcsvType::stringOffset);
        schema.ensureField("closed", smg::BcsvType::stringOffset);
        schema.ensureField("num_pnt", smg::BcsvType::integer);
        schema.ensureField("l_id", smg::BcsvType::integer);
        for (int index = 0; index < 8; ++index) {
            schema.ensureField("path_arg" + std::to_string(index), smg::BcsvType::integer);
        }
        schema.ensureField("usage", smg::BcsvType::stringOffset);
        schema.ensureField("no", smg::BcsvType::shortInteger);
        schema.ensureField("Path_ID", smg::BcsvType::shortInteger);
    }
    if (stage.tables()[pathTableIndex].table.fields().empty()) {
        return std::nullopt;
    }

    // Unique ids, mirroring Java: l_id picks the name, `no` picks the file.
    std::int32_t nextId = 0;
    std::int32_t nextNo = 0;
    const auto& info = stage.tables()[pathTableIndex].table;
    for (const auto& row : info.rows()) {
        nextId = std::max(nextId, info.getInt(row, "l_id", -1) + 1);
        nextNo = std::max(nextNo, info.getInt(row, "no", -1) + 1);
    }

    smg::ObjectTable points;
    points.path = smg::pathPointFile(nextNo);
    points.kind = "pathpoint";
    points.layer = "Common";
    smg::ensurePathPointSchema(points.table);
    stage.tables().push_back(std::move(points));

    // The push above may have reallocated tables_: re-index before writing.
    auto& table = stage.tables()[pathTableIndex].table;
    auto values = alignedRow(table);
    setNamed(table, values, "name", "Path " + std::to_string(nextId));
    setNamed(table, values, "type", "Bezier");
    setNamed(table, values, "closed", "OPEN");
    setNamed(table, values, "num_pnt", std::int32_t{0});
    setNamed(table, values, "l_id", nextId);
    setNamed(table, values, "no", nextNo);
    setNamed(table, values, "Path_ID", std::int32_t{-1});
    setNamed(table, values, "usage", "General");
    for (int index = 0; index < 8; ++index) {
        setNamed(table, values, "path_arg" + std::to_string(index), std::int32_t{-1});
    }
    try {
        return addObject(stage, stack, pathTableIndex, std::move(values), label);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}


std::optional<std::size_t> addPathPoint(smg::StageArchive& stage, UndoStack& stack,
                                        std::size_t pointTableIndex, const math::Vec3f& position,
                                        std::optional<std::size_t> insertAt, std::string label) {
    if (label.empty()) {
        label = "Add path point";
    }
    if (pointTableIndex >= stage.tables().size()
        || stage.tables()[pointTableIndex].kind != "pathpoint") {
        return std::nullopt;
    }
    auto& table = stage.tables()[pointTableIndex].table;
    // Additive: a legacy points file missing a field gains it here.
    smg::ensurePathPointSchema(table);

    auto values = alignedRow(table);
    setNamed(table, values, "id", std::int32_t{0}); // renumbered below
    for (const char* set : {"pnt0", "pnt1", "pnt2"}) {
        setNamed(table, values, std::string(set) + "_x", position.x);
        setNamed(table, values, std::string(set) + "_y", position.y);
        setNamed(table, values, std::string(set) + "_z", position.z);
    }
    for (int index = 0; index < 8; ++index) {
        setNamed(table, values, "point_arg" + std::to_string(index), std::int32_t{-1});
    }

    try {
        const auto rowIndex =
            addObject(stage, stack, pointTableIndex, std::move(values), label, insertAt);
        renumberPointIds(stage.tables()[pointTableIndex].table);
        return rowIndex;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool removePathPoint(smg::StageArchive& stage, UndoStack& stack, std::size_t pointTableIndex,
                     std::size_t rowIndex, std::string label) {
    if (label.empty()) {
        label = "Delete path point";
    }
    if (!removeObject(stage, stack, pointTableIndex, rowIndex, label)) {
        return false;
    }
    renumberPointIds(stage.tables()[pointTableIndex].table);
    return true;
}

} // namespace whitehole::edit
