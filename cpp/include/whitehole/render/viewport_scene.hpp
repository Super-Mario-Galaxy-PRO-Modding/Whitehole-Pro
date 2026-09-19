#pragma once

// Viewport scene model: turns StageArchive placement objects into oriented
// boxes the renderer can draw, plus CPU picking. No Win32, no OpenGL here,
// so this stays unit-testable and reusable by any future renderer
// (legacy GL today, modern GL / BMD models tomorrow).

#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera.hpp"
#include "whitehole/render/model_library.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/smg/placement.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whitehole::render {

// Half-extent of a placeholder box before per-object scale is applied.
// Large enough to click at galaxy scale, small enough not to swallow maps.
inline constexpr float kPlaceholderHalfExtent = 25.0F;

// Every object is drawn with at least this scale so micro-scaled objects
// (scale 0.001 etc.) stay visible and clickable at galaxy zoom levels.
inline constexpr float kMinVisualScale = 0.35F;

struct ViewportBox {
    std::size_t objectIndex{0};
    std::string name;
    std::string kind;
    math::Matrix4 world{};     // draw matrix: placeholders include their box size
    math::Matrix4 pickWorld{}; // picking matrix: unit box [-1,1]^3 * per-axis extent
    math::Vec3f center{};
    math::Vec3f halfExtents{kPlaceholderHalfExtent, kPlaceholderHalfExtent, kPlaceholderHalfExtent};
    ObjectCategory category{ObjectCategory::Misc};
    // The object's real game model when the workspace provides one; null keeps
    // the category placeholder shape. Shared across boxes of the same model.
    std::shared_ptr<const ModelMesh> model;
};

class ViewportScene {
public:
    void rebuild(const std::vector<smg::PlacementObject>& objects,
                 ModelLibrary* models = nullptr);
    void clear() noexcept;

    [[nodiscard]] const std::vector<ViewportBox>& boxes() const noexcept { return boxes_; }
    [[nodiscard]] bool empty() const noexcept { return boxes_.empty(); }

    // Closest box hit by a camera ray. Returns the object index, if any.
    // `maxDistance` keeps far-away misclicks from selecting across the map.
    [[nodiscard]] std::optional<std::size_t> pick(const ViewportCamera& camera, float screenX, float screenY,
                                                 float width, float height,
                                                 float maxDistance = 20000.0F) const noexcept;

    // Frame-all helper: scene center + suggested camera distance.
    [[nodiscard]] math::Vec3f center() const noexcept { return center_; }
    [[nodiscard]] float frameDistance() const noexcept { return frameDistance_; }

private:
    std::vector<ViewportBox> boxes_;
    math::Vec3f center_{};
    float frameDistance_{800.0F};
};

[[nodiscard]] math::Matrix4 placementWorldMatrix(const smg::PlacementObject& object) noexcept;

// Placement transform with the object's true scale (no placeholder sizing and
// no minimum visual clamp): the matrix the game itself would apply, so a real
// BMD model drawn with it sits at the same size the game shows.
[[nodiscard]] math::Matrix4 objectWorldMatrix(const smg::PlacementObject& object) noexcept;

// Placement matrix wrapping a sphere of `radius` model units around the
// origin, so picking a real model reuses the oriented-box slab test.
[[nodiscard]] math::Matrix4 objectPickMatrix(const smg::PlacementObject& object, float radius) noexcept;

// Radius of a mesh's bounding sphere around the model origin (max distance
// from the origin to a bounding-box corner), kept clickable-degenerate safe.
[[nodiscard]] float modelPickRadius(const ModelMesh& mesh) noexcept;

// Ray vs oriented box (pickWorld matrix maps unit box [-1,1]^3 * halfExtents).
// Returns distance along the ray, or nullopt on miss.
[[nodiscard]] std::optional<float> rayIntersectsBox(const Ray& ray, const ViewportBox& box) noexcept;

} // namespace whitehole::render
