#pragma once

// Validation & guidance layer: turns an open stage (plus the object database)
// into a list of actionable findings a beginner can act on -- "this object does
// not exist in SMG2", "this required parameter is unset", "SW_A points at
// switch 0". The same report feeds the Problems panel, the CLI `zone validate`
// command and the tests, so guidance can never drift between surfaces.

#include "whitehole/db/object_db.hpp"
#include "whitehole/smg/object_model.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::db {
class CustomObjDatabase;
}

namespace whitehole::edit {

enum class Severity { Info, Warning, Error };

[[nodiscard]] std::string_view toString(Severity severity) noexcept;

struct Finding {
    Severity severity{Severity::Info};
    std::string code;    // stable identifier, e.g. "unknown-object"
    std::string message; // human-readable description
    std::string hint;    // suggested fix
    std::optional<std::size_t> objectIndex; // null for stage-level findings
    std::string field;   // BCSV field name, may be empty
};

struct ValidationReport {
    std::vector<Finding> findings;

    [[nodiscard]] std::size_t count(Severity severity) const noexcept;
    [[nodiscard]] bool clean() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return findings.empty(); }
};

// Rules:
//   unknown-object    (Warning) placement name is not in the database
//   game-mismatch     (Warning) object exists but not in this game
//   missing-required  (Warning) a "Needed" class parameter is absent from the row
//   unset-required    (Info)    a "Needed" parameter is still zero/empty
//   dangling-switch   (Warning) a switch field references switch 0
//   zero-scale        (Info)    scale is (0,0,0) so the object would vanish
//
// `customObjects` (optional) is the CustomObjDatabase the BCSV editor keeps in
// step with object sections. A name registered there is a modder's own object,
// not a typo, so it must not raise `unknown-object`.
[[nodiscard]] ValidationReport validateStage(const smg::StageArchive& stage,
                                            const db::ObjectDatabase& database, int gameType,
                                            const db::CustomObjDatabase* customObjects = nullptr);

} // namespace whitehole::edit