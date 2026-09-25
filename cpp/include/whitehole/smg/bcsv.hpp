#pragma once

#include "whitehole/io/binary_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace whitehole::smg {

enum class BcsvType : std::uint8_t {
    integer = 0,
    fixedString = 1,
    floatingPoint = 2,
    integer2 = 3,
    shortInteger = 4,
    byte = 5,
    stringOffset = 6,
};

using BcsvValue = std::variant<std::int32_t, std::string, float, std::int16_t, std::int8_t>;

struct BcsvField {
    std::uint32_t hash{0};
    std::uint32_t mask{0xFFFFFFFFU};
    std::uint16_t offset{0};
    std::uint8_t shift{0};
    BcsvType type{BcsvType::integer};
};

struct BcsvRow {
    std::vector<BcsvValue> values;
};

class BcsvTable {
public:
    BcsvTable() = default;
    BcsvTable(std::vector<std::uint8_t> data, io::Endian endian);

    [[nodiscard]] static BcsvTable open(const std::filesystem::path& path, io::Endian endian);

    [[nodiscard]] io::Endian endian() const noexcept { return endian_; }
    [[nodiscard]] std::uint32_t entrySize() const noexcept { return entrySize_; }
    [[nodiscard]] const std::vector<BcsvField>& fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<BcsvField>& fields() noexcept {
        // Field offsets, masks and types may change, so drop the cached hash index.
        fieldLookup_.clear();
        return fields_;
    }
    [[nodiscard]] const std::vector<BcsvRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] std::vector<BcsvRow>& rows() noexcept { return rows_; }

    [[nodiscard]] std::optional<std::size_t> fieldIndex(std::string_view name) const;
    [[nodiscard]] std::optional<std::size_t> fieldIndex(std::uint32_t hash) const;
    [[nodiscard]] bool hasField(std::string_view name) const;
    [[nodiscard]] std::string getString(const BcsvRow& row, std::string_view name,
                                        std::string fallback = {}) const;
        [[nodiscard]] float getFloat(const BcsvRow& row, std::string_view name, float fallback = 0.0F) const;
    [[nodiscard]] std::int32_t getInt(const BcsvRow& row, std::string_view name,
                                      std::int32_t fallback = 0) const;
    [[nodiscard]] bool getBool(const BcsvRow& row, std::string_view name,
                               bool fallback = false) const;
    [[nodiscard]] std::string getStringById(const BcsvRow& row, std::uint32_t hash,
                                            std::string fallback = {}) const;
    [[nodiscard]] float getFloatById(const BcsvRow& row, std::uint32_t hash, float fallback = 0.0F) const;
    [[nodiscard]] std::int32_t getIntById(const BcsvRow& row, std::uint32_t hash,
                                          std::int32_t fallback = 0) const;
    [[nodiscard]] bool getBoolById(const BcsvRow& row, std::uint32_t hash,
                                   bool fallback = false) const;
    // Raw stored value for a field, or nullptr when the field/row has no value.
    // The pointer stays valid until the table is mutated.
    [[nodiscard]] const BcsvValue* rawValue(const BcsvRow& row, std::uint32_t hash) const;
    [[nodiscard]] const BcsvValue* rawValue(const BcsvRow& row, std::string_view name) const;

    void setString(BcsvRow& row, std::string_view name, std::string value);
    void setFloat(BcsvRow& row, std::string_view name, float value);
    // Type-aware integer/boolean setters: the stored variant is chosen from the
    // field's declared type, so a byte field keeps an int8_t and so on.
    void setInt(BcsvRow& row, std::string_view name, std::int32_t value);
    void setIntById(BcsvRow& row, std::uint32_t hash, std::int32_t value);
    void setBool(BcsvRow& row, std::string_view name, bool value);
    void setBoolById(BcsvRow& row, std::uint32_t hash, bool value);

    // Hash-addressed setters, the write-side counterpart of get*ById. A table
    // grid knows the column by its field hash (that is the only stable identity
    // a BCSV field has), not by a name it may not be able to resolve, so these
    // are what a generic editor writes through. No-ops when the hash is unknown.
    void setStringById(BcsvRow& row, std::uint32_t hash, std::string value);
    void setFloatById(BcsvRow& row, std::uint32_t hash, float value);

    // ---- structural mutation --------------------------------------------
    // Appends a row initialised with type-appropriate defaults (0 / 0.0f / "").
    // Returns the new row index. Throws when the table has no fields, because
    // there is no schema to size a row against.
    [[nodiscard]] std::size_t addRow();
    // Inserts a copy of `index` directly after it and returns the new index.
    // Throws std::out_of_range when `index` is not a valid row.
    [[nodiscard]] std::size_t cloneRow(std::size_t index);
    // Removes a row. Returns false when `index` is out of range.
    bool removeRow(std::size_t index);
    // Ensures a field exists, appending it when absent, and returns its index.
    // Appending preserves every existing field offset and widens each row, so
    // previously written data stays byte-identical.
    [[nodiscard]] std::size_t ensureField(std::string_view name, BcsvType type);

    // ---- column (field) editing ----------------------------------------
    // Renames a field. Only the hash changes: offsets, masks and every stored
    // value stay where they are, so the table's byte layout is untouched.
    // Throws std::out_of_range for a bad index and std::runtime_error when the
    // name is empty or already used by another field.
    void renameField(std::size_t index, std::string_view newName);
    // Removes a field and its value slot from every row. The row stride and the
    // remaining offsets are deliberately left alone: the bytes the field
    // occupied become padding, which keeps every other field byte-identical
    // (recomputing offsets could break fields that share one word with masks).
    // Returns false when `index` is out of range.
    bool removeField(std::size_t index);
    // Changes a field's declared type and re-coerces every stored value. The
    // field keeps its offset; a wider type is only accepted when it does not
    // run into the next field's storage. Throws std::runtime_error when the
    // resize would overlap another field (that table would corrupt on save).
    void setFieldType(std::size_t index, BcsvType type);

    // Replaces one row's values. Values are coerced to the field's declared
    // type, missing entries fall back to defaults and extras are dropped.
    // Throws std::out_of_range when `index` is not a valid row.
    void setRow(std::size_t index, const std::vector<BcsvValue>& values);
    // Inserts a row before `index` (clamped to the row count) with `values`
    // coerced the same way as setRow(). Returns the index actually used.
    [[nodiscard]] std::size_t insertRow(std::size_t index, const std::vector<BcsvValue>& values);

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;

private:
    void parse(const std::vector<std::uint8_t>& data);
    // Rebuilds the hash index after a structural field edit.
    void rebuildFieldLookup();

    io::Endian endian_{io::Endian::big};
    std::uint32_t entrySize_{0};
    std::vector<BcsvField> fields_;
    std::vector<BcsvRow> rows_;
    std::unordered_map<std::uint32_t, std::size_t> fieldLookup_;
};

[[nodiscard]] std::string toString(const BcsvValue& value);

// Zero value for a field type, matching how the game pads an unused slot.
[[nodiscard]] BcsvValue defaultValueFor(BcsvType type);

} // namespace whitehole::smg
