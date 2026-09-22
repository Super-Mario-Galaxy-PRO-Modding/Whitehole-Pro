#pragma once

// Level authoring: creating, duplicating and deleting placement objects.
//
// This is the native replacement for the Java editor's object creation path
// (GalaxyEditorForm.addObject() plus one AbstractObj subclass per object type,
// with ObjectSelectForm browsing ObjectDB for the name) and for its delete path.
//
// Java needed a subclass per object type purely to know which BCSV fields a new
// row carries and what each one starts at. The native side answers both from the
// stage's own table schema plus the same "-1 means none" sentinel set the Java
// constructors wrote by hand, and it picks the destination list from the
// database's ListSMG1/ListSMG2 entry instead of a hand-written switch.
//
// Everything here mutates the stage and records exactly one undo entry, so the
// desktop editor, the CLI and the tests share one implementation. No Win32, no
// ImGui, and no database is needed to build a row.

#include "whitehole/edit/undo.hpp"
#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::edit {

// One placement list a new object can be added to: a (kind, layer) table.
struct PlacementTarget {
    std::size_t tableIndex{0};
    std::string kind;  // "obj", "mappart", "area", "start", ...
    std::string layer; // "Common", "LayerA", ...
    std::size_t rowCount{0};
};

// Native kind for a database placement list ("MapPartsInfo" -> "mappart") and
// the reverse. Both directions ignore case; an unknown list yields an empty
// view, which callers read as "this stage has no such list".
[[nodiscard]] std::string_view kindForList(std::string_view list) noexcept;
[[nodiscard]] std::string_view listForKind(std::string_view kind) noexcept;

// Tables in the stage, or only those of `kind` when it is not empty, in stage
// order (so on real data the first entry of a kind is its Common layer).
[[nodiscard]] std::vector<PlacementTarget> placementTargets(const smg::StageArchive& stage,
                                                           std::string_view kind = {});

// Smallest id not used by any row of `kind`'s tables for `field` ("l_id",
// "MarioNo"), or 0 when the field is absent everywhere. Mirrors Java
// ObjIdUtil.generateUniqueLinkID()/generateUniqueMarioNo().
[[nodiscard]] std::int32_t nextFreeId(const smg::StageArchive& stage, std::string_view kind,
                                      std::string_view field);

// Everything the caller has decided before the row is built.
struct NewObject {
    std::string name;
    math::Vec3f position{};
    math::Vec3f rotation{};
    math::Vec3f scale{1.0F, 1.0F, 1.0F};
};

// Where a created object ended up, so the caller can select it.
struct CreatedObject {
    std::size_t objectIndex{0}; // index into StageArchive::objects()
    std::size_t tableIndex{0};
    std::size_t rowIndex{0};
};

// Appends `object` to the table at `tableIndex`. Returns nullopt when the table
// does not exist or its schema has no "name" field, because then there is no
// placement row to build. Throws only what BCSV insertion throws (a table with
// no fields at all).
[[nodiscard]] std::optional<CreatedObject> createObject(smg::StageArchive& stage, UndoStack& stack,
                                                        std::size_t tableIndex,
                                                        const NewObject& object,
                                                        std::string label = {});

// Copies the row of `objectIndex` directly after itself, moved by `offset` (all
// zero keeps the exact transform) and given a fresh id so both copies stay
// addressable. Returns nullopt when `objectIndex` is stale.
[[nodiscard]] std::optional<CreatedObject> duplicateObject(smg::StageArchive& stage,
                                                           UndoStack& stack,
                                                           std::size_t objectIndex,
                                                           const math::Vec3f& offset = {},
                                                           std::string label = {});

// Removes one object. undo() puts it back at the same row index.
[[nodiscard]] bool deleteObject(smg::StageArchive& stage, UndoStack& stack,
                                std::size_t objectIndex, std::string label = {});

// Removes several objects as a single undo step. `objectIndices` may arrive in
// any order and may repeat; rows are removed bottom-up per table so an earlier
// removal can never invalidate a later one. Returns how many rows were removed.
[[nodiscard]] std::size_t deleteObjects(smg::StageArchive& stage, UndoStack& stack,
                                        std::vector<std::size_t> objectIndices,
                                        std::string label = {});

// Index into objects() for the object stored at (tableIndex, rowIndex), or
// nullopt when no such row exists.
[[nodiscard]] std::optional<std::size_t> objectIndexAt(const smg::StageArchive& stage,
                                                       std::size_t tableIndex,
                                                       std::size_t rowIndex);

// ---- group transforms ----------------------------------------------------
// The mouse-driven editing path: move/rotate/scale a whole selection as one
// undo entry. Stale indices are skipped, so a selection captured before another
// edit never throws away the rest of the group. Each entry point returns false
// (and records nothing) when no valid object was found.

// Adds `delta` to the position of every listed object.
[[nodiscard]] bool translateObjects(smg::StageArchive& stage, UndoStack& stack,
                                    const std::vector<std::size_t>& objectIndices,
                                    const math::Vec3f& delta, std::string label = {});

// Adds `degrees` to the rotation of every listed object.
[[nodiscard]] bool rotateObjects(smg::StageArchive& stage, UndoStack& stack,
                                 const std::vector<std::size_t>& objectIndices,
                                 const math::Vec3f& degrees, std::string label = {});

// Multiplies the scale of every listed object by `factors` (1 = unchanged).
// Factors are clamped away from zero so an object can never scale to a
// degenerate point it would become unclickable.
[[nodiscard]] bool scaleObjects(smg::StageArchive& stage, UndoStack& stack,
                                const std::vector<std::size_t>& objectIndices,
                                const math::Vec3f& factors, std::string label = {});

} // namespace whitehole::edit
