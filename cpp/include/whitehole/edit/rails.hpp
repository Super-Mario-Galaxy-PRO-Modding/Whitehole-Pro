#pragma once

// Rail (path) authoring: create and delete rails, add and remove their points.
// Every operation records exactly one undo step through the shared UndoStack,
// so the desktop editor, the CLI and the tests all take the same path.
//
// Structural invariant: CommonPathInfo rows live in the stage's "path" table
// and each row's `no` selects a "pathpoint" table. createPath() registers both;
// undo removes the row and leaves the (empty) points table behind, which is
// harmless because a table without a referencing row is never drawn and only
// re-saves its own bytes.

#include "whitehole/edit/undo.hpp"
#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace whitehole::edit {

// Appends a new rail with Java's defaults (type Bezier, OPEN, usage General,
// unique l_id / no) and registers its empty points table. Returns the new
// CommonPathInfo row index, or nullopt when the stage has no usable path
// schema at all.
[[nodiscard]] std::optional<std::size_t> createPath(smg::StageArchive& stage, UndoStack& stack,
                                                    std::string label = {});

// Inserts a point (all three control vectors at `position`, args -1) at
// `insertAt` or at the end, then renumbers ids so row order and id order
// agree. Returns the new points-table row index.
[[nodiscard]] std::optional<std::size_t> addPathPoint(smg::StageArchive& stage, UndoStack& stack,
                                                      std::size_t pointTableIndex,
                                                      const math::Vec3f& position,
                                                      std::optional<std::size_t> insertAt = std::nullopt,
                                                      std::string label = {});

// Removes one point row; undo restores it in place.
[[nodiscard]] bool removePathPoint(smg::StageArchive& stage, UndoStack& stack,
                                   std::size_t pointTableIndex, std::size_t rowIndex,
                                   std::string label = {});

} // namespace whitehole::edit