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
    // Mirrors ViewportBox::hidden, and is skipped by raycastDown() for the same
    // reason. The caller (ViewportWindow::buildSnapScene) filters these out
    // rather than setting the flag, because a snapshot of the snap scene should
    // only ever contain what is actually there to land on.
    //
    // THE ONE-LINE REVERSAL: if a hidden layer should still catch drops, set this
    // true in buildSnapScene instead of omitting the box. That is a deliberate
    // product decision, not a bug: consistency with picking says an invisible
    // surface should not silently grab an object, and an author who wants to land
    // on a hidden platform can simply unhide the layer.
    bool hidden{false};
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

// Closest hit against the exact collision soup, for PICKING rather than for
// dropping. Snapping only ever casts straight down; a click is an arbitrary ray,
// and "clicking through a wall selects the thing behind it" is the single most
// common way an editor feels broken.
//
// Pure by design -- no Win32, no OpenGL -- so the whole decision is unit-testable,
// which matters because the Win32 pick path itself cannot be.
struct CollisionHit {
    float distance{0.0F};
    math::Vec3f point{};
    // The KCL face normal (outward, as the engine's own hit tests use it).
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    // Stage index of the object that owns this collision, or kCollisionNoOwner for
    // zone-level soup parsed straight from a zone KCL with no placement. That is
    // what lets a click on a wall SELECT the wall instead of selecting nothing.
    std::size_t sourceIndex{kCollisionNoOwner};
};

// Closest triangle along the ray, within maxDistance. Two-sided, like every other
// pick here: any drawn surface is clickable regardless of winding, because that is
// what the eye sees. Nullopt when the ray misses, which is the common case and
// must stay cheap.
[[nodiscard]] std::optional<CollisionHit> rayIntersectsCollision(
    const math::Vec3f& origin, const math::Vec3f& direction,
    const std::vector<SnapTriangle>& triangles, float maxDistance) noexcept;

// THE DECISION, stated once so both the viewport and the tests read it the same
// way: a candidate hit this far along the ray is hidden BEHIND collision.
//
// Returned as "should reject" rather than "should accept" because the honest
// default is to keep the existing behaviour -- if there is no collision at all,
// nothing is rejected, and picking behaves exactly as it did before this existed.
[[nodiscard]] bool collisionOccludes(const std::optional<CollisionHit>& hit,
                                     float candidateDistance) noexcept;

// Y of the highest box top under (x, z) within maxDrop below topY.
[[nodiscard]] std::optional<SnapHit> snapToObjectTop(const SnapScene& scene, float x, float z,
                                                     float topY, float maxDrop,
                                                     std::size_t ignoreIndex) noexcept;

// Tilt an up-vector toward a surface normal, preserving yaw. Returns the new
// rotation euler (radians, XYZ order matching placementWorldMatrix).
[[nodiscard]] math::Vec3f alignUpToNormal(const math::Vec3f& currentRotation,
                                          const math::Vec3f& normal) noexcept;

} // namespace whitehole::render
