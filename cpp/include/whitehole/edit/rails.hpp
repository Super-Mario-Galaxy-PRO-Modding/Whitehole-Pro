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
#include "whitehole/smg/path.hpp"
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

// Adds a point WITH its bezier handles, not just its position.
//
// WHY THIS EXISTS ALONGSIDE addPathPoint(): addPathPoint() writes pnt0, pnt1
// and pnt2 all at the same position, which is right for a hand-placed point (no
// handles) but silently DESTROYS a curve's shape when the handles matter. An arc
// generated through addPathPoint() would come back as a polygon of straight
// lines -- every handle collapsed onto its point -- and look correct in the point
// list while the viewport drew straight segments between them.
//
// `point.args` is copied verbatim; pass a PathPoint whose args are -1 to inherit
// addPathPoint()'s default speed. Same schema handling and id renumbering.
[[nodiscard]] std::optional<std::size_t> addPathPointWithHandles(
    smg::StageArchive& stage, UndoStack& stack, std::size_t pointTableIndex,
    const smg::PathPoint& point, std::optional<std::size_t> insertAt = std::nullopt,
    std::string label = {});

// Creates a rail from an arc spec: the rail row, its points table, and every
// generated point, ALL AS ONE UNDO STEP. Returns the new CommonPathInfo row
// index, or nullopt when the stage has no usable path schema or the spec
// produces no geometry (a zero sweep, a non-positive radius).
//
// WHY ONE UNDO STEP: an arc is 8-64 points. createPath + a loop of
// addPathPoint would put one entry per point on the stack, so a single Ctrl+Z
// would peel off one point at a time and leave a broken rail behind -- the
// author would have to hold Z until the rail vanished. The whole rail is
// therefore wrapped in beginGroup()/endGroup(), which is what those two helpers
// exist for.
//
// The rail is marked CLOSE only when the spec describes a whole turn, matching
// what circlePoints() emits; a partial arc stays OPEN so its last section does
// not wrap back to its first.
[[nodiscard]] std::optional<std::size_t> createArcPath(smg::StageArchive& stage, UndoStack& stack,
                                                       const smg::ArcSpec& spec,
                                                       std::string label = {});

} // namespace whitehole::edit