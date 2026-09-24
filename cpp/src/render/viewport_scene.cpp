#include "whitehole/render/viewport_scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace whitehole::render {
namespace {

// Placement stores rotation in degrees (dir_x/dir_y/dir_z). SMG convention:
// dir_x rotates about the Z axis, dir_y about Y and dir_z about X, applied in
// that order — identical to the Java editor's object renderer.
constexpr float kDegreesToRadians = 3.141592653589793F / 180.0F;

// Inverse of the placement matrix. The placement matrix is (in this codebase's
// row-major storage, where transformPoint() multiplies as p * M):
//     W = scale * rotateX * rotateY * rotateZ * translate = S * R * T
// which has no shear, so W^-1 = R^T * S^-1 * T^-1 and every entry can be
// written down directly.
//
// With A = S * R the linear part, row i of A is scale[i] * row i of R, so
//     A(i, j) = world.values[4 * i + j]
// and the inverse linear part A^-1 = R^T * S^-1 has entries
//     A^-1(i, j) = R(j, i) / scale[j] = world.values[4 * j + i] / scale[j]^2.
// The divisor must come from the entry's own COLUMN (j): using the row scale
// (i) instead shears the inverse, which made the slab test in
// rayIntersectsBox() report phantom hits and missed hits for rotated objects
// that are scaled non-uniformly.
math::Matrix4 placementWorldInverse(const math::Matrix4& world, const math::Vec3f& scale) noexcept {
    math::Matrix4 inverse;
    inverse.values.fill(0.0F);
    const float safeX = std::abs(scale.x) > 0.000001F ? scale.x : 1.0F;
    const float safeY = std::abs(scale.y) > 0.000001F ? scale.y : 1.0F;
    const float safeZ = std::abs(scale.z) > 0.000001F ? scale.z : 1.0F;
    // Row 0: A^-1(0, j) = world(j, 0) / scale[j]^2.
    inverse.values[0] = world.values[0] / (safeX * safeX);
    inverse.values[1] = world.values[4] / (safeY * safeY);
    inverse.values[2] = world.values[8] / (safeZ * safeZ);
    // Row 1: A^-1(1, j) = world(j, 1) / scale[j]^2.
    inverse.values[4] = world.values[1] / (safeX * safeX);
    inverse.values[5] = world.values[5] / (safeY * safeY);
    inverse.values[6] = world.values[9] / (safeZ * safeZ);
    // Row 2: A^-1(2, j) = world(j, 2) / scale[j]^2.
    inverse.values[8] = world.values[2] / (safeX * safeX);
    inverse.values[9] = world.values[6] / (safeY * safeY);
    inverse.values[10] = world.values[10] / (safeZ * safeZ);
    inverse.values[15] = 1.0F;
    // Last row: -t * A^-1 for the row-vector translation t (world row 3).
    const math::Vec3f translation{world.values[12], world.values[13], world.values[14]};
    inverse.values[12] =
        -(inverse.values[0] * translation.x + inverse.values[4] * translation.y + inverse.values[8] * translation.z);
    inverse.values[13] =
        -(inverse.values[1] * translation.x + inverse.values[5] * translation.y + inverse.values[9] * translation.z);
    inverse.values[14] =
        -(inverse.values[2] * translation.x + inverse.values[6] * translation.y + inverse.values[10] * translation.z);
    return inverse;
}

math::Matrix4 placementRotation(const smg::PlacementObject& object) noexcept {
    return math::Matrix4::rotationZ(object.rotation.x * kDegreesToRadians) *
           math::Matrix4::rotationY(object.rotation.y * kDegreesToRadians) *
           math::Matrix4::rotationX(object.rotation.z * kDegreesToRadians);
}

} // namespace

