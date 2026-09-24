#include "whitehole/smg/bcsv.hpp"

#include "whitehole/smg/hash.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace whitehole::smg {
namespace {

constexpr std::array<std::size_t, 7> fieldSizes{4, 32, 4, 4, 2, 1, 4};

std::size_t checkedProduct(std::uint32_t count, std::uint32_t size, std::string_view description) {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        throw std::runtime_error("BCSV " + std::string(description) + " overflows the host address space");
    }
    return static_cast<std::size_t>(count) * size;
}

std::string readTerminatedString(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("BCSV string offset points outside the file");
    }
    const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = std::find(begin, data.end(), 0);
    if (end == data.end()) {
        throw std::runtime_error("BCSV string is not null-terminated");
    }
    return {begin, end};
}

std::uint32_t integerValue(const BcsvValue& value, BcsvType type) {
    switch (type) {
    case BcsvType::integer:
    case BcsvType::integer2:
        return static_cast<std::uint32_t>(std::get<std::int32_t>(value));
    case BcsvType::shortInteger:
        return static_cast<std::uint16_t>(std::get<std::int16_t>(value));
    case BcsvType::byte:
        return static_cast<std::uint8_t>(std::get<std::int8_t>(value));
    default:
        throw std::logic_error("BCSV field is not an integer type");
    }
}

std::uint32_t readPacked(const std::vector<std::uint8_t>& output, std::size_t offset,
                         std::size_t width, io::Endian endian) {
    std::uint32_t result = 0;
    if (endian == io::Endian::big) {
        for (std::size_t index = 0; index < width; ++index) {
            result = (result << 8U) | output[offset + index];
        }
    } else {
        for (std::size_t index = 0; index < width; ++index) {
            result |= static_cast<std::uint32_t>(output[offset + index]) << (index * 8U);
        }
    }
    return result;
}

void writePacked(std::vector<std::uint8_t>& output, std::size_t offset, std::size_t width,
                 io::Endian endian, std::uint32_t value) {
    for (std::size_t index = 0; index < width; ++index) {
        const auto byteIndex = endian == io::Endian::big ? width - index - 1 : index;
        output[offset + byteIndex] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

bool isIntegerType(BcsvType type) noexcept {
    return type == BcsvType::integer || type == BcsvType::integer2
        || type == BcsvType::shortInteger || type == BcsvType::byte;
}

// Stores `input` in the variant arm that matches the field's declared width.
void assignInteger(BcsvValue& value, BcsvType type, std::int32_t input) {
    switch (type) {
    case BcsvType::integer:
    case BcsvType::integer2:
        value = input;
        return;
    case BcsvType::shortInteger:
        value = static_cast<std::int16_t>(input);
        return;
    case BcsvType::byte:
        value = static_cast<std::int8_t>(input);
        return;
    case BcsvType::floatingPoint:
        value = static_cast<float>(input);
        return;
    default:
        throw std::logic_error("BCSV field is not an integer type");
    }
}

std::size_t fieldWidth(BcsvType type) {
    const auto rawType = static_cast<std::uint8_t>(type);
    if (rawType >= fieldSizes.size()) {
        throw std::runtime_error("Cannot size an unsupported BCSV field type");
    }
    return fieldSizes[rawType];
}

std::int32_t asInteger(const BcsvValue& value) {
    return std::visit([](const auto& item) -> std::int32_t {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::string>) {
            return 0;
        } else {
            return static_cast<std::int32_t>(item);
        }
    }, value);
}

float asFloat(const BcsvValue& value) {
    return std::visit([](const auto& item) -> float {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::string>) {
            return 0.0F;
        } else {
            return static_cast<float>(item);
        }
    }, value);
}

std::string asString(const BcsvValue& value) {
    return std::visit([](const auto& item) -> std::string {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::string>) {
            return item;
        } else {
            return {};
        }
    }, value);
}

// Forces `value` into the variant arm the field's declared type expects, so a
// restored snapshot can never cause std::get<> to throw during serialization.
BcsvValue coerceToType(const BcsvValue& value, BcsvType type) {
    switch (type) {
    case BcsvType::integer:
    case BcsvType::integer2:
        return std::int32_t{asInteger(value)};
    case BcsvType::shortInteger:
        return static_cast<std::int16_t>(asInteger(value));
    case BcsvType::byte:
        return static_cast<std::int8_t>(asInteger(value));
    case BcsvType::floatingPoint:
        return asFloat(value);
    case BcsvType::fixedString:
    case BcsvType::stringOffset:
        return asString(value);
    }
    return std::int32_t{0};
}

} // namespace

