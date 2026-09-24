#include "whitehole/render/surface_snap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace whitehole::render {
namespace {

// Inverse of an S*R+T placement matrix given its per-axis world scales.
// Reimplemented here (rather than reusing viewport_scene.cpp's private helper)
// so this header stays dependency-free and unit-testable.
math::Matrix4 inverseST(const math::Matrix4& world, const math::Vec3f& scale) noexcept {
    math::Matrix4 inv{};
    inv.values.fill(0.0F);
    for (int col = 0; col < 3; ++col) {
        const float s = (col == 0 ? scale.x : col == 1 ? scale.y : scale.z);
        const float denom = s * s > 0.0000001F ? s * s : 1.0F;
        for (int row = 0; row < 3; ++row) {
            inv.values[col * 4 + row] = world.values[row * 4 + col] / denom;
        }
    }
    const math::Vec3f t{world.values[12], world.values[13], world.values[14]};
    inv.values[12] = -(inv.values[0] * t.x + inv.values[4] * t.y + inv.values[8] * t.z);
    inv.values[13] = -(inv.values[1] * t.x + inv.values[5] * t.y + inv.values[9] * t.z);
    inv.values[14] = -(inv.values[2] * t.x + inv.values[6] * t.y + inv.values[10] * t.z);
    inv.values[15] = 1.0F;
    return inv;
}

// Distance along -Y from `origin` to the box surface, or nullopt. Works by
// transforming the whole downward segment into the box's unit space and running
// a slab test there, so rotation and non-uniform scale are both exact.
std::optional<float> rayBoxDown(const math::Vec3f& origin, const SnapBox& box,
                                float maxDistance) noexcept {
    const math::Matrix4& w = box.pickWorld;
    const math::Vec3f scale{math::Vec3f{w.values[0], w.values[1], w.values[2]}.length(),
                            math::Vec3f{w.values[4], w.values[5], w.values[6]}.length(),
                            math::Vec3f{w.values[8], w.values[9], w.values[10]}.length()};
    const math::Matrix4 inv = inverseST(w, scale);
    const math::Vec3f lo = inv.transformPoint(origin);
    const math::Vec3f hi = inv.transformPoint({origin.x, origin.y - maxDistance, origin.z});
    const math::Vec3f seg{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};

    float tMin = 0.0F;
    float tMax = std::numeric_limits<float>::infinity();
    const float o[3] = {lo.x, lo.y, lo.z};
    const float d[3] = {seg.x, seg.y, seg.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-9F) {
            if (o[axis] < -1.0F || o[axis] > 1.0F) {
                return std::nullopt;
            }
            continue;
        }
        float e0 = (-1.0F - o[axis]) / d[axis];
        float e1 = (1.0F - o[axis]) / d[axis];
        if (e0 > e1) {
            std::swap(e0, e1);
        }
        tMin = std::max(tMin, e0);
        tMax = std::min(tMax, e1);
        if (tMin > tMax) {
            return std::nullopt;
        }
    }
    if (tMin < 0.0F || tMin > 1.0F) {
        return std::nullopt;
    }
    return tMin * maxDistance;
}

} // namespace

// Moller-Trumbore, written from scratch: two edge vectors, a determinant guard
// for parallel rays, then barycentric rejection. Returns the ray parameter t.
std::optional<float> rayIntersectsTriangle(const math::Vec3f& origin, const math::Vec3f& dir,
                                           const SnapTriangle& tri) noexcept {
    const math::Vec3f e1{tri.b.x - tri.a.x, tri.b.y - tri.a.y, tri.b.z - tri.a.z};
    const math::Vec3f e2{tri.c.x - tri.a.x, tri.c.y - tri.a.y, tri.c.z - tri.a.z};
    const math::Vec3f p = math::Vec3f::cross(dir, e2);
    const float det = math::Vec3f::dot(e1, p);
    if (std::abs(det) < 1e-9F) {
        return std::nullopt; // parallel or degenerate triangle
    }
    const float inv = 1.0F / det;
    const math::Vec3f s{origin.x - tri.a.x, origin.y - tri.a.y, origin.z - tri.a.z};
    const float u = math::Vec3f::dot(s, p) * inv;
    if (u < 0.0F || u > 1.0F) {
        return std::nullopt;
    }
    const math::Vec3f q = math::Vec3f::cross(s, e1);
    const float v = math::Vec3f::dot(dir, q) * inv;
    if (v < 0.0F || u + v > 1.0F) {
        return std::nullopt;
    }
    const float t = math::Vec3f::dot(e2, q) * inv;
    if (t < 0.0F) {
        return std::nullopt;
    }
    return t;
}