// Stable per-rail colour (see header). The hue walks the golden ratio from
// l_id and every channel is lifted off zero so even dark hues stay visible
// against the grid.
std::uint32_t railPathColor(std::int32_t lId) noexcept {
    float hue = std::fmod(static_cast<float>(lId) * 0.61803398875F, 1.0F);
    if (hue < 0.0F) {
        hue += 1.0F;
    }
    const float scaled = hue * 6.0F;
    const int sector = static_cast<int>(scaled) % 6;
    const float fraction = scaled - static_cast<int>(scaled);
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    switch (sector) {
        case 0: red = 1.0F; green = fraction; break;
        case 1: red = 1.0F - fraction; green = 1.0F; break;
        case 2: green = 1.0F; blue = fraction; break;
        case 3: green = 1.0F - fraction; blue = 1.0F; break;
        case 4: red = fraction; blue = 1.0F; break;
        default: red = 1.0F; blue = 1.0F - fraction; break;
    }
    const auto channel = [](float value) {
        const auto lifted = std::clamp(0.40F + value * 0.60F, 0.0F, 1.0F);
        return static_cast<std::uint32_t>(lifted * 255.0F + 0.5F);
    };
    return (channel(red) << 24) | (channel(green) << 16) | (channel(blue) << 8) | 0xFFu;
}

