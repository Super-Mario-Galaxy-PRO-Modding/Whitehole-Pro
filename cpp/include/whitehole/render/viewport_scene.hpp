#pragma once

// Viewport scene model: turns StageArchive placement objects into oriented
// boxes the renderer can draw, plus CPU picking. No Win32, no OpenGL here,
// so this stays unit-testable and reusable by any future renderer
// (legacy GL today, modern GL / BMD models tomorrow).

#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera.hpp"
#include "whitehole/render/model_library.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/render/surface_snap.hpp"
#include "whitehole/smg/path.hpp"
#include "whitehole/smg/placement.hpp"
#include "whitehole/smg/scenario_model.hpp"

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
    // The object's scenario layer, copied from PlacementObject at rebuild time.
    // Carried here so draw, the HUD and the object tree can label or filter a box
    // without going back to the stage's object list -- the scene is what the
    // renderer sees, and re-deriving the layer from an index it does not own is
    // exactly the coupling boxFor() exists to remove.
    std::string layer;
    // True when the layer filter says this object should not be seen or clicked.
    //
    // WHY A FLAG AND NOT AN ABSENT BOX: boxes_ stays parallel to the object list,
    // so boxes_[i].objectIndex == i always holds. Compacting boxes_ to hide a
    // layer would not hide anything -- it would shift every later index and make
    // every click select the WRONG object, silently. A flag keeps selection
    // indices meaningful across a visibility change, which is what lets a hidden
    // layer's objects stay in the undo history and the tree, just not on screen.
    bool hidden{false};
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

// WELDED, so this lives here rather than in the viewport: no Win32 and no GL,
// and therefore testable. A KCL is a closed mesh of prisms, so the SAME edge is
// shared by two triangles and naive 3-per-triangle output draws every interior
// edge -- roughly tripling the line count and turning a planet's hull into an
// unreadable thicket.
[[nodiscard]] std::vector<OverlaySegment> collisionSegmentsFor(
    const std::vector<SnapTriangle>& triangles);

// Which layers are visible. Small value type + free functions, resolved through
// smg::scenarioLayerBit() so the filter and the scenario tables cannot disagree
// about what "LayerB" means.
//
// WHY IT IS A MASK AND NOT A SET OF NAMES: an object's layer is compared on every
// rebuild for every object, and a zone can hold 10k of them. A 17-bit mask makes
// "is this hidden" one shift and one test, where a set of strings would hash a
// string per object per frame.
//
// "Common" IS ALWAYS VISIBLE and owns no bit -- exactly as it owns no bit in the
// scenario file. A filter that tested a bit for Common would hide every object in
// the Common layer, which in most zones is most of the geometry. That is the trap
// this type exists to make impossible: shows() special-cases it rather than
// leaving the caller to remember.
// WIRED INTO ViewportScene::rebuild() as its `layers` argument, and the wiring
// took the shape (b) this note originally recommended: boxes_ stays parallel to
// the object list, hidden boxes are FLAGGED (ViewportBox::hidden), and draw, pick,
// the marquee, the HUD legend, surface snapping and frame-all skip them.
//
// A CORRECTION TO THE ORIGINAL NOTE, because the stale version was worse than no
// note at all: it claimed "pickAt() returns a box index". That stopped being true
// -- pick()/pickForgiving()/pickRect() all return box.objectIndex, a STAGE object
// index, and there is no pickAt. What the note got RIGHT was the danger: six
// consumers still reached into boxes()[i] with a stage index, which only worked
// because boxes_ happened to be parallel to the object list. Those now go through
// boxFor(), so the invariant is declared and defensively enforced rather than
// true by accident. See boxFor() for why that distinction matters.
struct LayerFilter {
    // Every bit the game can address set, for the "show all" default and button.
    static constexpr std::uint32_t kAllLayersMask = 0xFFFFu;

    // One bit per LayerA..LayerP (bit 0 = LayerA). All-on is the default, so a
    // caller that never thinks about layers sees the whole zone.
    //
    // kAllLayersMask rather than 0xFFFFFFFF: only 16 layers are addressable, and a
    // mask with meaningless high bits set would make everythingVisible() false for
    // a filter that has in fact hidden nothing. That reads as "some layers are
    // hidden" for a zone with none hidden.
    std::uint32_t bits{kAllLayersMask};

    // Explicit constructor helpers, because "all visible" is spelled differently
    // from "nothing visible" and getting it backwards hides a whole zone.
    [[nodiscard]] static LayerFilter allVisible() noexcept { return LayerFilter{}; }
    [[nodiscard]] static LayerFilter noneVisible() noexcept { return LayerFilter{0}; }