std::optional<SnapHit> raycastDown(const SnapScene& scene, const math::Vec3f& origin,
                                   float maxDistance, std::size_t ignoreIndex) noexcept {
    if (!(maxDistance > 0.0F)) {
        return std::nullopt;
    }
    const math::Vec3f down{0.0F, -1.0F, 0.0F};
    std::optional<SnapHit> best;
    float bestDist = maxDistance;

    // Exact KCL triangles first: they win over the coarse box approximation
    // whenever they are closer, which is what makes a drop land flush on a
    // sculpted planet surface instead of its bounding box.
    for (const auto& tri : scene.kcl) {
        // Owned collision remembers its object so a drop can lift an object
        // over its own roof; the sentinel guard keeps zone-level soup (and
        // the default "ignore nothing" sentinel) always hittable.
        if (ignoreIndex != kCollisionNoOwner && tri.sourceIndex == ignoreIndex) {
            continue;
        }
        const auto t = rayIntersectsTriangle(origin, down, tri);
        if (t.has_value() && *t > 0.001F && *t < bestDist) {
            bestDist = *t;
            SnapHit hit;
            hit.point = {origin.x, origin.y - *t, origin.z};
            hit.normal = tri.normal;
            hit.distance = *t;
            hit.fromKcl = true;
            best = hit;
        }
    }

    for (const auto& box : scene.boxes) {
        // Never snap an object onto itself; the caller passes its own index.
        if (box.objectIndex == ignoreIndex) {
            continue;
        }
        // A box entirely above the ray start cannot be landed on going down.
        if (box.center.y - box.halfExtents.y > origin.y) {
            continue;
        }
        const auto t = rayBoxDown(origin, box, bestDist);
        if (t.has_value() && *t > 0.001F && *t < bestDist) {
            bestDist = *t;
            SnapHit hit;
            hit.point = {origin.x, origin.y - *t, origin.z};
            hit.normal = {0.0F, 1.0F, 0.0F};
            hit.distance = *t;
            hit.fromKcl = false;
            hit.sourceIndex = box.objectIndex;
            best = hit;
        }
    }
    return best;
}
std::optional<SnapHit> dropToSurface(const SnapScene& scene, const math::Vec3f& position,
                                     float halfHeight, float standOff, float maxDistance,
                                     std::size_t ignoreIndex) noexcept {
    // Start just above the object's own base so a drop can never pick up the
    // object it is moving, then cast down far enough to reach the floor.
    const float lift = std::max(halfHeight, 0.0F) + 50.0F;
    const math::Vec3f origin{position.x, position.y + lift, position.z};
    auto hit = raycastDown(scene, origin, maxDistance + lift, ignoreIndex);
    if (hit.has_value()) {
        hit->point.y += standOff;
    }
    return hit;
}

std::optional<SnapHit> snapToObjectTop(const SnapScene& scene, float x, float z, float topY,
                                       float maxDrop, std::size_t ignoreIndex) noexcept {
    const math::Vec3f origin{x, topY, z};
    return raycastDown(scene, origin, std::max(maxDrop, 1.0F), ignoreIndex);
}

math::Vec3f alignUpToNormal(const math::Vec3f& currentRotation,
                            const math::Vec3f& normal) noexcept {
    // Yaw (Y) is preserved so the object keeps facing where the author put it;
    // the tilt is split between pitch (X) and roll (Z) from the normal's lean.
    const float yaw = currentRotation.y;
    const math::Vec3f n = normal.normalized();
    const float upright = std::max(n.y, 0.02F); // guard the atan2 denominators
    const float pitch = std::atan2(-n.z, upright);
    const float roll = std::atan2(n.x, upright);
    return {pitch, yaw, roll};
}

} // namespace whitehole::render