BcsvTable::BcsvTable(std::vector<std::uint8_t> data, io::Endian endian) : endian_(endian) {
    parse(data);
}

BcsvTable BcsvTable::open(const std::filesystem::path& path, io::Endian endian) {
    return BcsvTable(io::readFile(path), endian);
}

std::optional<std::size_t> BcsvTable::fieldIndex(std::uint32_t hash) const {
    if (fieldLookup_.size() == fields_.size()) {
        const auto found = fieldLookup_.find(hash);
        if (found == fieldLookup_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    // The non-const fields() accessor drops the cache, so fall back to a scan.
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        if (fields_[index].hash == hash) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> BcsvTable::fieldIndex(std::string_view name) const {
    return fieldIndex(fieldHash(name));
}

std::string BcsvTable::getString(const BcsvRow& row, std::string_view name, std::string fallback) const {
    const auto index = fieldIndex(name);
    if (!index || *index >= row.values.size()) {
        return fallback;
    }
    if (const auto* text = std::get_if<std::string>(&row.values[*index])) {
        return *text;
    }
    return toString(row.values[*index]);
}

float BcsvTable::getFloat(const BcsvRow& row, std::string_view name, float fallback) const {
    const auto index = fieldIndex(name);
    if (!index || *index >= row.values.size()) {
        return fallback;
    }
    if (const auto* value = std::get_if<float>(&row.values[*index])) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int32_t>(&row.values[*index])) {
        return static_cast<float>(*value);
    }
    return fallback;
}

std::string BcsvTable::getStringById(const BcsvRow& row, std::uint32_t hash, std::string fallback) const {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return fallback;
    }
    if (const auto* text = std::get_if<std::string>(&row.values[*index])) {
        return *text;
    }
    return toString(row.values[*index]);
}

float BcsvTable::getFloatById(const BcsvRow& row, std::uint32_t hash, float fallback) const {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return fallback;
    }
    if (const auto* value = std::get_if<float>(&row.values[*index])) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int32_t>(&row.values[*index])) {
        return static_cast<float>(*value);
    }
    return fallback;
}

std::int32_t BcsvTable::getIntById(const BcsvRow& row, std::uint32_t hash, std::int32_t fallback) const {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return fallback;
    }
    return std::visit([&](const auto& value) -> std::int32_t {
        using Item = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Item, std::string> || std::is_same_v<Item, float>) {
            return fallback;
        } else {
            return static_cast<std::int32_t>(value);
        }
    }, row.values[*index]);
}

std::int32_t BcsvTable::getInt(const BcsvRow& row, std::string_view name,
                               std::int32_t fallback) const {
    return getIntById(row, jmapHash(name), fallback);
}

void BcsvTable::setStringById(BcsvRow& row, std::uint32_t hash, std::string value) {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return;
    }
    if (std::holds_alternative<std::string>(row.values[*index])) {
        row.values[*index] = std::move(value);
    }
}

void BcsvTable::setString(BcsvRow& row, std::string_view name, std::string value) {
    setStringById(row, fieldHash(name), std::move(value));
}

void BcsvTable::setFloatById(BcsvRow& row, std::uint32_t hash, float value) {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return;
    }
    if (std::holds_alternative<float>(row.values[*index])) {
        row.values[*index] = value;
    }
}

void BcsvTable::setFloat(BcsvRow& row, std::string_view name, float value) {
    setFloatById(row, fieldHash(name), value);
}

bool BcsvTable::hasField(std::string_view name) const {
    return fieldIndex(name).has_value();
}

bool BcsvTable::getBool(const BcsvRow& row, std::string_view name, bool fallback) const {
    const auto index = fieldIndex(name);
    if (!index) {
        return fallback;
    }
    return getBoolById(row, fields_[*index].hash, fallback);
}

bool BcsvTable::getBoolById(const BcsvRow& row, std::uint32_t hash, bool fallback) const {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return fallback;
    }
    return std::visit([&fallback](const auto& value) -> bool {
        using Item = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Item, std::string>) {
            return fallback;
        } else if constexpr (std::is_same_v<Item, float>) {
            return value != 0.0F;
        } else {
            return value != 0;
        }
    }, row.values[*index]);
}

const BcsvValue* BcsvTable::rawValue(const BcsvRow& row, std::uint32_t hash) const {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return nullptr;
    }
    return &row.values[*index];
}

