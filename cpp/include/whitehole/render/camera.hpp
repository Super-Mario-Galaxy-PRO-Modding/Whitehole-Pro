#pragma once

// Viewport camera: pure math, no Win32, no OpenGL.
// Mirrors the Java GalaxyRenderer camera (FOV 70deg, orbit eye/target maths,
// pan and orbit directions) so BMD/model renderers can reuse it unchanged.
// Java renders at 1/SCALE_DOWN with Z_NEAR 0.01 / Z_FAR 1000; here the camera
// works in raw SMG units with the equivalent near/far below.

#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera_tween.hpp"

namespace whitehole::render {

struct Ray {
    math::Vec3f origin{};
    math::Vec3f direction{0.0F, 0.0F, -1.0F};
};

class ViewportCamera {
public:
    ViewportCamera();

    // Orbit target (world units are raw SMG units / SCALE_DOWN like Java).
    math::Vec3f target{0.0F, 0.0F, 0.0F};
    float yawRadians{0.7853982F};   // 45 deg, matches Java start view
    float pitchRadians{0.5F};
    float distance{800.0F};

    static constexpr float kFieldOfView = 1.2217305F; // 70 deg in radians
    // Floor of the dynamic near plane (see nearPlane()). The old fixed
    // 1..60000 frustum quantised depth to ~20 world units at galaxy range,
    // which z-fought rails and overlays while zoomed out; the clip planes now
    // track the camera distance (see nearPlane()/farPlane()).
    static constexpr float kNearPlane = 10.0F;
    static constexpr float kFarPlane = 60000.0F; // legacy fallback, rarely reached

    // Zoom/framing bounds. 150000 (was 20000) spans the largest SMG galaxies
    // plus their frame distances, so Frame All can never be cut off mid-map.
    static constexpr float kMinDistance = 5.0F;
    static constexpr float kMaxDistance = 150000.0F;
    // Dynamic near-plane band: proportional to orbit distance (1%) so the
    // near/far ratio stays in the low thousands at every zoom level.
    static constexpr float kMaxDynamicNear = 100.0F;
    static constexpr float kMaxDynamicFar = 500000.0F;

    // Grid lines per side; the grid spec and the renderer share this number.
    static constexpr int kGridHalfLines = 20;

    // Pitch is clamped just short of straight up/down so up()/right() stay
    // well conditioned (matches the Java editor, which never rolls).
    static constexpr float kMaxPitch = 1.55F;

    [[nodiscard]] math::Vec3f eye() const noexcept;
    [[nodiscard]] math::Matrix4 viewMatrix() const noexcept;
    [[nodiscard]] math::Matrix4 projectionMatrix(float aspect) const noexcept;

    // Camera basis. Single source of truth for the view matrix, the picking
    // ray, panning and the OpenGL renderer, so screen picks always agree with
    // what is drawn. `up` mirrors the Java renderer: it flips to -Y once the
    // pitch passes straight down/up (cos(pitch) < 0), and pitch is clamped
    // before that so the basis never degenerates or rolls.
    [[nodiscard]] math::Vec3f forward() const noexcept;
    [[nodiscard]] math::Vec3f up() const noexcept;
    [[nodiscard]] math::Vec3f right() const noexcept;

    // Screen pixel -> world ray (origin at eye, direction normalized).
    // screenY grows downward like Win32/Java; height must be > 0.
    [[nodiscard]] Ray screenToRay(float screenX, float screenY, float width, float height) const noexcept;

    // Java parity controls: left-drag pans, right-drag orbits, wheel dollies.
    void orbit(float deltaYaw, float deltaPitch) noexcept;
    void pan(float deltaX, float deltaY) noexcept;
    void dolly(float wheelDelta) noexcept;
    // Wheel zoom that lands where the cursor points: the orbit target slides
    // toward the point under the cursor by the same proportion the zoom moved,
    // so "zoom into that crate over there" needs no panning first. Falls back to
    // a plain dolly for degenerate screen sizes or a singularity-free miss.
    void dollyTowardCursor(float wheelDelta, float screenX, float screenY, float width,
                           float height) noexcept;
    void frameTarget(const math::Vec3f& point, float framedDistance = 300.0F) noexcept;

    // Current pose, for the smooth-focus tween ('F').
    [[nodiscard]] CameraPose pose() const noexcept;
    // Applies a pose verbatim (used by the tween's per-frame interpolation).
    void setPose(const CameraPose& pose) noexcept;

    // Fly movement (WASD/arrows): slides the orbit target along the camera
    // basis, so the eye follows rigidly. Amounts are world units per step.
    void fly(float rightAmount, float upAmount, float forwardAmount) noexcept;

    // Clip planes. Near tracks 1% of the orbit distance (clamped), far tracks
    // distance + scene radius, so 24-bit depth stays precise from a 5-unit
    // close-up to a 150000-unit galaxy overview with no z-fighting.
    [[nodiscard]] float nearPlane() const noexcept;
    [[nodiscard]] float farPlane(float sceneRadius) const noexcept;

    // World -> screen pixel (for labels/overlays). Returns false when behind camera.
    [[nodiscard]] bool worldToScreen(const math::Vec3f& point, float width, float height, float& outX,
                                     float& outY) const noexcept;
};

// Reversed-Z perspective projection, laid out column-major
// (values[4 * column + row]) so it can be handed straight to glLoadMatrixf.
// The near plane maps to NDC depth +1 and the far plane to 0, which is the
// whole contract the GL viewport's depth buffer runs on: it clears depth to 0
// and keeps GL_GEQUAL as the frame default, so "nearer" is the LARGER value and
// the nearest surface owns every pixel. Negating the depth row (near -> -1)
// leaves the FURTHEST fragment holding the largest value instead, so the far
// side of a closed mesh wins every pixel and models render inside-out, exactly
// like standing inside a backface-culled room.
//
// The x/y terms are the same centric frustum the old glFrustum call produced
// (`m00 = f / aspect`, `m11 = f`, `f = 1 / tan(fov / 2)`, w = -z), so switching
// depth conventions can never mirror the image or reverse a triangle's screen
// winding -- which the GL_CW front face plus the per-material cull modes depend
// on. Owned here (not inline in the renderer) so the CPU side and the unit
// tests share one definition.
[[nodiscard]] math::Matrix4 reversedZProjectionMatrix(float aspect, float nearPlane, float farPlane) noexcept;

// Viewport grid sizing for one camera distance: the patch must cover the
// whole visible ground (2x orbit distance spans any aspect ratio), snapped to
// a 1/2/5x10^n step so divisions read cleanly. Shared by the renderer and
// the tests, so the grid can never silently regress to ending mid-screen.
struct GridSpec {
    float step{100.0F};
    float extent{2000.0F}; // lines run +/-extent in X and Z
};
[[nodiscard]] GridSpec gridSpec(float distance) noexcept;

} // namespace whitehole::render