namespace {

OverlayBatch makeBatch(std::uint32_t color, float width = 1.0F) {
    OverlayBatch batch;
    batch.color = color;
    batch.width = width;
    return batch;
}

// Axis-aligned wire box pushed through a world matrix (12 edges).
void appendBoxWire(OverlayBatch& batch, const math::Matrix4& world, const math::Vec3f& half) {
    math::Vec3f corners[8];
    for (int index = 0; index < 8; ++index) {
        corners[index] = world.transformPoint({(index & 1) != 0 ? half.x : -half.x,
                                               (index & 2) != 0 ? half.y : -half.y,
                                               (index & 4) != 0 ? half.z : -half.z});
    }
    constexpr int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                  {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : edges) {
        batch.segments.push_back({corners[edge[0]], corners[edge[1]]});
    }
}

// Small wire cube around a point: Java drew every rail point as a CubeRenderer
// box (size 100 for the point, 50 for the handles).
void appendCubeAt(OverlayBatch& batch, const math::Vec3f& center, float half) {
    appendBoxWire(batch, math::Matrix4::translation(center), {half, half, half});
}

bool coincident(const math::Vec3f& left, const math::Vec3f& right) noexcept {
    return (left - right).length() <= 0.001F;
}

// The rail polyline: straight sections collapse to one segment (Java's
// roughlyEqual shortcut), curved sections tessellate at Java's 0.01 step.
void appendRailCurve(OverlayBatch& batch, const smg::RailPath& path) {
    const auto& points = path.points;
    if (points.size() < 2) {
        return;
    }
    const std::size_t sections = points.size() - 1 + (path.closed ? 1 : 0);
    for (std::size_t section = 0; section < sections; ++section) {
        const auto& current = points[section];
        const auto& next = (section + 1 < points.size()) ? points[section + 1] : points[0];
        const auto& a = current.position;
        const auto& b = current.control2;
        const auto& c = next.control1;
        const auto& d = next.position;
        if (coincident(a, b) && coincident(c, d)) {
            batch.segments.push_back({a, d});
            continue;
        }
        math::Vec3f previous = a;
        constexpr int kSteps = 100;
        for (int step = 1; step < kSteps; ++step) {
            const auto sample = smg::bezierPoint(static_cast<float>(step) / kSteps, a, b, c, d);
            batch.segments.push_back({previous, sample});
            previous = sample;
        }
        batch.segments.push_back({previous, d});
    }
}

// Control handles and point cubes for one rail.
void appendRailPoints(OverlayBatch& batch, const smg::RailPath& path) {
    for (const auto& point : path.points) {
        if (!coincident(point.position, point.control1)) {
            batch.segments.push_back({point.position, point.control1});
        }
        if (!coincident(point.position, point.control2)) {
            batch.segments.push_back({point.position, point.control2});
        }
        appendCubeAt(batch, point.position, 50.0F);
        appendCubeAt(batch, point.control1, 25.0F);
        appendCubeAt(batch, point.control2, 25.0F);
    }
}

// Placement transform with a visibility floor: hair-thin area cubes (scale
// 0.001) would otherwise be impossible to find on the map.
math::Matrix4 overlayWorld(const smg::PlacementObject& object) noexcept {
    constexpr float kMinimumExtent = 15.0F;
    const auto safe = [](float value) {
        const auto magnitude = std::abs(value);
        if (magnitude > kMinimumExtent) {
            return value < 0.0F ? -magnitude : magnitude;
        }
        return kMinimumExtent;
    };
    return math::Matrix4::scale({safe(object.scale.x), safe(object.scale.y), safe(object.scale.z)}) *
           placementRotation(object) * math::Matrix4::translation(object.position);
}

// Generates every enabled overlay family. Runs after the box pass so the
// axis can be sized from frameDistance_.
void buildOverlays(const std::vector<smg::PlacementObject>& objects, OverlayFlags flags,
                   const std::vector<smg::RailPath>& paths, float frameDistance,
                   std::vector<OverlayBatch>& out) {
    if (flags.axis) {
        const float length = std::clamp(frameDistance, 300.0F, 5000.0F);
        const math::Vec3f origin{};
        auto xAxis = makeBatch(0xE05050FFu, 1.5F);
        xAxis.segments.push_back({origin, {length, 0.0F, 0.0F}});
        auto yAxis = makeBatch(0x50D060FFu, 1.5F);
        yAxis.segments.push_back({origin, {0.0F, length, 0.0F}});
        auto zAxis = makeBatch(0x5080E0FFu, 1.5F);
        zAxis.segments.push_back({origin, {0.0F, 0.0F, length}});
        out.push_back(std::move(xAxis));
        out.push_back(std::move(yAxis));
        out.push_back(std::move(zAxis));
    }

    const auto collect = [&objects, &out](bool enabled, const char* kind, std::uint32_t color) {
        if (!enabled) {
            return;
        }
        auto batch = makeBatch(color);
        for (const auto& object : objects) {
            if (object.kind == kind) {
                appendBoxWire(batch, overlayWorld(object), {1.0F, 1.0F, 1.0F});
            }
        }
        if (!batch.segments.empty()) {
            out.push_back(std::move(batch));
        }
    };
    collect(flags.cameras, "camera", 0x58C8E8FFu); // cyan
    collect(flags.areas, "area", 0xD060C0FFu);     // magenta
    collect(flags.gravity, "gravity", 0x70E090FFu); // green

    if (flags.paths) {
        for (std::size_t pathIndex = 0; pathIndex < paths.size(); ++pathIndex) {
            const auto& path = paths[pathIndex];
            if (path.points.empty()) {
                continue;
            }
            const auto color = railPathColor(path.lId);
            auto curve = makeBatch(color, 1.5F);
            curve.railIndex = pathIndex;
            appendRailCurve(curve, path);
            if (!curve.segments.empty()) {
                out.push_back(std::move(curve));
            }
            auto handles = makeBatch(color);
            handles.railIndex = pathIndex;
            appendRailPoints(handles, path);
            if (!handles.segments.empty()) {
                out.push_back(std::move(handles));
            }
        }
    }
}

} // namespace

math::Matrix4 placementWorldMatrix(const smg::PlacementObject& object) noexcept {
    const math::Vec3f safeScale{std::abs(object.scale.x) > 0.000001F ? object.scale.x : 1.0F,
                                std::abs(object.scale.y) > 0.000001F ? object.scale.y : 1.0F,
                                std::abs(object.scale.z) > 0.000001F ? object.scale.z : 1.0F};
    // kMinVisualScale keeps micro-scaled objects visible and clickable; it is
    // a rendering/picking clamp only and never touches the stored level data.
    const float visualX = std::max(std::abs(safeScale.x), kMinVisualScale);
    const float visualY = std::max(std::abs(safeScale.y), kMinVisualScale);
    const float visualZ = std::max(std::abs(safeScale.z), kMinVisualScale);
    const math::Matrix4 scaled =
        math::Matrix4::scale({visualX * kPlaceholderHalfExtent, visualY * kPlaceholderHalfExtent, visualZ * kPlaceholderHalfExtent});
    const math::Matrix4 rotation = placementRotation(object);
    return math::Matrix4::translation(object.position) * rotation * scaled;
}

