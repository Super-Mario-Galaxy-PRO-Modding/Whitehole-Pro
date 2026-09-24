#include "whitehole/render/gizmo.hpp"

#include <algorithm>
#include <cmath>

namespace whitehole::render {
namespace {

// Gizmo on-screen size: how long an axis reads at any zoom level.
constexpr float kAxisLengthPx = 80.0F;
constexpr float kCenterRadiusPx = 9.0F;
// An axis projecting to fewer pixels than this is nearly parallel to the view
// direction and cannot be driven by the mouse in a way anyone can predict.
constexpr float kMinAxisScreenPx = 8.0F;

// Degrees of rotation per pixel of drag along the handle's screen direction.
constexpr float kDegreesPerPixel = 0.6F;
// Free-rotate (centre handle): degrees per pixel of horizontal drag.
constexpr float kFreeRotateDegreesPerPixel = 0.5F;

// Point-to-segment distance in screen space; reports how far along the segment
// the closest point lies (0 = at `a`, 1 = at `b`).
float pointSegmentDistance(const math::Vec2f point, const math::Vec2f a, const math::Vec2f b,
                           float& outAlong) noexcept {
    const math::Vec2f ab{b.x - a.x, b.y - a.y};
    const float length2 = ab.x * ab.x + ab.y * ab.y;
    float t = 0.0F;
    if (length2 > 0.000001F) {
        const math::Vec2f ap{point.x - a.x, point.y - a.y};
        t = std::clamp((ap.x * ab.x + ap.y * ab.y) / length2, 0.0F, 1.0F);
    }
    outAlong = t;
    const math::Vec2f closest{a.x + ab.x * t, a.y + ab.y * t};
    return std::hypot(point.x - closest.x, point.y - closest.y);
}

bool projectPoint(const ViewportCamera& camera, const math::Vec3f& point, float width,
                  float height, math::Vec2f& out) noexcept {
    return camera.worldToScreen(point, width, height, out.x, out.y);
}

// Distance from the camera to the anchor, guarded so a degenerate camera never
// divides by zero.
float anchorDepth(const ViewportCamera& camera, const math::Vec3f& anchor) noexcept {
    const math::Vec3f offset{anchor.x - camera.eye().x, anchor.y - camera.eye().y,
                             anchor.z - camera.eye().z};
    return std::max(offset.length(), ViewportCamera::kNearPlane);
}

// Intersect a screen pixel's ray with the plane through `anchor` that faces the
// camera. This is what makes a centre drag a view-plane move: the grabbed point
// stays under the cursor at every depth.
bool rayPlaneHit(const ViewportCamera& camera, float screenX, float screenY, float width,
                 float height, const math::Vec3f& anchor, const math::Vec3f& normal,
                 math::Vec3f& out) noexcept {
    const Ray ray = camera.screenToRay(screenX, screenY, width, height);
    const float denominator = math::Vec3f::dot(ray.direction, normal);
    if (std::abs(denominator) < 0.000001F) {
        return false;
    }
    const math::Vec3f toAnchor{anchor.x - ray.origin.x, anchor.y - ray.origin.y,
                               anchor.z - ray.origin.z};
    const float t = math::Vec3f::dot(toAnchor, normal) / denominator;
    out = {ray.origin.x + ray.direction.x * t, ray.origin.y + ray.direction.y * t,
           ray.origin.z + ray.direction.z * t};
    return true;
}

} // namespace

math::Vec3f axisDirection(GizmoHandle handle) noexcept {
    switch (handle) {
    case GizmoHandle::AxisX: return {1.0F, 0.0F, 0.0F};
    case GizmoHandle::AxisY: return {0.0F, 1.0F, 0.0F};
    case GizmoHandle::AxisZ: return {0.0F, 0.0F, 1.0F};
    default: return {};
    }
}

const char* axisLabel(GizmoHandle handle) noexcept {
    switch (handle) {
    case GizmoHandle::AxisX: return "X";
    case GizmoHandle::AxisY: return "Y";
    case GizmoHandle::AxisZ: return "Z";
    case GizmoHandle::Center: return "Center";
    default: return "";
    }
}

const char* modeLabel(GizmoMode mode) noexcept {
    switch (mode) {
    case GizmoMode::Translate: return "Move";
    case GizmoMode::Rotate: return "Rotate";
    case GizmoMode::Scale: return "Scale";
    }
    return "Move";
}

float gizmoAxisLength(const ViewportCamera& camera, const math::Vec3f& anchor,
                      float viewportHeight) noexcept {
    if (viewportHeight <= 0.0F) {
        return 1.0F;
    }
    const float half = ViewportCamera::kFieldOfView * 0.5F;
    const float worldPerPixel =
        2.0F * anchorDepth(camera, anchor) * std::tan(half) / viewportHeight;
    return kAxisLengthPx * worldPerPixel;
}

GizmoHandle pickGizmoHandle(const ViewportCamera& camera, const math::Vec3f& anchor,
                            float screenX, float screenY, float width, float height,
                            float tolerancePx) noexcept {
    math::Vec2f centre{};
    if (!projectPoint(camera, anchor, width, height, centre)) {
        return GizmoHandle::None; // anchor behind the camera: nothing to grab
    }
    const float axisLength = gizmoAxisLength(camera, anchor, height);
    const math::Vec2f mouse{screenX, screenY};

    float best = std::max(tolerancePx, 0.0F);
    GizmoHandle result = GizmoHandle::None;
    for (const auto handle : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
        const math::Vec3f tip{anchor.x + axisDirection(handle).x * axisLength,
                              anchor.y + axisDirection(handle).y * axisLength,
                              anchor.z + axisDirection(handle).z * axisLength};
        math::Vec2f tipScreen{};
        if (!projectPoint(camera, tip, width, height, tipScreen)) {
            continue;
        }
        // An axis nearly parallel to the view direction projects to a point and
        // cannot be dragged in any way the author can predict: skip it instead
        // of inventing a mapping.
        const math::Vec2f span{tipScreen.x - centre.x, tipScreen.y - centre.y};
        if (span.length() < kMinAxisScreenPx) {
            continue;
        }
        // The centre cube owns the pixels right at the anchor: without this,
        // every axis segment passes through the anchor and the centre cube
        // would be impossible to grab. Only the anchor end of a segment is
        // excluded — the tip stays fully grabbable.
        float along = 0.0F;
        const float distance = pointSegmentDistance(mouse, centre, tipScreen, along);
        const float alongPx = along * span.length();
        if (distance < best && alongPx > kCenterRadiusPx) {
            best = distance;
            result = handle;
        }
    }
    if (result == GizmoHandle::None) {
        const math::Vec2f offset{mouse.x - centre.x, mouse.y - centre.y};
        if (offset.length() <= kCenterRadiusPx) {
            return GizmoHandle::Center;
        }
    }
    return result;
}

bool beginGizmoDrag(GizmoDrag& drag, const ViewportCamera& camera, const math::Vec3f& anchor,
                    GizmoMode mode, GizmoHandle handle, float screenX, float screenY,
                    float width, float height) noexcept {
    if (handle == GizmoHandle::None) {
        return false;
    }
    math::Vec2f centre{};
    if (!projectPoint(camera, anchor, width, height, centre)) {
        return false;
    }
    GizmoDrag fresh;
    fresh.handle = handle;
    fresh.mode = mode;
    fresh.anchor = anchor;
    fresh.mouseStart = {screenX, screenY};
    fresh.anchorScreen = centre;
    if (handle == GizmoHandle::Center) {
        fresh.viewNormal = camera.forward();
    } else {
        fresh.axis = axisDirection(handle);
        const float axisLength = gizmoAxisLength(camera, anchor, height);
        fresh.axisWorldLength = axisLength;
        fresh.viewNormal = camera.forward();
        const math::Vec3f tip{anchor.x + fresh.axis.x * axisLength,
                              anchor.y + fresh.axis.y * axisLength,
                              anchor.z + fresh.axis.z * axisLength};
        math::Vec2f tipScreen{};
        if (!projectPoint(camera, tip, width, height, tipScreen)) {
            return false;
        }
        const math::Vec2f span{tipScreen.x - centre.x, tipScreen.y - centre.y};
        const float spanLength = span.length();
        if (spanLength < kMinAxisScreenPx) {
            return false;
        }
        fresh.axisDir = {span.x / spanLength, span.y / spanLength};
        fresh.worldPerPixel = axisLength / spanLength;
    }
    drag = fresh;
    return true;
}

math::Vec3f gizmoDragValue(const GizmoDrag& drag, const ViewportCamera& camera, float screenX,
                           float screenY, float width, float height) noexcept {
    const float deltaX = screenX - drag.mouseStart.x;
    const float deltaY = screenY - drag.mouseStart.y;

    // Drag projected onto the handle's screen direction, in world units along it.
    const float alongAxisWorld = (deltaX * drag.axisDir.x + deltaY * drag.axisDir.y)
                                 * drag.worldPerPixel;

    switch (drag.mode) {
    case GizmoMode::Translate:
        if (drag.handle == GizmoHandle::Center) {
            // Move in the plane through the anchor that faces the camera, so the
            // grabbed point tracks the cursor whatever the angle.
            math::Vec3f startHit{};
            math::Vec3f currentHit{};
            if (!rayPlaneHit(camera, drag.mouseStart.x, drag.mouseStart.y, width, height,
                             drag.anchor, drag.viewNormal, startHit) ||
                !rayPlaneHit(camera, screenX, screenY, width, height, drag.anchor,
                             drag.viewNormal, currentHit)) {
                return {};
            }
            return {currentHit.x - startHit.x, currentHit.y - startHit.y,
                    currentHit.z - startHit.z};
        }
        return {drag.axis.x * alongAxisWorld, drag.axis.y * alongAxisWorld,
                drag.axis.z * alongAxisWorld};

    case GizmoMode::Rotate:
        if (drag.handle == GizmoHandle::Center) {
            // Free rotate spins the common case — world Y, the axis most placed
            // objects stand on — with a horizontal drag.
            return {0.0F, deltaX * kFreeRotateDegreesPerPixel, 0.0F};
        }
        {
            // Dragging *along* an axis twists around it, the way a ring handle
            // reads on screen: one axis length of motion is one full handle of
            // twist per pixel, tuned to feel like the Java key rotation.
            const float degrees = (deltaX * drag.axisDir.x + deltaY * drag.axisDir.y)
                                  * kDegreesPerPixel;
            return {drag.axis.x * degrees, drag.axis.y * degrees, drag.axis.z * degrees};
        }

    case GizmoMode::Scale:
        // One axis length of drag doubles (or halves) the scale, so the result
        // stays proportional to the gizmo the author is looking at.
        if (drag.handle == GizmoHandle::Center) {
            const math::Vec2f startOffset{drag.mouseStart.x - drag.anchorScreen.x,
                                          drag.mouseStart.y - drag.anchorScreen.y};
            const math::Vec2f currentOffset{screenX - drag.anchorScreen.x,
                                            screenY - drag.anchorScreen.y};
            const float startDistance = std::max(startOffset.length(), 4.0F);
            const float factor = std::clamp(currentOffset.length() / startDistance, 0.01F, 1000.0F);
            return {factor, factor, factor};
        }
        if (drag.axisWorldLength > 0.000001F) {
            const float factor = std::clamp(1.0F + alongAxisWorld / drag.axisWorldLength,
                                            0.01F, 1000.0F);
            return {(drag.axis.x) * factor + (1.0F - drag.axis.x) * 1.0F,
                    (drag.axis.y) * factor + (1.0F - drag.axis.y) * 1.0F,
                    (drag.axis.z) * factor + (1.0F - drag.axis.z) * 1.0F};
        }
        return {1.0F, 1.0F, 1.0F};
    }
    return {};
}

float snapValue(float value, float step) noexcept {
    if (!(step > 0.0F) || !std::isfinite(value)) {
        return value;
    }
    return std::round(value / step) * step;
}

math::Vec3f snapTranslate(math::Vec3f delta, float step) noexcept {
    return {snapValue(delta.x, step), snapValue(delta.y, step), snapValue(delta.z, step)};
}

math::Vec3f snapRotate(math::Vec3f degrees, float step) noexcept {
    return {snapValue(degrees.x, step), snapValue(degrees.y, step), snapValue(degrees.z, step)};
}

math::Vec3f snapScale(math::Vec3f factors, float step) noexcept {
    if (!(step > 0.0F)) {
        return factors;
    }
    // Factors compound multiplicatively, so snap the *offset from 1* instead of
    // the factor itself: 1.0 stays exactly 1.0 at any step size.
    return {1.0F + snapValue(factors.x - 1.0F, step), 1.0F + snapValue(factors.y - 1.0F, step),
            1.0F + snapValue(factors.z - 1.0F, step)};
}

math::Vec3f nudgeDelta(NudgeKey key, float step) noexcept {
    // A disabled channel (step <= 0) still nudges by one unit rather than
    // silently eating the keypress.
    const float s = step > 0.0F ? step : 1.0F;
    switch (key) {
    case NudgeKey::Left: return {-s, 0.0F, 0.0F};
    case NudgeKey::Right: return {s, 0.0F, 0.0F};
    case NudgeKey::Up: return {0.0F, 0.0F, -s};
    case NudgeKey::Down: return {0.0F, 0.0F, s};
    case NudgeKey::PageUp: return {0.0F, s, 0.0F};
    case NudgeKey::PageDown: return {0.0F, -s, 0.0F};
    }
    return {};
}

int nudgeAxis(NudgeKey key) noexcept {
    switch (key) {
    case NudgeKey::Left:
    case NudgeKey::Right: return 0;
    case NudgeKey::PageUp:
    case NudgeKey::PageDown: return 1;
    case NudgeKey::Up:
    case NudgeKey::Down: return 2;
    }
    return 0;
}

} // namespace whitehole::render
