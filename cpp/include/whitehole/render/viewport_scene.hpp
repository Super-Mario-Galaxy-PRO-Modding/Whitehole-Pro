#pragma once

// Viewport scene model: turns StageArchive placement objects into oriented
// boxes the renderer can draw, plus CPU picking. No Win32, no OpenGL here,
// so this stays unit-testable and reusable by any future renderer
// (legacy GL today, modern GL / BMD models tomorrow).

#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera.hpp"
#include "whitehole/render/model_library.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/smg/path.hpp"
#include "whitehole/smg/placement.hpp"

#include <cstddef>
#include <cstdint>
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

// Sentinel rail index: an overlay batch that belongs to no rail.
inline constexpr std::size_t kNoRail = static_cast<std::size_t>(-1);

// One straight overlay line: the primitive every overlay shape (rails, boxes,
// the axis) is built from. The renderer draws each batch as one GL_LINES pass.
struct OverlaySegment {
    math::Vec3f from;
    math::Vec3f to;
};

// One colour + line width group of segments -- one draw call in the renderer.
struct OverlayBatch {
    std::uint32_t color{0xFFFFFFFFu}; // 0xRRGGBBAA
    float width{1.0F};
    std::size_t railIndex{kNoRail};   // rail this batch draws, or kNoRail
    std::vector<OverlaySegment> segments;
};

// One pickable rail point: which rail, which point, and which of its three
// vectors (0 = pnt0 position, 1 = control1, 2 = control2).
struct RailPointRef {
    std::size_t pathIndex{0};
    std::size_t pointIndex{0};
    int part{0};

    [[nodiscard]] bool operator==(const RailPointRef& other) const noexcept = default;
};

// Which overlay families a rebuild generates. The View menu toggles map onto
// these one-to-one, so a disabled family produces no geometry at all instead
// of geometry the renderer has to filter.
struct OverlayFlags {
    bool axis{true};
    bool areas{true};
    bool cameras{true};
    bool gravity{true};
    bool paths{true};
};

// Stable colour for one rail: the hue walks the golden ratio from l_id, so a
// path keeps the same colour across sessions and across rebuilds (Java drew a
// fresh random colour every run, which made screenshots incomparable).
[[nodiscard]] std::uint32_t railPathColor(std::int32_t lId) noexcept;

class ViewportScene {
public:
    // `paths` feeds the rail overlays (null = no rails); `overlays` selects
    // which families to generate. Both default so existing callers keep
    // compiling unchanged.
    void rebuild(const std::vector<smg::PlacementObject>& objects, ModelLibrary* models = nullptr,
                 const std::vector<smg::RailPath>* paths = nullptr, OverlayFlags overlays = {});
    void clear() noexcept;

    [[nodiscard]] const std::vector<ViewportBox>& boxes() const noexcept { return boxes_; }
    [[nodiscard]] bool empty() const noexcept { return boxes_.empty(); }
    [[nodiscard]] const std::vector<OverlayBatch>& overlays() const noexcept { return overlayBatches_; }
    [[nodiscard]] const std::vector<smg::RailPath>& railPaths() const noexcept { return paths_; }

    // Closest box hit by a camera ray. Returns the object index, if any.
    // `maxDistance` keeps far-away misclicks from selecting across the map.
    [[nodiscard]] std::optional<std::size_t> pick(const ViewportCamera& camera, float screenX, float screenY,
                                                 float width, float height,
                                                 float maxDistance = 20000.0F) const noexcept;

    // Forgiving click: exact ray hit first, else the box whose screen-projected
    // centre is closest to the cursor within `slopPx`. Keeps tiny / distant
    // objects clickable without stealing clicks from real hits.
    [[nodiscard]] std::optional<std::size_t> pickForgiving(const ViewportCamera& camera, float screenX,
                                                           float screenY, float width, float height,
                                                           float maxDistance = 20000.0F,
                                                           float slopPx = 10.0F) const noexcept;

    // Rubber-band box select: every box whose projected centre falls inside the
    // screen rectangle (corners in any order). Used for Shift-drag marquee.
    [[nodiscard]] std::vector<std::size_t> pickRect(const ViewportCamera& camera, float x0, float y0, float x1,
                                                    float y1, float width, float height) const noexcept;

    // Frame-all helper: scene center + suggested camera distance.
    [[nodiscard]] math::Vec3f center() const noexcept { return center_; }
    [[nodiscard]] float frameDistance() const noexcept { return frameDistance_; }

    // Closest rail point under the cursor -- point cubes and handle cubes are
    // pickable while the paths overlay is on. nullopt when the overlay is off
    // (picking invisible geometry would feel like a bug) or nothing is hit.
    [[nodiscard]] std::optional<RailPointRef> pickRailPoint(const ViewportCamera& camera, float screenX,
                                                            float screenY, float width, float height,
                                                            float maxDistance = 20000.0F) const noexcept;

private:
    std::vector<ViewportBox> boxes_;
    std::vector<OverlayBatch> overlayBatches_;
    std::vector<smg::RailPath> paths_;
    bool pathsPickable_{false};
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
