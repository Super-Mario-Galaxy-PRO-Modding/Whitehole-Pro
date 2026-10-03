#include "whitehole/smg/bcsv_csv.hpp"

#include "whitehole/smg/hash.hpp"

#include <charconv>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <type_traits>

namespace whitehole::smg {
namespace {

// ---- writing -----------------------------------------------------------------

// Quotes a cell when the text would otherwise break the row: the delimiter, a
// quote, or a line break. Excel does exactly this, and it is why a string
// containing a comma must not be written bare.
[[nodiscard]] std::string quoteIfNeeded(std::string_view text) {
    bool needsQuotes = false;
    for (const char character : text) {
        if (character == ',' || character == '"' || character == '\r' || character == '\n') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return std::string(text);
    }
    std::string result = "\"";
    for (const char character : text) {
        if (character == '"') {
            result += "\"\"";
        } else {
            result += character;
        }
    }
    result += '"';
    return result;
}

// A cell's text. FLOATS GO THROUGH std::to_chars, not smg::toString: toString
// streams at default precision, so 123456.789F would export as "123457" and the
// import would write a different number back into the game than it read out.
[[nodiscard]] std::string formatValue(const BcsvValue& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::string>) {
                return item;
            } else if constexpr (std::is_same_v<Item, float>) {
                char buffer[40]{};
                const auto written = std::to_chars(buffer, buffer + sizeof(buffer), item);
                return std::string(buffer, written.ptr);
            } else if constexpr (std::is_same_v<Item, std::int8_t>) {
                return std::to_string(static_cast<int>(item));
            } else {
                return std::to_string(item);
            }
        },
        value);
}

[[nodiscard]] std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

// Exactly eight hex digits, the width of a BCSV field hash.
[[nodiscard]] bool parseHex8(std::string_view text, std::uint32_t& out) noexcept {
    if (text.size() != 8) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char character : text) {
        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        }
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    out = value;
    return true;
}

// A header cell -> the field hash it names. Three spellings, all of them things
// a real file contains: the game's bracketed form, a bare hex, or the field
// NAME (what the CLI's `bcsv inspect` prints, and what a human would type).
[[nodiscard]] std::uint32_t headerHash(std::string_view cell) {
    const std::string text = trim(cell);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        std::uint32_t hash = 0;
        if (parseHex8(std::string_view(text).substr(1, text.size() - 2), hash)) {
            return hash;
        }
    }
    std::uint32_t bare = 0;
    if (parseHex8(text, bare)) {
        return bare;
    }
    // A name. jmapHash is the hash the game itself writes for a field name, so a
    // readable column header resolves with no lookup table at all.
    return jmapHash(text);
}

// RFC4180-ish: a quoted cell may hold the delimiter, line breaks and doubled
// quotes; CRLF and LF both end a line. A blank line is NOT a record, so a file
// that merely ends with a newline cannot import a phantom empty row.
[[nodiscard]] std::vector<std::vector<std::string>> parseCsv(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    bool pending = false; // the current line holds something, so it is a record
    const auto endLine = [&]() {
        if (pending || !cell.empty() || !row.empty()) {
            row.push_back(std::move(cell));
            cell.clear();
            rows.push_back(std::move(row));
            row.clear();
        }
        cell.clear();
        pending = false;
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < text.size() && text[index + 1] == '"') {
                    cell += '"';
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                cell += character;
            }
        } else if (character == '"') {
            quoted = true;
            pending = true;
        } else if (character == ',') {
            row.push_back(std::move(cell));
            cell.clear();
            pending = true;
        } else if (character == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                continue; // the \n closes the line
            }
            endLine();
        } else if (character == '\n') {
            endLine();
        } else {
            cell += character;
            pending = true;
        }
    }
    endLine();
    return rows;
}

// ---- cell values -------------------------------------------------------------

// An integer cell. A spreadsheet writes whole numbers as "3", and sometimes as
// "3.0" for a column it decided was floating point: that is accepted. "3.7" is
// refused with a message rather than silently truncated, because a level's
// coordinates are not a place to guess.
[[nodiscard]] bool parseInteger(std::string_view text, std::int32_t& out) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        out = 0; // clearing a cell in Excel means zero, not "leave it alone"
        return true;
    }
    const char* first = trimmed.data();
    const char* last = first + trimmed.size();
    std::int64_t whole = 0;
    const auto asInteger = std::from_chars(first, last, whole);
    if (asInteger.ec == std::errc{} && asInteger.ptr == last) {
        if (whole < -2147483648LL || whole > 2147483647LL) {
            return false;
        }
        out = static_cast<std::int32_t>(whole);
        return true;
    }
    double decimal = 0.0;
    const auto asDecimal = std::from_chars(first, last, decimal);
    if (asDecimal.ec != std::errc{} || asDecimal.ptr != last || !std::isfinite(decimal)) {
        return false;
    }
    if (decimal != std::trunc(decimal) || decimal < -2147483648.0 || decimal > 2147483647.0) {
        return false;
    }
    out = static_cast<std::int32_t>(decimal);
    return true;
}

// A float cell. NaN and the infinities are REFUSED: they parse fine and then
// poison whatever reads the table, which is how this repo has already lost an
// afternoon once (BLUEPRINT section 4, the camera solver).
[[nodiscard]] bool parseFloat(std::string_view text, float& out) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        out = 0.0F;
        return true;
    }
    const char* first = trimmed.data();
    const char* last = first + trimmed.size();
    float value = 0.0F;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// Writes one cell, or records why it could not be. It goes through the by-id
