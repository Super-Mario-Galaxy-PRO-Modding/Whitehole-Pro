#include "whitehole/smg/object_model.hpp"

#include "whitehole/edit/commands.hpp"
#include "whitehole/smg/hash.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace whitehole::smg {
namespace {

struct BaseField {
    const char* identifier;
    const char* label;
    db::PropertyKind kind;
};

// Fields every JMap placement row carries, in the order the editor shows them.
constexpr std::array<BaseField, 10> kBaseFields{{
    {"name", "Name", db::PropertyKind::Text},
    {"pos_x", "Position X", db::PropertyKind::Float},
    {"pos_y", "Position Y", db::PropertyKind::Float},
    {"pos_z", "Position Z", db::PropertyKind::Float},
    {"dir_x", "Rotation X", db::PropertyKind::Float},
    {"dir_y", "Rotation Y", db::PropertyKind::Float},
    {"dir_z", "Rotation Z", db::PropertyKind::Float},
    {"scale_x", "Scale X", db::PropertyKind::Float},
    {"scale_y", "Scale Y", db::PropertyKind::Float},
    {"scale_z", "Scale Z", db::PropertyKind::Float},
}};

bool extractInt(const BcsvValue& value, std::int32_t& out) {
    if (const auto* item = std::get_if<std::int32_t>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<std::int16_t>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<std::int8_t>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<float>(&value)) { out = static_cast<std::int32_t>(*item); return true; }
    return false;
}

bool extractFloat(const BcsvValue& value, float& out) {
    if (const auto* item = std::get_if<float>(&value)) { out = *item; return true; }
    if (const auto* item = std::get_if<std::int32_t>(&value)) { out = static_cast<float>(*item); return true; }
    return false;
}

bool extractString(const BcsvValue& value, std::string& out) {
    if (const auto* item = std::get_if<std::string>(&value)) { out = *item; return true; }
    return false;
}

} // namespace

ObjectModel::ObjectModel(StageArchive& stage, const db::ObjectDatabase& database, int gameType)
    : stage_(&stage), database_(&database), gameType_(gameType) {}

bool ObjectModel::locate(std::size_t objectIndex, std::size_t& tableIndex, std::size_t& rowIndex) const {
    if (objectIndex >= stage_->objects().size()) {
        return false;
    }
    const auto& object = stage_->objects()[objectIndex];
    tableIndex = object.tableIndex;
    rowIndex = object.rowIndex;
    if (tableIndex >= stage_->tables().size()) {
        return false;
    }
    if (rowIndex >= stage_->tables()[tableIndex].table.rows().size()) {
        return false;
    }
    return true;
}

const db::ClassInfo* ObjectModel::objectClass(std::size_t objectIndex) const {
    if (objectIndex >= stage_->objects().size()) {
        return nullptr;
    }
    return database_->classForObject(stage_->objects()[objectIndex].name, gameType_);
}

std::vector<ObjectField> ObjectModel::fields(std::size_t objectIndex) const {
    std::vector<ObjectField> result;
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return result;
    }
    const auto& table = stage_->tables()[tableIndex].table;
    const auto& row = table.rows()[rowIndex];

    for (const auto& base : kBaseFields) {
        ObjectField field;
        field.identifier = base.identifier;
        field.label = base.label;
        field.kind = base.kind;
        field.used = true;
        if (const auto* raw = table.rawValue(row, std::string_view(base.identifier))) {
            field.present = true;
            field.value = *raw;
        }
        result.push_back(std::move(field));
    }

    const auto* info = objectClass(objectIndex);
    if (info == nullptr) {
        return result;
    }
    // Class properties live in an unordered_map; sort so the grid and the tests
    // see the same order every run.
    std::vector<std::string> identifiers;
    identifiers.reserve(info->properties.size());
    for (const auto& entry : info->properties) {
        identifiers.push_back(entry.first);
    }
    std::sort(identifiers.begin(), identifiers.end());

    const auto& objectName = stage_->objects()[objectIndex].name;
    for (const auto& identifier : identifiers) {
        const auto found = info->properties.find(identifier);
        if (found == info->properties.end()) {
            continue;
        }
        const auto& property = found->second;
        // Only advertise fields this archive actually stores; a missing field
        // cannot be edited without rewriting the table layout.
        if (!table.hasField(identifier)) {
            continue;
        }
        ObjectField field;
        field.identifier = identifier;
        field.label = property.simpleName.empty() ? identifier : property.simpleName;
        field.description = property.description;
        field.kind = property.kind;
        field.needed = property.needed;
        field.present = true;
        field.used = property.appliesTo(gameType_, objectName);
        field.values = property.values;
        if (const auto* raw = table.rawValue(row, identifier)) {
            field.value = *raw;
        }
        result.push_back(std::move(field));
    }
    return result;
}

std::string_view propertyKindLabel(db::PropertyKind kind) noexcept {
    using db::PropertyKind;
    switch (kind) {
    case PropertyKind::Integer: return "integer";
    case PropertyKind::Float: return "float";
    case PropertyKind::Boolean: return "boolean";
    case PropertyKind::Text: return "text";
    case PropertyKind::List: return "list";
    case PropertyKind::IntList: return "int list";
    case PropertyKind::TextList: return "text list";
    case PropertyKind::SwitchId: return "switch";
    case PropertyKind::ObjectName: return "object";
    case PropertyKind::Bitfield: return "bitfield";
    case PropertyKind::Unknown: break;
    }
    return "unknown";
}

// The raw value, unformatted. Text prints itself; numbers print through
// smg::toString so an int8_t never renders as a raw character.
std::string ObjectField::plainValue() const { return toString(value); }

