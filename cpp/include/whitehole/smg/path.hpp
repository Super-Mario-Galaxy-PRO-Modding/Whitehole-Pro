#pragma once

// Zone rail ("path") data: the native port of Java's PathObj / PathPointObj /
// RailUtil trio. A path row lives in /Stage/jmp/Path/CommonPathInfo (loaded by
// StageArchive as kind "path"); the row's `no` field selects its points file
// /Stage/jmp/Path/CommonPathPointInfo.<no> (loaded as kind "pathpoint").
// Neither kind ever appears in StageArchive::objects() -- like Java, which
// keeps paths in their own list beside the placement objects.
//
// Everything here is a read-only projection over the stage tables plus the
// bezier maths the renderer, the properties panel and the tests all share.

#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace whitehole::smg {

// Sentinel for "no point table was loaded" (missing file, or no `no` row).
inline constexpr std::size_t kNoPointTable = static_cast<std::size_t>(-1);

// One bezier control point. Field names follow the JMap schema: pnt0 is the
// point itself, pnt1/pnt2 are the incoming/outgoing handles.
struct PathPoint {
    std::int16_t id{0};
    // Row this point came from in its points table (file order). The id sort
    // below reorders `points`, so editors must address rows through this.
    std::size_t rowIndex{0};
    math::Vec3f position;                 // pnt0
    math::Vec3f control1;                 // pnt1
    math::Vec3f control2;                 // pnt2
    std::array<std::int32_t, 8> args{};   // point_arg0..7 (0 = speed, 5 = wait time)
};

// One rail. References its stage rows so editors can write changes back.
struct RailPath {
    std::size_t tableIndex{0};  // CommonPathInfo table inside StageArchive::tables()
    std::size_t rowIndex{0};    // row inside that table
    std::string name;
    std::string type{"Bezier"};
    bool closed{false};         // "CLOSE" vs "OPEN"
    std::int32_t lId{-1};
    std::array<std::int32_t, 8> pathArgs{}; // path_arg0..7
    std::string usage{"General"};
    std::int32_t no{0};         // selects CommonPathPointInfo.<no>
    std::int32_t pathId{-1};    // Path_ID: the linked path, -1 for none
    std::size_t pointTableIndex{kNoPointTable}; // loaded point table, if any
    std::vector<PathPoint> points;             // sorted by id, like Java's POINT_SORTER

    // Display name used by lists and labels: "[l_id] name" (Java toString()).
    [[nodiscard]] std::string label() const;
};

// Convenience alias used by the editor UI and viewport layer.
using Path = RailPath;

// The points file for a path's `no` index. Archive lookups are case-insensitive.
[[nodiscard]] std::string pathPointFile(std::int32_t no);

// Stage table index holding CommonPathPointInfo.<no>, or kNoPointTable.
[[nodiscard]] std::size_t pathPointTableIndex(const StageArchive& stage, std::int32_t no);

// All points of a point table, sorted by id.
[[nodiscard]] std::vector<PathPoint> readPathPoints(const BcsvTable& table);

// Every path of a stage with its points attached. A missing points file is
// not an error: the path simply has no points, exactly like Java, which logs
// the failure and clears the list. Never throws.
[[nodiscard]] std::vector<RailPath> loadPaths(const StageArchive& stage);

// Ensures the CommonPathPointInfo schema (id + pnt0/1/2 xyz + point_arg0..7)
// so a brand-new points file can be created from scratch.
void ensurePathPointSchema(BcsvTable& table);

// ---- bezier maths (port of RailUtil; pure and unit-testable) --------------

// Point on the cubic through a->b->c->d at parameter t in [0, 1].
[[nodiscard]] math::Vec3f bezierPoint(float t, const math::Vec3f& a, const math::Vec3f& b,
                                      const math::Vec3f& c, const math::Vec3f& d) noexcept;

// Arc length of the bezier section current -> next, sampled the way Java's
// RailUtil does (one unit of interpolation per unit of chord length).
[[nodiscard]] double pathSectionLength(const PathPoint& current, const PathPoint& next) noexcept;

// Total arc length; a closed path includes the wrap section back to point 0.
[[nodiscard]] double pathLength(const std::vector<PathPoint>& points, bool closed) noexcept;

// Position at `coord` world units along the path. nullopt when the coordinate
// is past the end of an open path (Java returned null there).
[[nodiscard]] std::optional<math::Vec3f> posAtCoord(double coord, const std::vector<PathPoint>& points,
                                                    bool closed) noexcept;

// Unit tangent at `coord`, falling back to a secant estimate where the
// derivative degenerates (handles collapsed onto their point).
[[nodiscard]] std::optional<math::Vec3f> dirAtCoord(double coord, const std::vector<PathPoint>& points,
                                                    bool closed) noexcept;

// Reverses points [first, last] inclusive, swapping each point's handles as it
// moves (Java RailUtil.reversePath). Returns false for out-of-range indices.
bool reversePoints(std::vector<PathPoint>& points, int first, int last) noexcept;

} // namespace whitehole::smg