const BcsvValue* BcsvTable::rawValue(const BcsvRow& row, std::string_view name) const {
    return rawValue(row, jmapHash(name));
}

void BcsvTable::setIntById(BcsvRow& row, std::uint32_t hash, std::int32_t value) {
    const auto index = fieldIndex(hash);
    if (!index || *index >= row.values.size()) {
        return;
    }
    assignInteger(row.values[*index], fields_[*index].type, value);
}

void BcsvTable::setInt(BcsvRow& row, std::string_view name, std::int32_t value) {
    setIntById(row, fieldHash(name), value);
}

void BcsvTable::setBoolById(BcsvRow& row, std::uint32_t hash, bool value) {
    setIntById(row, hash, value ? 1 : 0);
}

void BcsvTable::setBool(BcsvRow& row, std::string_view name, bool value) {
    setBoolById(row, fieldHash(name), value);
}

std::size_t BcsvTable::addRow() {
    if (fields_.empty()) {
        throw std::runtime_error("Cannot add a BCSV row to a table without fields");
    }
    if (rows_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("BCSV contains too many rows");
    }
    BcsvRow row;
    row.values.reserve(fields_.size());
    for (const auto& field : fields_) {
        row.values.push_back(defaultValueFor(field.type));
    }
    rows_.push_back(std::move(row));
    return rows_.size() - 1;
}

std::size_t BcsvTable::cloneRow(std::size_t index) {
    if (index >= rows_.size()) {
        throw std::out_of_range("BCSV row index out of range");
    }
    if (rows_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("BCSV contains too many rows");
    }
    BcsvRow copy = rows_[index];
    rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(copy));
    return index + 1;
}

bool BcsvTable::removeRow(std::size_t index) {
    if (index >= rows_.size()) {
        return false;
    }
    rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::size_t BcsvTable::ensureField(std::string_view name, BcsvType type) {
    const auto hash = fieldHash(name);
    if (const auto existing = fieldIndex(hash)) {
        return *existing;
    }
    if (fields_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("BCSV contains too many fields");
    }
    const auto width = fieldWidth(type);
    if (entrySize_ > std::numeric_limits<std::uint16_t>::max() - width) {
        throw std::runtime_error("BCSV entry size would exceed the 16-bit field offset");
    }

    BcsvField field;
    field.hash = hash;
    field.mask = isIntegerType(type) && width < 4
        ? static_cast<std::uint32_t>((1U << (width * 8U)) - 1U)
        : 0xFFFFFFFFU;
    field.offset = static_cast<std::uint16_t>(entrySize_);
    field.shift = 0;
    field.type = type;
    fields_.push_back(field);
    entrySize_ += static_cast<std::uint32_t>(width);

    const auto value = defaultValueFor(type);
    for (auto& row : rows_) {
        row.values.push_back(value);
    }
    // Offsets moved past the appended field, so rebuild the hash index.
    fieldLookup_.clear();
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        fieldLookup_[fields_[i].hash] = i;
    }
    return fields_.size() - 1;
}
// Rebuilds the hash -> index map from scratch. Any structural edit (rename,
// remove, type change) must call this: fieldIndex() only trusts the cache when
// it covers every field, but rebuilding eagerly keeps lookups O(1).
void BcsvTable::rebuildFieldLookup() {
    fieldLookup_.clear();
    fieldLookup_.reserve(fields_.size());
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        fieldLookup_[fields_[index].hash] = index;
    }
}

void BcsvTable::renameField(std::size_t index, std::string_view newName) {
    if (index >= fields_.size()) {
        throw std::out_of_range("BCSV field index out of range");
    }
    if (newName.empty()) {
        throw std::runtime_error("A BCSV field name cannot be empty");
    }
    const auto hash = fieldHash(newName);
    if (const auto other = fieldIndex(hash); other && *other != index) {
        throw std::runtime_error("Another field already uses the name \"" +
                                 std::string(newName) + "\"");
    }
    fields_[index].hash = hash;
    rebuildFieldLookup();
}

bool BcsvTable::removeField(std::size_t index) {
    if (index >= fields_.size()) {
        return false;
    }
    fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(index));
    // Values are positional, so every row must drop the same slot. Rows that
    // were already short are padded instead of skipped: serialize() requires
    // one value per field, and a mismatch there would make the table unwritable.
    for (auto& row : rows_) {
        if (index < row.values.size()) {
            row.values.erase(row.values.begin() + static_cast<std::ptrdiff_t>(index));
        }
        while (row.values.size() < fields_.size()) {
            row.values.push_back(defaultValueFor(fields_[row.values.size()].type));
        }
    }
    rebuildFieldLookup();
    return true;
}