    // True when an object in `layer` should be drawn and pickable. A name that is
    // not a layer at all (or an unrecognised one from a modded zone) is treated as
    // visible: hiding geometry the filter does not understand would make a
    // mislabelled layer disappear with no way to get it back.
    [[nodiscard]] bool shows(std::string_view layer) const noexcept;
    void set(std::string_view layer, bool visible) noexcept;
    [[nodiscard]] bool everythingVisible() const noexcept { return bits == kAllLayersMask; }
    [[nodiscard]] std::uint32_t mask() const noexcept { return bits; }
    // Round-trips the mask through Settings, so the panel's state survives a
    // restart. The high bits are cleared rather than kept as a value no layer can
    // ever read, which keeps everythingVisible() honest about what is hidden.
    void setMask(std::uint32_t value) noexcept { bits = value & kAllLayersMask; }
    // How many of the 16 addressable layers are currently visible. Lets the panel
    // say "none of LayerA..LayerP" without the caller re-deriving it.
    [[nodiscard]] int visibleLayerCount() const noexcept;
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
    // which families to generate; `layers` hides whole scenario layers.
    //
    // All three default so existing callers keep compiling unchanged -- and
    // `layers` defaults to ALL VISIBLE specifically, because a filter that
    // defaulted to nothing would open every project on an empty viewport.
    //
    // A hidden object still gets a box (flagged, not absent): see
    // ViewportBox::hidden for why dropping it would shift every selection index.
    void rebuild(const std::vector<smg::PlacementObject>& objects, ModelLibrary* models = nullptr,
                 const std::vector<smg::RailPath>* paths = nullptr, OverlayFlags overlays = {},
                 LayerFilter layers = LayerFilter::allVisible());
    void clear() noexcept;

    [[nodiscard]] const std::vector<ViewportBox>& boxes() const noexcept { return boxes_; }
    [[nodiscard]] bool empty() const noexcept { return boxes_.empty(); }
    // How many objects the last rebuild saw -- hidden ones INCLUDED, because a
    // selection is a set of stage indices and an object on a hidden layer is
    // still in the zone. This is what callers should prune a selection against;
    // boxes().size() happens to equal it today but says the wrong thing.
    [[nodiscard]] std::size_t objectCount() const noexcept { return boxes_.size(); }
    // How many of those are currently drawn (not on a hidden layer).
    [[nodiscard]] std::size_t visibleObjectCount() const noexcept;
    [[nodiscard]] const std::vector<OverlayBatch>& overlays() const noexcept { return overlayBatches_; }
    [[nodiscard]] const std::vector<smg::RailPath>& railPaths() const noexcept { return paths_; }
    // The filter this scene was last rebuilt with, so a caller can render a panel
    // from the same value the viewport is actually using rather than from a
    // second copy it might have let drift.
    [[nodiscard]] const LayerFilter& layerFilter() const noexcept { return layers_; }

    // The ONE place that turns a stage object index into a box.
    //
    // WHY IT EXISTS: pick(), pickForgiving() and pickRect() answer with
    // box.objectIndex -- a STAGE object index -- and the GUI stores that value in
    // state.selectedObject, where everything downstream treats it as a stage
    // index. Six call sites used to reach into boxes()[i] with it, which was
    // correct only while boxes_ happened to be parallel to the object list. That
    // is an invariant nothing declared, and it is invisible until broken: one
    // compacted box does not hide a layer, it makes every later click select the
    // WRONG object.
    //
    // The fast path is the parallel case -- one bounds check, one compare. The
    // scan is the fallback, so a scene whose invariant does NOT hold still returns
    // the right box rather than the wrong one. That is the whole point: the
    // difference between "the filter stopped drawing geometry" and "the filter
    // made clicking select the wrong object".
    //
    // Returns nullptr for an out-of-range index, and for a HIDDEN box only when
    // `includeHidden` is false -- callers that ask "is this object on screen?"
    // want nullptr, while the ones that want its geometry regardless (gizmo
    // anchoring, framing a selection that is currently hidden) pass true.
    [[nodiscard]] const ViewportBox* boxFor(std::size_t objectIndex,
                                            bool includeHidden = false) const noexcept;

    // Closest box hit by a camera ray. Two-phase: the oriented proxy box
    // (broad) then the object's actual drawn triangles (narrow), so a click
    // selects exactly the geometry you see -- empty space inside a bounding
    // volume, or geometry hidden behind a nearer object, cannot be picked.
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
    // Kept so layerFilter() can report what the viewport is really using, and so
    // clear() can put it back to "everything visible" rather than leaving the
    // previous zone's filter to describe an empty scene.
    LayerFilter layers_;
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

// Ray vs the triangles an object actually draws, placed by `world` (the same
// matrix the renderer multiplies with). Flat positions take placeholder shape
// soup (3 vertices per triangle); ModelTriangle takes real model meshes.
// Returns the world-space distance along the ray or nullopt on miss. Two-sided: any
// drawn triangle is pickable regardless of winding, matching what the eye sees.
[[nodiscard]] std::optional<float> rayIntersectsTriangles(const Ray& ray, const math::Matrix4& world,
                                                           const std::vector<math::Vec3f>& triangles,
                                                           float maxDistance) noexcept;
[[nodiscard]] std::optional<float> rayIntersectsTriangles(const Ray& ray, const math::Matrix4& world,
                                                           const std::vector<ModelTriangle>& triangles,
                                                           float maxDistance) noexcept;

} // namespace whitehole::render
