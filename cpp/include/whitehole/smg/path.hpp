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

// ---- circular arc generation (pure maths) ---------------------------------
//
// WHY THIS EXISTS: authoring a round rail by hand means placing points and
// guessing handles until the curve looks circular. The result is only visually
// round, and the error is worst where it is most visible -- a big circle is
// obviously polygonal at any segment count a person would place by hand.
//
// The fix is the exact circular-arc bezier: a cubic segment spanning angle
// theta is a true arc when its two handles sit at distance
//
//     kappa = (4/3) * tan(theta / 4)
//
// from the point, along the tangent. Note the /4, not the /8 that looks just as
// plausible. For a quarter circle (theta = 90 degrees) this yields the textbook
// 0.5522847 * radius; /8 yields 0.2652 * radius, about half, and the rail visibly
// sags inside the circle. The construction is exact for any theta, so a circle
// built from 4 segments and one built from 64 are both genuinely round rather than
// merely close.

// Which plane a generated arc lies in: the normal axis of that plane.
enum class ArcAxis : std::uint8_t { X, Y, Z };

struct ArcSpec {
    math::Vec3f center{};
    float radius{100.0F};
    // Start angle in DEGREES, measured from the in-plane +X direction and
    // increasing anticlockwise about the plane normal. Degrees because that is
    // what an author types; the maths converts once, here.
    float startDegrees{0.0F};
    // How far to sweep, in degrees. Clamped to (0, 360]; 360 produces a closed
    // circle. Negative sweeps are normalised rather than rejected, so "clockwise
    // half turn" is -180 and not an error the caller has to pre-screen.
    float sweepDegrees{360.0F};
    // Cubic bezier segments. More segments means a longer point list, NOT a
    // rounder curve -- every segment is already an exact arc. It exists to let
    // an author trade path precision (how smoothly a moving object turns) for
    // point count. Clamped to at least 1.
    int segments{8};
    ArcAxis axis{ArcAxis::Y};
};

// Points tracing the arc, INCLUDING both endpoints.
//
// HANDLE CONVENTION, which is the easy thing to get backwards: a section from
// A to B is evaluated as bezier(t, A.position, A.control2, B.control1,
// B.position) -- so `control2` (pnt2) leaves A and `control1` (pnt1) arrives at
// B. The incoming handle of a point is therefore the one pointing BACK along the
// curve, and the outgoing one points FORWARD. arcPoints() honours that; a
// generator that filled both handles with the same tangent direction produces
// rails with a visible kink at every point.
//
// Returns an empty vector (never a partial arc) for a non-positive radius or a
// sweep that normalises to nothing. Never throws and never emits NaN: callers
// wire these straight into BCSV floats, where one NaN would silently poison a
// saved zone.
[[nodiscard]] std::vector<PathPoint> arcPoints(const ArcSpec& spec);

// Same arc, but as a CLOSED circle: the duplicate endpoint is dropped so the
// last section wraps back to the first. Only meaningful at 360 degrees; for any
// smaller sweep it returns exactly what arcPoints() would, since dropping the
// final point of a partial arc would throw away real geometry.
[[nodiscard]] std::vector<PathPoint> circlePoints(const ArcSpec& spec);

// True when the spec describes a full turn, i.e. its circlePoints() form would
// drop the duplicate endpoint. Callers use it to decide the rail's CLOSE/OPEN
// flag without duplicating the clamping rules.
[[nodiscard]] bool isFullCircle(const ArcSpec& spec) noexcept;

} // namespace whitehole::smg