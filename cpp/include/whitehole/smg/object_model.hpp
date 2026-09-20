#pragma once

// Per-object view of a stage table row, resolved against the community object
// database. This is the native half of the Java AbstractObj + PropertyGrid
// pairing: it answers "which fields does this object have, what do they mean,
// and what is each one set to right now".
//
// Phase 0 scope: it reads and writes fields that already exist in the stage
// table. Adding a brand-new field is deliberately not done here because that
// changes the row layout of every row in the table -- set*() reports false for
// an absent field rather than silently rewriting the archive.

#include "whitehole/db/object_db.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// One editable BCSV field on one object, with its database metadata.
struct ObjectField {
    std::string identifier;          // BCSV field name, e.g. "Obj_arg0"
    std::string label;               // human label; falls back to identifier
    std::string description;         // tooltip text
    db::PropertyKind kind{db::PropertyKind::Unknown};
    bool needed{false};              // database marks it as required
    bool present{false};             // the field exists in this row
    bool used{false};                // passes the game/exclusives filter
    std::vector<std::string> values; // value list, e.g. "0: Off"
    BcsvValue value{std::int32_t{0}};

    // The stored value as plain text, without any database formatting.
    [[nodiscard]] std::string plainValue() const;
    // Interprets the stored value as an on/off flag; false for empty or zero.
    [[nodiscard]] bool flag() const noexcept;
    // Interprets the stored value as a decimal number; false on free text.
    [[nodiscard]] bool decimal(double& out) const noexcept;
};

class ObjectModel {
public:
    ObjectModel(StageArchive& stage, const db::ObjectDatabase& database, int gameType);

    [[nodiscard]] int gameType() const noexcept { return gameType_; }
    void setGameType(int gameType) noexcept { gameType_ = gameType; }

    [[nodiscard]] const db::ObjectDatabase& database() const noexcept { return *database_; }
    [[nodiscard]] StageArchive& stage() noexcept { return *stage_; }
    [[nodiscard]] const StageArchive& stage() const noexcept { return *stage_; }
    [[nodiscard]] std::size_t objectCount() const noexcept { return stage_->objects().size(); }

    // Class describing the object, or nullptr when the database has none.
    [[nodiscard]] const db::ClassInfo* objectClass(std::size_t objectIndex) const;

    // Base transform fields first (name / pos / rot / scale), then the object's
    // class properties in a deterministic order.
    [[nodiscard]] std::vector<ObjectField> fields(std::size_t objectIndex) const;

    [[nodiscard]] bool getInt(std::size_t objectIndex, std::string_view field, std::int32_t& out) const;
    [[nodiscard]] bool getFloat(std::size_t objectIndex, std::string_view field, float& out) const;
    [[nodiscard]] bool getString(std::size_t objectIndex, std::string_view field, std::string& out) const;
    [[nodiscard]] bool getBool(std::size_t objectIndex, std::string_view field, bool& out) const;

    // Sets a value and records it on `stack`. A change that would not alter the
    // row is not recorded. Returns false when the object or field is missing.
    bool setInt(std::size_t objectIndex, std::string_view field, std::int32_t value,
                edit::UndoStack& stack, std::string label = {});
    bool setFloat(std::size_t objectIndex, std::string_view field, float value,
                  edit::UndoStack& stack, std::string label = {});
    bool setString(std::size_t objectIndex, std::string_view field, std::string value,
                   edit::UndoStack& stack, std::string label = {});
    bool setBool(std::size_t objectIndex, std::string_view field, bool value,
                 edit::UndoStack& stack, std::string label = {});

private:
    [[nodiscard]] bool locate(std::size_t objectIndex, std::size_t& tableIndex,
                              std::size_t& rowIndex) const;
    // Pushes exactly one undo entry for a change that has already been applied.
    void record(std::size_t tableIndex, std::size_t rowIndex, std::vector<BcsvValue> before,
                std::string_view field, edit::UndoStack& stack, const std::string& label);

    StageArchive* stage_;
    const db::ObjectDatabase* database_;
    int gameType_{2};
};

// Lower-case type name for the property grid ("float", "integer", ...).
[[nodiscard]] std::string_view propertyKindLabel(db::PropertyKind kind) noexcept;

} // namespace whitehole::smg