void BcsvTable::setFieldType(std::size_t index, BcsvType type) {
    if (index >= fields_.size()) {
        throw std::out_of_range("BCSV field index out of range");
    }
    BcsvField& field = fields_[index];
    if (field.type == type) {
        return;
    }
    const auto newWidth = static_cast<std::uint32_t>(fieldWidth(type));
    // A wider field may only grow into unused space. Refusing the change beats
    // writing over a neighbour: serialize() would silently clobber it.
    for (std::size_t other = 0; other < fields_.size(); ++other) {
        if (other == index) {
            continue;
        }
        const std::uint32_t otherOffset = fields_[other].offset;
        if (otherOffset >= field.offset && otherOffset < field.offset + newWidth) {
            throw std::runtime_error(
                "Cannot resize this field: it would overlap the storage of another field");
        }
    }
    field.type = type;
    // Java parity (Bcsv.Field.changeType): a type change drops any bit packing.
    // Fixed strings are written as raw bytes, so their mask is meaningless.
    field.mask = type == BcsvType::fixedString ? 0U : 0xFFFFFFFFU;
    field.shift = 0;
    const auto requiredSize = static_cast<std::uint32_t>(field.offset) + newWidth;
    if (requiredSize > entrySize_) {
        entrySize_ = requiredSize;
    }
    // Stored values must match the new variant arm, or serialize() would throw
    // std::bad_variant_access for a field the user merely retyped.
    for (auto& row : rows_) {
        if (index < row.values.size()) {
            row.values[index] = coerceToType(row.values[index], type);
        }
    }
    rebuildFieldLookup();
}



BcsvValue defaultValueFor(BcsvType type) {
    switch (type) {
    case BcsvType::integer:
    case BcsvType::integer2:
        return std::int32_t{0};
    case BcsvType::shortInteger:
        return std::int16_t{0};
    case BcsvType::byte:
        return std::int8_t{0};
    case BcsvType::floatingPoint:
        return 0.0F;
    case BcsvType::fixedString:
    case BcsvType::stringOffset:
        return std::string{};
    }
    return std::int32_t{0};
}

void BcsvTable::parse(const std::vector<std::uint8_t>& data) {
    fields_.clear();
    rows_.clear();
    entrySize_ = 0;
    if (data.empty()) {
        return;
    }
    if (data.size() < 0x10) {
        throw std::runtime_error("BCSV header is truncated");
    }

    io::BinaryReader reader(data, endian_);
    const auto rowCount = reader.readU32();
    const auto fieldCount = reader.readU32();
    const auto dataOffset = reader.readU32();
    entrySize_ = reader.readU32();

    if (fieldCount > (data.size() - 0x10) / 0x0C) {
        throw std::runtime_error("BCSV field count is not plausible for the file size");
    }
    const auto fieldTableEnd = 0x10U + static_cast<std::size_t>(fieldCount) * 0x0CU;
    if (dataOffset < fieldTableEnd || dataOffset > data.size()) {
        throw std::runtime_error("BCSV data offset is invalid");
    }
    if (rowCount != 0 && entrySize_ == 0) {
        throw std::runtime_error("BCSV has rows with a zero-sized entry");
    }
    const auto rowBytes = checkedProduct(rowCount, entrySize_, "row table");
    if (rowBytes > data.size() - dataOffset) {
        throw std::runtime_error("BCSV row table extends outside the file");
    }
    const auto stringOffset = static_cast<std::size_t>(dataOffset) + rowBytes;

    fields_.reserve(fieldCount);
    for (std::uint32_t index = 0; index < fieldCount; ++index) {
        reader.seek(0x10U + static_cast<std::size_t>(index) * 0x0CU);
        BcsvField field;
        field.hash = reader.readU32();
        field.mask = reader.readU32();
        field.offset = reader.readU16();
        field.shift = reader.readU8();
        const auto rawType = reader.readU8();
        if (rawType >= fieldSizes.size()) {
            throw std::runtime_error("BCSV uses unsupported field type " + std::to_string(rawType));
        }
        field.type = static_cast<BcsvType>(rawType);
        const auto width = fieldSizes[rawType];
        if (field.offset > entrySize_ || width > entrySize_ - field.offset) {
            throw std::runtime_error("BCSV field extends outside its row");
        }
        const auto bitWidth = width * 8U;
        if ((field.type == BcsvType::integer || field.type == BcsvType::integer2
             || field.type == BcsvType::shortInteger || field.type == BcsvType::byte)
            && field.shift >= bitWidth) {
            throw std::runtime_error("BCSV integer field has an invalid shift");
        }
        fields_.push_back(field);
    }

    rows_.reserve(rowCount);
    for (std::uint32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        BcsvRow row;
        row.values.reserve(fields_.size());
        const auto rowOffset = static_cast<std::size_t>(dataOffset)
            + static_cast<std::size_t>(rowIndex) * entrySize_;
        for (const auto& field : fields_) {
            reader.seek(rowOffset + field.offset);
            switch (field.type) {
            case BcsvType::integer:
            case BcsvType::integer2:
                row.values.emplace_back(static_cast<std::int32_t>((reader.readU32() & field.mask) >> field.shift));
                break;
            case BcsvType::fixedString: {
                const auto bytes = reader.readBytes(32);
                const auto end = std::find(bytes.begin(), bytes.end(), 0);
                row.values.emplace_back(std::string(bytes.begin(), end));
                break;
            }
            case BcsvType::floatingPoint:
                row.values.emplace_back(reader.readF32());
                break;
            case BcsvType::shortInteger:
                row.values.emplace_back(static_cast<std::int16_t>((reader.readU16() & field.mask) >> field.shift));
                break;
            case BcsvType::byte:
                row.values.emplace_back(static_cast<std::int8_t>((reader.readU8() & field.mask) >> field.shift));
                break;
            case BcsvType::stringOffset: {
                const auto relativeOffset = reader.readU32();
                if (relativeOffset > data.size() - stringOffset) {
                    throw std::runtime_error("BCSV string offset overflows the string table");
                }
                row.values.emplace_back(readTerminatedString(data, stringOffset + relativeOffset));
                break;
            }
            }
        }
        rows_.push_back(std::move(row));
    }

    fieldLookup_.clear();
    fieldLookup_.reserve(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        fieldLookup_[fields_[i].hash] = i;
    }
}

