#include "whitehole/smg/path.hpp"

#include "whitehole/util/text.hpp"

#include <algorithm>
#include <cmath>

namespace whitehole::smg {
namespace {

// Reads pnt<set>_x/y/z as one vector.
math::Vec3f readPointVector(const BcsvTable& table, const BcsvRow& row, const char* set) {
    return {table.getFloat(row, std::string(set) + "_x"),
            table.getFloat(row, std::string(set) + "_y"),
            table.getFloat(row, std::string(set) + "_z")};
}

} // namespace

std::string RailPath::label() const {
    return "[" + std::to_string(lId) + "] " + name;
}

std::string pathPointFile(std::int32_t no) {
    return "/Stage/jmp/Path/CommonPathPointInfo." + std::to_string(no);
}

std::size_t pathPointTableIndex(const StageArchive& stage, std::int32_t no) {
    const auto wanted = pathPointFile(no);
    const auto& tables = stage.tables();
    for (std::size_t index = 0; index < tables.size(); ++index) {
        if (tables[index].kind != "pathpoint") {
            continue;
        }
        if (util::equalIgnoreCase(tables[index].path, wanted)) {
            return index;
        }
    }
    return kNoPointTable;
}

std::vector<PathPoint> readPathPoints(const BcsvTable& table) {
    std::vector<PathPoint> points;
    points.reserve(table.rows().size());
    std::size_t rowIndex = 0;
    for (const auto& row : table.rows()) {
        PathPoint point;
        point.rowIndex = rowIndex++;
        point.id = static_cast<std::int16_t>(table.getInt(row, "id", 0));
        point.position = readPointVector(table, row, "pnt0");
        point.control1 = readPointVector(table, row, "pnt1");
        point.control2 = readPointVector(table, row, "pnt2");
        for (std::size_t index = 0; index < point.args.size(); ++index) {
            point.args[index] = table.getInt(row, "point_arg" + std::to_string(index), -1);
        }
        points.push_back(point);
    }
    // Java sorted each points file by its short id before building the list;
    // the order in the file itself is not guaranteed to match draw order.
    std::stable_sort(points.begin(), points.end(),
                     [](const PathPoint& left, const PathPoint& right) { return left.id < right.id; });
    return points;
}

std::vector<RailPath> loadPaths(const StageArchive& stage) {
    std::vector<RailPath> paths;
    const auto& tables = stage.tables();
    for (std::size_t tableIndex = 0; tableIndex < tables.size(); ++tableIndex) {
        const auto& table = tables[tableIndex];
        if (table.kind != "path") {
            continue;
        }
        for (std::size_t rowIndex = 0; rowIndex < table.table.rows().size(); ++rowIndex) {
            const auto& row = table.table.rows()[rowIndex];
            RailPath path;
            path.tableIndex = tableIndex;
            path.rowIndex = rowIndex;
            path.name = table.table.getString(row, "name");
            path.type = table.table.getString(row, "type", "Bezier");
            path.closed = table.table.getString(row, "closed") == "CLOSE";
            path.lId = table.table.getInt(row, "l_id", -1);
            for (std::size_t index = 0; index < path.pathArgs.size(); ++index) {
                path.pathArgs[index] = table.table.getInt(row, "path_arg" + std::to_string(index), -1);
            }
            path.usage = table.table.getString(row, "usage", "General");
            path.no = table.table.getInt(row, "no", 0);
            path.pathId = table.table.getInt(row, "Path_ID", -1);

            path.pointTableIndex = pathPointTableIndex(stage, path.no);
            if (path.pointTableIndex != kNoPointTable) {
                path.points = readPathPoints(tables[path.pointTableIndex].table);
            }
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

void ensurePathPointSchema(BcsvTable& table) {
    table.ensureField("id", BcsvType::shortInteger);
    for (const char* set : {"pnt0", "pnt1", "pnt2"}) {
        table.ensureField(std::string(set) + "_x", BcsvType::floatingPoint);
        table.ensureField(std::string(set) + "_y", BcsvType::floatingPoint);
        table.ensureField(std::string(set) + "_z", BcsvType::floatingPoint);
    }
    for (std::size_t index = 0; index < 8; ++index) {
        table.ensureField("point_arg" + std::to_string(index), BcsvType::integer);
    }
}


// ---- bezier maths ---------------------------------------------------------

math::Vec3f bezierPoint(float t, const math::Vec3f& a, const math::Vec3f& b, const math::Vec3f& c,
                        const math::Vec3f& d) noexcept {
    const auto t2 = t * t;
    const auto t3 = t2 * t;
    const auto inv = 1.0F - t;
    const auto inv2 = inv * inv;
    const auto w0 = inv2 * inv;
    const auto w1 = 3.0F * inv2 * t;
    const auto w2 = 3.0F * inv * t2;
    const auto w3 = t3;
    return {a.x * w0 + b.x * w1 + c.x * w2 + d.x * w3,
            a.y * w0 + b.y * w1 + c.y * w2 + d.y * w3,
            a.z * w0 + b.z * w1 + c.z * w2 + d.z * w3};
}

double pathSectionLength(const PathPoint& current, const PathPoint& next) noexcept {
    const auto chord = (next.position - current.position).length();
    if (chord < 0.000001F) {
        return 0.0;
    }
    // Java: dt = LineInterpolationPrecision / |D - A| with precision 1.
    const double dt = 1.0 / static_cast<double>(chord);
    double length = 0.0;
    math::Vec3f previous = current.position;
    for (double t = dt; t < 1.0; t += dt) {
        const auto sample = bezierPoint(static_cast<float>(t), current.position, current.control2,
                                        next.control1, next.position);
        length += static_cast<double>((sample - previous).length());
        previous = sample;
    }
    const auto tail = bezierPoint(1.0F, current.position, current.control2, next.control1, next.position);
    length += static_cast<double>((tail - previous).length());
    return length;
}

double pathLength(const std::vector<PathPoint>& points, bool closed) noexcept {
    if (points.empty()) {
        return 0.0;
    }
    const std::size_t sections = points.size() - 1 + (closed && points.size() > 1 ? 1 : 0);
    double total = 0.0;
    for (std::size_t index = 0; index < sections; ++index) {
        const auto& next = (index + 1 < points.size()) ? points[index + 1] : points[0];
        total += pathSectionLength(points[index], next);
    }
    return total;
}

std::optional<math::Vec3f> posAtCoord(double coord, const std::vector<PathPoint>& points,
                                      bool closed) noexcept {
    if (points.empty()) {
        return math::Vec3f{};
    }
    if (coord < 0.0) {
        return points.front().position;
    }
    const std::size_t sections = points.size() - 1 + (closed && points.size() > 1 ? 1 : 0);
    double accumulated = 0.0;
    for (std::size_t index = 0; index < sections; ++index) {
        const auto& current = points[index];
        const auto& next = (index + 1 < points.size()) ? points[index + 1] : points[0];
        const double section = pathSectionLength(current, next);
        const double previous = accumulated;
        accumulated += section;
        if (coord > accumulated) {
            continue;
        }
        // A degenerate section collapses to its start point instead of Java's
        // division by zero.
        const double t = section < 0.0000001 ? 0.0 : (coord - previous) / section;
        return bezierPoint(static_cast<float>(std::clamp(t, 0.0, 1.0)), current.position,
                           current.control2, next.control1, next.position);
    }
    return std::nullopt; // past the end of an open path
}


std::optional<math::Vec3f> dirAtCoord(double coord, const std::vector<PathPoint>& points,
                                      bool closed) noexcept {
    if (points.empty()) {
        return math::Vec3f{};
    }
    const std::size_t sections = points.size() - 1 + (closed && points.size() > 1 ? 1 : 0);
    double accumulated = 0.0;
    for (std::size_t index = 0; index < sections; ++index) {
        const auto& current = points[index];
        const auto& next = (index + 1 < points.size()) ? points[index + 1] : points[0];
        const double section = pathSectionLength(current, next);
        const double previous = accumulated;
        accumulated += section;
        if (coord > accumulated) {
            continue;
        }
        const double t = std::clamp(section < 0.0000001 ? 0.0 : (coord - previous) / section, 0.0, 1.0);
        const double inv = 1.0 - t;
        const auto& a = current.position;
        const auto& b = current.control2;
        const auto& c = next.control1;
        const auto& d = next.position;
        math::Vec3f derivative{
            static_cast<float>(3.0 * inv * inv * (b.x - a.x) + 6.0 * inv * t * (c.x - b.x) +
                               3.0 * t * t * (d.x - c.x)),
            static_cast<float>(3.0 * inv * inv * (b.y - a.y) + 6.0 * inv * t * (c.y - b.y) +
                               3.0 * t * t * (d.y - c.y)),
            static_cast<float>(3.0 * inv * inv * (b.z - a.z) + 6.0 * inv * t * (c.z - b.z) +
                               3.0 * t * t * (d.z - c.z))};
        if (math::Vec3f::dot(derivative, derivative) > 0.0001F) {
            return derivative.normalized();
        }

        // Handles collapsed onto the point: estimate the tangent with a secant
        // around the coordinate, exactly like Java's fallback.
        const double total = pathLength(points, closed);
        double delta = std::max(10.0, section * 0.02);
        delta = std::min(delta, total * 0.1);
        const auto before = posAtCoord(coord - delta, points, closed);
        const auto after = posAtCoord(coord + delta, points, closed);
        if (!before.has_value() || !after.has_value()) {
            return math::Vec3f{};
        }
        const auto secant = *after - *before;
        if (math::Vec3f::dot(secant, secant) <= 0.0001F) {
            return math::Vec3f{};
        }
        return secant.normalized();
    }
    return std::nullopt;
}

bool reversePoints(std::vector<PathPoint>& points, int first, int last) noexcept {
    const int count = static_cast<int>(points.size());
    if (first < 0 || first >= count || last < 0 || last >= count) {
        return false;
    }
    if (first == last) {
        return true;
    }
    if (first > last) {
        std::swap(first, last);
    }
    std::vector<PathPoint> captured;
    captured.reserve(static_cast<std::size_t>(last - first + 1));
    for (int index = first; index <= last; ++index) {
        captured.push_back(points[static_cast<std::size_t>(index)]);
    }
    int target = last;
    for (std::size_t index = 0; index < captured.size(); ++index) {
        auto moved = captured[index];
        // Moving backwards flips the curve direction, so the handles swap too.
        std::swap(moved.control1, moved.control2);
        points[static_cast<std::size_t>(target)] = moved;
        --target;
    }
    return true;
}

} // namespace whitehole::smg
