#pragma once

// Transform gizmo: the 3D handles that let an author drag objects with the mouse
// instead of typing numbers into the Properties panel.
//
// This header is pure math — no Win32, no OpenGL — so the picking and drag
// mapping are unit-testable and reusable by any future renderer. The Win32
// viewport renders the handles and feeds mouse pixels in; the editor turns the
// resulting deltas into undoable TransformCommands.
//
// Drag maths in one place is the whole point: the screen-space axis projection
// is captured once when the drag begins, so the gizmo can never drift away from
// the cursor mid-gesture the way a "recompute per frame" version would.

#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera.hpp"

#include <cstdint>

namespace whitehole::render {

enum class GizmoMode : std::uint8_t { Translate, Rotate, Scale };

enum class GizmoHandle : std::uint8_t { None, AxisX, AxisY, AxisZ, Center };

// Unit world direction of an axis handle; the zero vector for Center.
[[nodiscard]] math::Vec3f axisDirection(GizmoHandle handle) noexcept;

// Short labels for status lines and tooltips ("X", "Translate", ...).
[[nodiscard]] const char* axisLabel(GizmoHandle handle) noexcept;
[[nodiscard]] const char* modeLabel(GizmoMode mode) noexcept;

// World length of an axis handle so the gizmo stays a constant ~80 px long on
// screen at any camera distance. viewportHeight must be > 0.
[[nodiscard]] float gizmoAxisLength(const ViewportCamera& camera, const math::Vec3f& anchor,
                                    float viewportHeight) noexcept;

// Which handle the mouse is over, if any. Axes win over the centre when both
// are in range: the centre cube is small and the axes are the common case.
[[nodiscard]] GizmoHandle pickGizmoHandle(const ViewportCamera& camera, const math::Vec3f& anchor,
                                          float screenX, float screenY, float width, float height,
                                          float tolerancePx = 7.0F) noexcept;

// One drag gesture. Everything screen-space is captured at Begin, so the result
// is a pure function of the current mouse position and the anchor cannot move
// under the cursor mid-drag.
struct GizmoDrag {
    GizmoHandle handle{GizmoHandle::None};
    GizmoMode mode{GizmoMode::Translate};
    math::Vec3f anchor{};
    math::Vec3f axis{};          // unit world axis (Axis* handles)
    math::Vec3f viewNormal{};    // camera forward at Begin (Center handle plane)
    math::Vec2f anchorScreen{};  // projected anchor, px
    math::Vec2f axisDir{};       // projected axis direction (unit, px)
    float worldPerPixel{0.0F};   // axis world length / its projected length
    float axisWorldLength{0.0F}; // world length of the axis at Begin
    math::Vec2f mouseStart{};
};

// Captures the drag. False when the handle cannot be driven from this camera
// (e.g. an axis pointing straight at the camera, which projects to a point).
[[nodiscard]] bool beginGizmoDrag(GizmoDrag& drag, const ViewportCamera& camera,
                                  const math::Vec3f& anchor, GizmoMode mode, GizmoHandle handle,
                                  float screenX, float screenY, float width, float height) noexcept;

// The world-space result of the drag so far, cumulative from Begin:
//  - Translate: position delta along the axis (Center: delta in the view plane)
//  - Rotate:    Euler degrees delta, only the handle's axis non-zero
//               (Center: free rotate around world Y, driven by horizontal drag)
//  - Scale:     per-axis factor, 1 = unchanged (Center: uniform, radial drag)
[[nodiscard]] math::Vec3f gizmoDragValue(const GizmoDrag& drag, const ViewportCamera& camera,
                                         float screenX, float screenY, float width,
                                         float height) noexcept;

enum class GizmoPhase : std::uint8_t { Begin, Update, End };

// One message from the viewport to the editor as a gizmo drag unfolds.
struct GizmoEdit {
    GizmoPhase phase{GizmoPhase::Update};
    GizmoMode mode{GizmoMode::Translate};
    GizmoHandle handle{GizmoHandle::None};
    math::Vec3f value{}; // meaning depends on mode; see gizmoDragValue
};

} // namespace whitehole::render