std::vector<std::uint8_t> BcsvTable::serialize() const {
    if (fields_.size() > std::numeric_limits<std::uint32_t>::max()
        || rows_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("BCSV contains too many fields or rows");
    }
    const auto fieldTableEnd = 0x10U + fields_.size() * 0x0CU;
    if (fieldTableEnd > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("BCSV field table is too large");
    }
    const auto rowBytes = checkedProduct(static_cast<std::uint32_t>(rows_.size()), entrySize_, "row table");
    if (rowBytes > std::numeric_limits<std::uint32_t>::max() - fieldTableEnd) {
        throw std::runtime_error("BCSV row table is too large");
    }
    const auto dataOffset = static_cast<std::uint32_t>(fieldTableEnd);
    const auto stringOffset = fieldTableEnd + rowBytes;
    std::vector<std::uint8_t> output(stringOffset, 0);

    io::BinaryWriter header(endian_);
    header.writeU32(static_cast<std::uint32_t>(rows_.size()));
    header.writeU32(static_cast<std::uint32_t>(fields_.size()));
    header.writeU32(dataOffset);
    header.writeU32(entrySize_);
    for (const auto& field : fields_) {
        const auto rawType = static_cast<std::uint8_t>(field.type);
        if (rawType >= fieldSizes.size()) {
            throw std::runtime_error("Cannot write unsupported BCSV field type");
        }
        const auto width = fieldSizes[rawType];
        if (field.offset > entrySize_ || width > entrySize_ - field.offset) {
            throw std::runtime_error("Cannot write a BCSV field outside its row");
        }
        const auto isInteger = field.type == BcsvType::integer || field.type == BcsvType::integer2
            || field.type == BcsvType::shortInteger || field.type == BcsvType::byte;
        if (isInteger && field.shift >= width * 8U) {
            throw std::runtime_error("Cannot write a BCSV integer field with an invalid shift");
        }
        header.writeU32(field.hash);
        header.writeU32(field.mask);
        header.writeU16(field.offset);
        header.writeU8(field.shift);
        header.writeU8(rawType);
    }
    std::copy(header.data().begin(), header.data().end(), output.begin());

    std::unordered_map<std::string, std::uint32_t> stringOffsets;
    for (std::size_t rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
        const auto& row = rows_[rowIndex];
        if (row.values.size() != fields_.size()) {
            throw std::runtime_error("BCSV row value count does not match its field count");
        }
        const auto rowOffset = static_cast<std::size_t>(dataOffset) + rowIndex * entrySize_;
        for (std::size_t fieldIndex = 0; fieldIndex < fields_.size(); ++fieldIndex) {
            const auto& field = fields_[fieldIndex];
            const auto& value = row.values[fieldIndex];
            const auto valueOffset = rowOffset + field.offset;
            switch (field.type) {
            case BcsvType::integer:
            case BcsvType::integer2:
            case BcsvType::shortInteger:
            case BcsvType::byte: {
                const auto width = fieldSizes[static_cast<std::uint8_t>(field.type)];
                const auto current = readPacked(output, valueOffset, width, endian_);
                const auto rawValue = integerValue(value, field.type);
                const auto widthMask = width == 4
                    ? std::numeric_limits<std::uint32_t>::max()
                    : static_cast<std::uint32_t>((1U << (width * 8U)) - 1U);
                const auto storageMask = field.mask & widthMask;
                const auto encoded = (rawValue << field.shift) & storageMask;
                if ((encoded >> field.shift) != rawValue) {
                    throw std::runtime_error("BCSV integer value does not fit its field mask");
                }
                writePacked(output, valueOffset, width, endian_, (current & ~storageMask) | encoded);
                break;
            }
            case BcsvType::fixedString: {
                const auto& text = std::get<std::string>(value);
                if (text.find('\0') != std::string::npos) {
                    throw std::runtime_error("BCSV strings cannot contain embedded nulls");
                }
                if (text.size() > 32) {
                    throw std::runtime_error("BCSV fixed string exceeds its 32-byte field");
                }
                std::copy_n(text.begin(), static_cast<std::ptrdiff_t>(text.size()),
                            output.begin() + static_cast<std::ptrdiff_t>(valueOffset));
                break;
            }
            case BcsvType::floatingPoint: {
                io::BinaryWriter writer(endian_);
                writer.writeF32(std::get<float>(value));
                std::copy(writer.data().begin(), writer.data().end(),
                          output.begin() + static_cast<std::ptrdiff_t>(valueOffset));
                break;
            }
            case BcsvType::stringOffset: {
                const auto& text = std::get<std::string>(value);
                if (text.find('\0') != std::string::npos) {
                    throw std::runtime_error("BCSV strings cannot contain embedded nulls");
                }
                auto found = stringOffsets.find(text);
                std::uint32_t relativeOffset = 0;
                if (found == stringOffsets.end()) {
                    const auto stringBytes = output.size() - stringOffset;
                    if (stringBytes > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::runtime_error("BCSV string table is too large");
                    }
                    relativeOffset = static_cast<std::uint32_t>(stringBytes);
                    stringOffsets.emplace(text, relativeOffset);
                    output.insert(output.end(), text.begin(), text.end());
                    output.push_back(0);
                } else {
                    relativeOffset = found->second;
                }
                writePacked(output, valueOffset, 4, endian_, relativeOffset);
                break;
            }
            }
        }
    }

    if (output.size() > std::numeric_limits<std::size_t>::max() - 0x1FU) {
        throw std::runtime_error("BCSV output is too large to align");
    }
    const auto alignedSize = (output.size() + 0x1FU) & ~std::size_t{0x1F};
    output.resize(alignedSize, 0x40);
    return output;
}