math::Matrix4 objectWorldMatrix(const smg::PlacementObject& object) noexcept {
    const math::Vec3f safeScale{std::abs(object.scale.x) > 0.000001F ? object.scale.x : 1.0F,
                                std::abs(object.scale.y) > 0.000001F ? object.scale.y : 1.0F,
                                std::abs(object.scale.z) > 0.000001F ? object.scale.z : 1.0F};
    const math::Matrix4 rotation = placementRotation(object);
    return math::Matrix4::translation(object.position) * rotation * math::Matrix4::scale(safeScale);
}

math::Matrix4 objectPickMatrix(const smg::PlacementObject& object, float radius) noexcept {
    // Wrapping the model's bounding sphere in the same placement transform
    // gives picking the oriented-box maths it already has, with the unit box
    // acting as the sphere's proxy.
    return objectWorldMatrix(object) * math::Matrix4::scale({radius, radius, radius});
}

float modelPickRadius(const ModelMesh& mesh) noexcept {
    // Max corner length of the bounding box: the tightest origin-centred
    // sphere that contains the whole mesh even when it is built off-centre.
    float radius = 0.0F;
    if (!mesh.empty()) {
        const float x[2] = {mesh.boundsMin.x, mesh.boundsMax.x};
        const float y[2] = {mesh.boundsMin.y, mesh.boundsMax.y};
        const float z[2] = {mesh.boundsMin.z, mesh.boundsMax.z};
        for (const float cornerX : x) {
            for (const float cornerY : y) {
                for (const float cornerZ : z) {
                    radius = std::max(radius, math::Vec3f{cornerX, cornerY, cornerZ}.length());
                }
            }
        }
    }
    // Degenerate/point models still need to be clickable.
    return std::max(radius, 1.0F);
}

std::optional<float> rayIntersectsBox(const Ray& ray, const ViewportBox& box) noexcept {
    // The linear part of the picking matrix is S * R, so its row i is exactly
    // scale[i] * (row i of the rotation). Row lengths therefore recover the
    // per-axis world scale (including the placeholder half extent or a model's
    // pick radius) with no approximation, which is what placementWorldInverse()
    // expects.
    const math::Matrix4& world = box.pickWorld;
    const math::Vec3f scale{math::Vec3f{world.values[0], world.values[1], world.values[2]}.length(),
                            math::Vec3f{world.values[4], world.values[5], world.values[6]}.length(),
                            math::Vec3f{world.values[8], world.values[9], world.values[10]}.length()};
    const math::Matrix4 inverse = placementWorldInverse(world, scale);
    const math::Vec3f localOrigin = inverse.transformPoint(ray.origin);
    const math::Vec3f localDirection{ray.direction.x * inverse.values[0] + ray.direction.y * inverse.values[4] +
                                         ray.direction.z * inverse.values[8],
                                     ray.direction.x * inverse.values[1] + ray.direction.y * inverse.values[5] +
                                         ray.direction.z * inverse.values[9],
                                     ray.direction.x * inverse.values[2] + ray.direction.y * inverse.values[6] +
                                         ray.direction.z * inverse.values[10]};

    // Slab test against unit box [-1,1]^3 (extents baked into world matrix).
    float tMin = 0.0F;
    float tMax = std::numeric_limits<float>::infinity();
    const float origins[3] = {localOrigin.x, localOrigin.y, localOrigin.z};
    const float directions[3] = {localDirection.x, localDirection.y, localDirection.z};
    for (int axis = 0; axis < 3; ++axis) {
        const float origin = origins[axis];
        const float direction = directions[axis];
        if (std::abs(direction) < 0.0000001F) {
            if (origin < -1.0F || origin > 1.0F) {
                return std::nullopt;
            }
            continue;
        }
        float entry = (-1.0F - origin) / direction;
        float exit = (1.0F - origin) / direction;
        if (entry > exit) {
            std::swap(entry, exit);
        }
        tMin = std::max(tMin, entry);
        tMax = std::min(tMax, exit);
        if (tMin > tMax) {
            return std::nullopt;
        }
    }
    return tMin;
}

