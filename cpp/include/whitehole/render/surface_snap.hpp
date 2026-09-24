#pragma once

// Surface snapping: drop-to-surface + snap-to-object-tops. Pure math over the
// ViewportScene's oriented boxes plus an optional KCL triangle soup.
//
// Clean-room design: KCL parsing (collision_kcl.hpp) reimplements the format
// from scratch; this header only consumes triangles, never game code.

#include "whitehole/math/geometry.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace whitehole::render {

// Sentinel owner index: collision that no placement object can own.
// Declared before SnapTriangle so the field default below can use it.
inline constexpr std::size_t kCollisionNoOwner = static_cast<std::size_t>(-1);

struct SnapTriangle {
    math::Vec3f a{};
    math::Vec3f b{};
    math::Vec3f c{};
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    // Stage index of the object this collision belongs to, or
    // kCollisionNoOwner for zone-level soup (parsed straight from a zone
    // KCL with no placement). Lets raycastDown skip an object's own roof
    // during a drop, exactly like the box pass skips its own box.
    std::size_t sourceIndex{kCollisionNoOwner};
};

// Moller-Trumbore ray vs triangle. Returns distance along ray, or nullopt.
[[nodiscard]] std::optional<float> rayIntersectsTriangle(const math::Vec3f& origin,
                                                         const math::Vec3f& dir,
                                                         const SnapTriangle& tri) noexcept;

struct SnapHit {
    math::Vec3f point{};
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    float distance{0.0F};
    bool fromKcl{false};
    std::size_t sourceIndex{0}; // box index when !fromKcl
};

// Flat list of oriented boxes for snapping (mirrors ViewportBox essentials
// without pulling Win32 into this header).
struct SnapBox {
    std::size_t objectIndex{0};
    math::Vec3f center{};
    math::Vec3f halfExtents{};
    math::Matrix4 pickWorld{};
};

struct SnapScene {
    std::vector<SnapBox> boxes;
    std::vector<SnapTriangle> kcl; // optional exact collision, may be empty
};

// Cast straight down (-Y) from origin; KCL wins when closer, else box tops.
[[nodiscard]] std::optional<SnapHit> raycastDown(const SnapScene& scene, const math::Vec3f& origin,
                                                 float maxDistance,
                                                 std::size_t ignoreIndex) noexcept;

// Drop an object so its base sits on the surface below it.
// halfHeight = object's vertical half extent; standOff lifts above the hit.
[[nodiscard]] std::optional<SnapHit> dropToSurface(const SnapScene& scene, const math::Vec3f& position,
                                                   float halfHeight, float standOff,
                                                   float maxDistance,
                                                   std::size_t ignoreIndex) noexcept;

// Y of the highest box top under (x, z) within maxDrop below topY.
[[nodiscard]] std::optional<SnapHit> snapToObjectTop(const SnapScene& scene, float x, float z,
                                                     float topY, float maxDrop,
                                                     std::size_t ignoreIndex) noexcept;

// Tilt an up-vector toward a surface normal, preserving yaw. Returns the new
// rotation euler (radians, XYZ order matching placementWorldMatrix).
[[nodiscard]] math::Vec3f alignUpToNormal(const math::Vec3f& currentRotation,
                                          const math::Vec3f& normal) noexcept;

} // namespace whitehole::render