void BcsvTable::setRow(std::size_t index, const std::vector<BcsvValue>& values) {
    if (index >= rows_.size()) {
        throw std::out_of_range("BCSV row index out of range");
    }
    auto& target = rows_[index].values;
    target.resize(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        target[i] = i < values.size() ? coerceToType(values[i], fields_[i].type)
                                      : defaultValueFor(fields_[i].type);
    }
}

std::size_t BcsvTable::insertRow(std::size_t index, const std::vector<BcsvValue>& values) {
    if (fields_.empty()) {
        throw std::runtime_error("Cannot insert a BCSV row into a table without fields");
    }
    if (rows_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("BCSV contains too many rows");
    }
    if (index > rows_.size()) {
        index = rows_.size();
    }
    BcsvRow row;
    row.values.resize(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        row.values[i] = i < values.size() ? coerceToType(values[i], fields_[i].type)
                                          : defaultValueFor(fields_[i].type);
    }
    rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(index), std::move(row));
    return index;
}

std::string toString(const BcsvValue& value) {
    return std::visit([](const auto& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::string>) {
            return item;
        } else if constexpr (std::is_same_v<Item, std::int8_t>) {
            return std::to_string(static_cast<int>(item));
        } else {
            std::ostringstream stream;
            stream << item;
            return stream.str();
        }
    }, value);
}

} // namespace whitehole::smg