// setters so a masked or packed field is written by the SAME coercion path the
// panels and the CLI use -- a spreadsheet edit must not be a second, looser way
// into a table.
void assignCell(BcsvTable& table, BcsvRow& row, const BcsvField& field, std::string_view cell,
                std::size_t line, std::string_view columnLabel,
                std::vector<std::string>& badCells) {
    const auto refused = [&](const std::string& reason) {
        badCells.push_back("line " + std::to_string(line) + ", " + std::string(columnLabel) + ": "
                           + reason);
    };
    switch (field.type) {
    case BcsvType::floatingPoint: {
        float value = 0.0F;
        if (!parseFloat(cell, value)) {
            refused("'" + trim(cell) + "' is not a number");
            return;
        }
        table.setFloatById(row, field.hash, value);
        return;
    }
    case BcsvType::fixedString:
    case BcsvType::stringOffset:
        table.setStringById(row, field.hash, std::string(cell));
        return;
    case BcsvType::integer:
    case BcsvType::integer2:
    case BcsvType::shortInteger:
    case BcsvType::byte: {
        std::int32_t value = 0;
        if (!parseInteger(cell, value)) {
            refused("'" + trim(cell) + "' is not a whole number");
            return;
        }
        table.setIntById(row, field.hash, value);
        return;
    }
    }
}

} // namespace

std::string bcsvCsvHeader(std::uint32_t hash, const FieldHashes* hashes) {
    if (hashes != nullptr) {
        return hashes->nameOf(hash); // the name when it is known, else "[XXXXXXXX]"
    }
    // No lookup table -- the GUI deliberately does not load hashlookup.txt. The
    // bracketed hash comes from the SAME nameOf() rather than a second snprintf
    // here, so the two spellings of a hash cannot drift apart.
    static const FieldHashes emptyHashes;
    return emptyHashes.nameOf(hash);
}

std::string bcsvToCsv(const BcsvTable& table, const FieldHashes* hashes) {
    std::string out;
    out.reserve((table.fields().size() + 1) * table.rows().size() * 8 + 64);
    for (std::size_t column = 0; column < table.fields().size(); ++column) {
        if (column > 0) {
            out += ',';
        }
        out += quoteIfNeeded(bcsvCsvHeader(table.fields()[column].hash, hashes));
    }
    out += "\r\n";
    for (const auto& row : table.rows()) {
        for (std::size_t column = 0; column < table.fields().size(); ++column) {
            if (column > 0) {
                out += ',';
            }
            if (column < row.values.size()) {
                out += quoteIfNeeded(formatValue(row.values[column]));
            }
        }
        out += "\r\n";
    }
    return out;
}

BcsvCsvPlan planCsvImport(const BcsvTable& table, std::string_view csv) {
    const auto lines = parseCsv(csv);
    if (lines.empty()) {
        throw std::runtime_error(
            "That file has no rows: a CSV import needs a header row naming the columns");
    }
    BcsvCsvPlan plan;
    plan.table = table; // schema, offsets, masks, types and endianness all preserved
    plan.rowsBefore = plan.table.rows().size();

    const auto& header = lines.front();
    std::vector<std::optional<std::size_t>> columns(header.size());
    for (std::size_t column = 0; column < header.size(); ++column) {
        columns[column] = table.fieldIndex(headerHash(header[column]));
        if (!columns[column].has_value()) {
            plan.unknownColumns.push_back(trim(header[column]));
        }
    }

    // The row count follows the file: extra rows are appended, surplus rows at
    // the end are dropped, and both are counted so the caller can say so BEFORE
    // anything is applied.
    const std::size_t wanted = lines.size() - 1;
    while (plan.table.rows().size() > wanted) {
        (void)plan.table.removeRow(plan.table.rows().size() - 1);
    }
    while (plan.table.rows().size() < wanted) {
        (void)plan.table.addRow();
    }
    plan.rowsAfter = plan.table.rows().size();

    for (std::size_t line = 1; line < lines.size(); ++line) {
        const auto& cells = lines[line];
        // `line` is a 0-based index, and lines[0] is the header, so the number a
        // person sees in the spreadsheet is index + 1.
        const std::size_t lineNo = line + 1;
        // A row with more cells than columns means a quote went missing
        // somewhere (a stray comma splits a value in two), and every cell after
        // it would land in the wrong column. Refuse the row rather than write
        // misaligned numbers into a level.
        if (cells.size() > header.size()) {
            plan.badCells.push_back("line " + std::to_string(lineNo) + ": " +
                                    std::to_string(cells.size()) + " cells but the header has " +
                                    std::to_string(header.size()) + " columns (a stray comma?)");
            continue;
        }
        auto& target = plan.table.rows()[line - 1];
        for (std::size_t column = 0; column < columns.size() && column < cells.size(); ++column) {
            if (!columns[column].has_value()) {
                continue; // reported above; a new column is never added as a side effect
            }
            assignCell(plan.table, target, table.fields()[*columns[column]], cells[column], lineNo,
                       trim(header[column]), plan.badCells);
        }
    }
    return plan;
}

} // namespace whitehole::smg