void ViewportScene::rebuild(const std::vector<smg::PlacementObject>& objects, ModelLibrary* models,
                            const std::vector<smg::RailPath>* paths, OverlayFlags overlays) {
    boxes_.clear();
    overlayBatches_.clear();
    paths_.clear();
    pathsPickable_ = overlays.paths;
    if (paths != nullptr) {
        paths_ = *paths;
    }
    boxes_.reserve(objects.size());
    math::Vec3f sum{0.0F, 0.0F, 0.0F};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto& object = objects[index];
        ViewportBox box;
        box.objectIndex = index;
        box.name = object.name;
        box.kind = object.kind;
        box.category = classifyObject(object.kind, object.name);
        box.center = object.position;

        // Real game model when the workspace provides one: drawn at the
        // object's true scale and picked against its bounding sphere. Missing
        // models keep the category placeholder box (and its visibility clamp)
        // so every object stays visible and clickable either way.
        std::shared_ptr<const ModelMesh> mesh;
        if (models != nullptr) {
            mesh = models->model(object.name);
        }
        if (mesh != nullptr && !mesh->empty()) {
            box.model = std::move(mesh);
            box.world = objectWorldMatrix(object);
            const float radius = modelPickRadius(*box.model);
            box.pickWorld = objectPickMatrix(object, radius);
            box.halfExtents = {radius * std::abs(object.scale.x > 0.000001F ? object.scale.x : 1.0F),
                               radius * std::abs(object.scale.y > 0.000001F ? object.scale.y : 1.0F),
                               radius * std::abs(object.scale.z > 0.000001F ? object.scale.z : 1.0F)};
        } else {
            box.model.reset();
            box.world = placementWorldMatrix(object);
            box.pickWorld = box.world;
            // Extents mirror the visual clamp used by the world matrix so the
            // frame-all radius and picking agree with what is drawn.
            const float extent = kPlaceholderHalfExtent *
                                 std::max({std::abs(object.scale.x) > 0.000001F ? std::abs(object.scale.x) : 1.0F,
                                           std::abs(object.scale.y) > 0.000001F ? std::abs(object.scale.y) : 1.0F,
                                           std::abs(object.scale.z) > 0.000001F ? std::abs(object.scale.z) : 1.0F,
                                           kMinVisualScale});
            box.halfExtents = {extent, extent, extent};
        }
        boxes_.push_back(box);
        sum = sum + object.position;
    }
    if (!boxes_.empty()) {
        const float count = static_cast<float>(boxes_.size());
        center_ = {sum.x / count, sum.y / count, sum.z / count};
        float maxRadius = 0.0F;
        for (const auto& box : boxes_) {
            maxRadius = std::max(maxRadius, (box.center - center_).length() + box.halfExtents.length());
        }
        frameDistance_ = std::clamp(maxRadius * 1.6F, 200.0F, 12000.0F);
    } else {
        center_ = {};
        frameDistance_ = 800.0F;
    }
    buildOverlays(objects, overlays, paths_, frameDistance_, overlayBatches_);
}

void ViewportScene::clear() noexcept {
    boxes_.clear();
    overlayBatches_.clear();
    paths_.clear();
    pathsPickable_ = false;
    center_ = {};
    frameDistance_ = 800.0F;
}

