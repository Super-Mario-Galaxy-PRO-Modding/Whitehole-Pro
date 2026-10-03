#pragma once

// BCSV <-> spreadsheet text, so a table can be bulk-edited in Excel and pasted
// back. This is roadmap 4's actual win (BLUEPRINT section 16 item 6): the panels
// already cover the fields an author edits one at a time, and what the BCSV
// editor could not do was move a hundred rows at once.
//
// THE ROUND TRIP IS THE CONTRACT. Export, edit in Excel, import, and the table
// must come back byte-for-byte where nobody typed anything. Three decisions
// exist only to make that true:
//
//   * FLOATS ARE WRITTEN SHORTEST-ROUND-TRIP (std::to_chars), not through
//     smg::toString -- that uses an ostringstream at default precision, so
//     123456.789F would export as "123457" and quietly change on the way back.
//   * THE HEADER IS THE FIELD HASH, spelled the way the game itself spells it:
//     "[1A2B3C4D]", or the field's name when a FieldHashes knows it. Import
//     accepts either, plus a bare 8-digit hex, and resolves a name with
//     jmapHash -- so a CSV edited on a machine with no hashlookup.txt still
//     imports, and a human can read the columns.
//   * EVERY ROW IS EXPORTED, never the editor's filtered view. Re-importing a
//     filtered export would silently delete every row the search hid, which is
//     the one failure of this feature that looks like it worked.

#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/field_hashes.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// The header cell for one column: the name when `hashes` knows it, else the
// game's own "[XXXXXXXX]" form. `hashes` may be null (the GUI does not load
// hashlookup.txt), in which case every header is the bracketed hash.
[[nodiscard]] std::string bcsvCsvHeader(std::uint32_t hash, const FieldHashes* hashes);

// The whole table as CSV, header row first, CRLF line endings (Excel's own, so
// a hand-edited file does not end up with one long line). A trailing newline is
// written; an empty table is still a valid header row.
[[nodiscard]] std::string bcsvToCsv(const BcsvTable& table, const FieldHashes* hashes = nullptr);

// What an import WOULD do, computed without touching the caller's table.
//
// Same plan/apply split as the create dialog and the playtest export: the UI has
// to show what is about to change -- rows before and after, columns it could not
// resolve, cells it refused -- and then apply exactly that. `table` is the
// schema source and is never modified.
struct BcsvCsvPlan {
    BcsvTable table;      // the table as it would become
    std::size_t rowsBefore{0};
    std::size_t rowsAfter{0};
    // Header cells that match no field in this table. Their COLUMN is ignored,
    // deliberately: adding a column changes the bytes the game reads, and that
    // stays a deliberate act (the editor's Add column button), never a side
    // effect of pasting numbers in.
    std::vector<std::string> unknownColumns;
    // "line 4, Obj_arg0: 'abc' is not a number". The cell keeps its old value, so
    // a typo costs one cell rather than the whole table. `line` is the CSV line
    // (1 = the header), which is what a spreadsheet shows the author.
    std::vector<std::string> badCells;

    [[nodiscard]] bool changesRows() const noexcept { return rowsBefore != rowsAfter; }
};

// Reads `csv` and applies it to a COPY of `table`, whose schema (fields,
// offsets, masks, types, endianness) is preserved exactly -- only values and the
// row count can change.
//
// The first row MUST be a header. The row count follows the CSV: extra rows are
// appended, and surplus rows at the end are dropped, both counted in the plan so
// the caller can say so before applying. An empty cell means "the zero value for
// this column" rather than "leave it alone", because clearing a cell in Excel is
// how a person says zero.
[[nodiscard]] BcsvCsvPlan planCsvImport(const BcsvTable& table, std::string_view csv);

} // namespace whitehole::smg