// On/off interpretation: anything non-zero counts, matching the fixed-function
// BCSV convention where booleans are stored as ordinary integers.
bool ObjectField::flag() const noexcept {
    if (const auto* number = std::get_if<float>(&value)) {
        return *number != 0.0F;
    }
    if (const auto* number = std::get_if<std::int32_t>(&value)) {
        return *number != 0;
    }
    if (const auto* number = std::get_if<std::int16_t>(&value)) {
        return *number != 0;
    }
    if (const auto* number = std::get_if<std::int8_t>(&value)) {
        return *number != 0;
    }
    return false;
}

// Numeric interpretation. A float is exact; an integer converts losslessly into
// a double; free text is not a number at all, which is how callers tell the two
// widget families apart.
bool ObjectField::decimal(double& out) const noexcept {
    if (const auto* number = std::get_if<float>(&value)) {
        out = static_cast<double>(*number);
        return true;
    }
    if (const auto* number = std::get_if<std::int32_t>(&value)) {
        out = static_cast<double>(*number);
        return true;
    }
    if (const auto* number = std::get_if<std::int16_t>(&value)) {
        out = static_cast<double>(*number);
        return true;
    }
    if (const auto* number = std::get_if<std::int8_t>(&value)) {
        out = static_cast<double>(*number);
        return true;
    }
    return false;
}

bool ObjectModel::getInt(std::size_t objectIndex, std::string_view field, std::int32_t& out) const {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    const auto& table = stage_->tables()[tableIndex].table;
    const auto* raw = table.rawValue(table.rows()[rowIndex], field);
    return raw != nullptr && extractInt(*raw, out);
}

bool ObjectModel::getFloat(std::size_t objectIndex, std::string_view field, float& out) const {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    const auto& table = stage_->tables()[tableIndex].table;
    const auto* raw = table.rawValue(table.rows()[rowIndex], field);
    return raw != nullptr && extractFloat(*raw, out);
}

bool ObjectModel::getString(std::size_t objectIndex, std::string_view field, std::string& out) const {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    const auto& table = stage_->tables()[tableIndex].table;
    const auto* raw = table.rawValue(table.rows()[rowIndex], field);
    return raw != nullptr && extractString(*raw, out);
}

bool ObjectModel::getBool(std::size_t objectIndex, std::string_view field, bool& out) const {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    const auto& table = stage_->tables()[tableIndex].table;
    const auto* raw = table.rawValue(table.rows()[rowIndex], field);
    if (raw == nullptr || std::holds_alternative<std::string>(*raw)) {
        return false;
    }
    std::int32_t asInteger = 0;
    if (!extractInt(*raw, asInteger)) {
        return false;
    }
    out = asInteger != 0;
    return true;
}

void ObjectModel::record(std::size_t tableIndex, std::size_t rowIndex, std::vector<BcsvValue> before,
                         std::string_view field, edit::UndoStack& stack, const std::string& label) {
    auto after = edit::captureRowValues(*stage_, tableIndex, rowIndex);
    if (before == after) {
        return; // No-op edit: never pollute the undo history.
    }
    stage_->rebuildObjects();
    const auto action = label.empty() ? ("Edit " + std::string(field)) : label;
    stack.push(std::make_unique<edit::RowEditCommand>(*stage_, tableIndex, rowIndex,
                                                      std::move(before), std::move(after), action));
}

bool ObjectModel::setInt(std::size_t objectIndex, std::string_view field, std::int32_t value,
                         edit::UndoStack& stack, std::string label) {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    auto& table = stage_->tables()[tableIndex].table;
    auto& row = table.rows()[rowIndex];
    const auto hash = jmapHash(field);
    if (table.rawValue(row, hash) == nullptr) {
        return false;
    }
    auto before = row.values;
    table.setIntById(row, hash, value);
    record(tableIndex, rowIndex, std::move(before), field, stack, label);
    return true;
}

bool ObjectModel::setFloat(std::size_t objectIndex, std::string_view field, float value,
                           edit::UndoStack& stack, std::string label) {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    auto& table = stage_->tables()[tableIndex].table;
    auto& row = table.rows()[rowIndex];
    if (table.rawValue(row, field) == nullptr) {
        return false;
    }
    auto before = row.values;
    table.setFloat(row, field, value);
    record(tableIndex, rowIndex, std::move(before), field, stack, label);
    return true;
}

bool ObjectModel::setString(std::size_t objectIndex, std::string_view field, std::string value,
                            edit::UndoStack& stack, std::string label) {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    auto& table = stage_->tables()[tableIndex].table;
    auto& row = table.rows()[rowIndex];
    if (table.rawValue(row, field) == nullptr) {
        return false;
    }
    auto before = row.values;
    table.setString(row, field, std::move(value));
    record(tableIndex, rowIndex, std::move(before), field, stack, label);
    return true;
}

bool ObjectModel::setBool(std::size_t objectIndex, std::string_view field, bool value,
                          edit::UndoStack& stack, std::string label) {
    std::size_t tableIndex = 0;
    std::size_t rowIndex = 0;
    if (!locate(objectIndex, tableIndex, rowIndex)) {
        return false;
    }
    auto& table = stage_->tables()[tableIndex].table;
    auto& row = table.rows()[rowIndex];
    const auto hash = jmapHash(field);
    if (table.rawValue(row, hash) == nullptr) {
        return false;
    }
    auto before = row.values;
    table.setBoolById(row, hash, value);
    record(tableIndex, rowIndex, std::move(before), field, stack, label);
    return true;
}

} // namespace whitehole::smg