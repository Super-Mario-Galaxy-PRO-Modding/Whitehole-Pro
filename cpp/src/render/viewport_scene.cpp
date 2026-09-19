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

void ViewportScene::rebuild(const std::vector<smg::PlacementObject>& objects, ModelLibrary* models) {
    boxes_.clear();
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
}

void ViewportScene::clear() noexcept {
    boxes_.clear();
    center_ = {};
    frameDistance_ = 800.0F;
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

} // namespace whitehole::render