std::optional<RailPointRef> ViewportScene::pickRailPoint(const ViewportCamera& camera, float screenX,
                                                         float screenY, float width, float height,
                                                         float maxDistance) const noexcept {
    if (!pathsPickable_ || width <= 0.0F || height <= 0.0F) {
        return std::nullopt;
    }
    const auto ray = camera.screenToRay(screenX, screenY, width, height);
    // Cube half-sizes from the overlay pass (50 for a point, 25 for a handle),
    // padded slightly so the pick target is at least as friendly as it looks.
    struct Part {
        const math::Vec3f* center;
        float radius;
        int part;
    };
    std::optional<RailPointRef> best;
    float bestDistance = maxDistance;
    for (std::size_t pathIndex = 0; pathIndex < paths_.size(); ++pathIndex) {
        const auto& points = paths_[pathIndex].points;
        for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            const auto& point = points[pointIndex];
            const Part parts[] = {{&point.position, 70.0F, 0},
                                  {&point.control1, 45.0F, 1},
                                  {&point.control2, 45.0F, 2}};
            for (const auto& part : parts) {
                const auto offset = ray.origin - *part.center;
                const auto projected = math::Vec3f::dot(offset, ray.direction);
                const auto closest = math::Vec3f::dot(offset, offset) -
                                     part.radius * part.radius;
                const auto discriminant = projected * projected - closest;
                if (discriminant < 0.0F) {
                    continue;
                }
                const auto root = std::sqrt(discriminant);
                float distance = -projected - root;
                if (distance < 0.0F) {
                    distance = -projected + root;
                }
                if (distance < 0.0F || distance >= bestDistance) {
                    continue;
                }
                bestDistance = distance;
                best = RailPointRef{pathIndex, pointIndex, part.part};
            }
        }
    }
    return best;
}

std::optional<std::size_t> ViewportScene::pick(const ViewportCamera& camera, float screenX, float screenY, float width,
                                               float height, float maxDistance) const noexcept {
    if (boxes_.empty() || width <= 0.0F || height <= 0.0F) {
        return std::nullopt;
    }
    const Ray ray = camera.screenToRay(screenX, screenY, width, height);
    std::optional<std::size_t> best;
    float bestDistance = maxDistance;
    for (const auto& box : boxes_) {
        const auto distance = rayIntersectsBox(ray, box);
        if (distance.has_value() && *distance < bestDistance) {
            bestDistance = *distance;
            best = box.objectIndex;
        }
    }
    return best;
}

std::optional<std::size_t> ViewportScene::pickForgiving(const ViewportCamera& camera, float screenX,
                                                        float screenY, float width, float height,
                                                        float maxDistance, float slopPx) const noexcept {
    if (auto hit = pick(camera, screenX, screenY, width, height, maxDistance)) {
        return hit;
    }
    if (boxes_.empty() || width <= 0.0F || height <= 0.0F || slopPx <= 0.0F) {
        return std::nullopt;
    }
    std::optional<std::size_t> best;
    float bestPx = slopPx;
    for (const auto& box : boxes_) {
        float px = 0.0F;
        float py = 0.0F;
        if (!camera.worldToScreen(box.center, width, height, px, py)) {
            continue;
        }
        const float dist = std::hypot(px - screenX, py - screenY);
        if (dist < bestPx) {
            bestPx = dist;
            best = box.objectIndex;
        }
    }
    return best;
}

std::vector<std::size_t> ViewportScene::pickRect(const ViewportCamera& camera, float x0, float y0, float x1,
                                                 float y1, float width, float height) const noexcept {
    std::vector<std::size_t> hits;
    if (boxes_.empty() || width <= 0.0F || height <= 0.0F) {
        return hits;
    }
    const float lowX = std::min(x0, x1);
    const float highX = std::max(x0, x1);
    const float lowY = std::min(y0, y1);
    const float highY = std::max(y0, y1);
    for (const auto& box : boxes_) {
        float px = 0.0F;
        float py = 0.0F;
        if (!camera.worldToScreen(box.center, width, height, px, py)) {
            continue;
        }
        if (px >= lowX && px <= highX && py >= lowY && py <= highY) {
            hits.push_back(box.objectIndex);
        }
    }
    return hits;
}

} // namespace whitehole::render
