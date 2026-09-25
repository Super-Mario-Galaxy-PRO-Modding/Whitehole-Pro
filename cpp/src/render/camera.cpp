#include "whitehole/render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace whitehole::render {

ViewportCamera::ViewportCamera() = default;

math::Vec3f ViewportCamera::eye() const noexcept {
    const float cosPitch = std::cos(pitchRadians);
    const float sinPitch = std::sin(pitchRadians);
    const float cosYaw = std::cos(yawRadians);
    const float sinYaw = std::sin(yawRadians);
    return {target.x + distance * cosPitch * cosYaw, target.y + distance * sinPitch,
            target.z + distance * cosPitch * sinYaw};
}

math::Vec3f ViewportCamera::forward() const noexcept {
    const math::Vec3f position = eye();
    const math::Vec3f toTarget{target.x - position.x, target.y - position.y, target.z - position.z};
    return toTarget.normalized();
}

math::Vec3f ViewportCamera::up() const noexcept {
    // Java parity: the editor's up vector flips only once the camera passes
    // straight up/down. Clamping the pitch inside +/-kMaxPitch keeps this
    // continuous, so the view never rolls or flips near the poles the way the
    // old "|forward.y| > 0.999 -> +/-Z up" special case did.
    return std::cos(pitchRadians) >= 0.0F ? math::Vec3f{0.0F, 1.0F, 0.0F} : math::Vec3f{0.0F, -1.0F, 0.0F};
}

math::Vec3f ViewportCamera::right() const noexcept {
    const math::Vec3f facing = forward();
    math::Vec3f sideways = math::Vec3f::cross(facing, up());
    if (sideways.length() < 0.000001F) {
        // Degenerate only when looking exactly along the up axis; nudge to a
        // stable fallback instead of returning a zero basis.
        sideways = math::Vec3f::cross(facing, {0.0F, 0.0F, 1.0F});
        if (sideways.length() < 0.000001F) {
            return {1.0F, 0.0F, 0.0F};
        }
    }
    return sideways.normalized();
}

math::Matrix4 ViewportCamera::viewMatrix() const noexcept {
    const math::Vec3f position = eye();
    const math::Vec3f facing = forward();
    if (facing.length() < 0.000001F) {
        return math::Matrix4{};
    }
    const math::Vec3f sideways = right();
    const math::Vec3f upwards = math::Vec3f::cross(sideways, facing);

    // Row-major view matrix (matches Matrix4::transformPoint convention).
    math::Matrix4 view;
    view.values = {sideways.x, upwards.x, -facing.x, 0.0F, sideways.y, upwards.y, -facing.y, 0.0F, sideways.z,
                   upwards.z, -facing.z, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    view.values[12] = -(sideways.x * position.x + sideways.y * position.y + sideways.z * position.z);
    view.values[13] = -(upwards.x * position.x + upwards.y * position.y + upwards.z * position.z);
    view.values[14] = (facing.x * position.x + facing.y * position.y + facing.z * position.z);
    return view;
}

math::Matrix4 reversedZProjectionMatrix(float aspect, float nearPlane, float farPlane) noexcept {
    const float safeAspect = aspect > 0.000001F ? aspect : 1.0F;
    // The clip planes come from the camera's dynamic band, so they are already
    // positive and ordered; these guards only stop a degenerate caller from
    // dividing by zero (or leaking a NaN into the depth buffer).
    const float nearValue = nearPlane > 0.000001F ? nearPlane : 0.000001F;
    const float farValue = farPlane > nearValue ? farPlane : nearValue * 2.0F;
    const float depthRange = farValue - nearValue;
    const float f = 1.0F / std::tan(ViewportCamera::kFieldOfView * 0.5F);
    math::Matrix4 projection;
    projection.values.fill(0.0F);
    projection.values[0] = f / safeAspect;
    projection.values[5] = f;
    // Depth row: z_clip = (near / range) * z + (near * far) / range with
    // w_clip = -z, which maps the near plane to +1 and the far plane to 0.
    // Both terms share one sign; flipping it (or the two independently) is what
    // inverts the buffer and makes the far side of every model win.
    projection.values[10] = nearValue / depthRange;
    projection.values[11] = -1.0F;
    projection.values[14] = (nearValue * farValue) / depthRange;
    projection.values[15] = 0.0F;
    return projection;
}

math::Matrix4 ViewportCamera::projectionMatrix(float aspect) const noexcept {
    // Deliberately the same reversed-Z matrix the GL viewport loads, so nothing
    // projecting through this can disagree with the rendered image about which
    // way depth runs (the scene radius here only feeds the far plane, which the
    // renderer supplies itself).
    return reversedZProjectionMatrix(aspect, nearPlane(), farPlane(10000.0F));
}

Ray ViewportCamera::screenToRay(float screenX, float screenY, float width, float height) const noexcept {
    Ray ray;
    ray.origin = eye();
    if (width <= 0.0F || height <= 0.0F) {
        return ray;
    }
    const float aspect = width / height;
    const float half = kFieldOfView * 0.5F;
    const float tanHalf = std::tan(half);
    const float ndcX = (2.0F * screenX / width - 1.0F) * aspect * tanHalf;
    const float ndcY = (1.0F - 2.0F * screenY / height) * tanHalf;

    // Same basis the view matrix (and therefore the renderer) uses, so a click
    // always ray-casts through exactly the pixel it points at.
    const math::Vec3f facing = forward();
    const math::Vec3f sideways = right();
    const math::Vec3f upwards = math::Vec3f::cross(sideways, facing);
    ray.direction = {facing.x + sideways.x * ndcX + upwards.x * ndcY, facing.y + sideways.y * ndcX + upwards.y * ndcY,
                     facing.z + sideways.z * ndcX + upwards.z * ndcY};
    ray.direction = ray.direction.normalized();
    return ray;
}

void ViewportCamera::orbit(float deltaYaw, float deltaPitch) noexcept {
    yawRadians += deltaYaw;
    pitchRadians = std::clamp(pitchRadians + deltaPitch, -kMaxPitch, kMaxPitch);
}

void ViewportCamera::pan(float deltaX, float deltaY) noexcept {
    // Pan in the camera plane, scaled by distance so far scenes move faster.
    const math::Vec3f sideways = right();
    const math::Vec3f upwards = math::Vec3f::cross(sideways, forward());
    const float scale = distance * 0.0016F;
    target.x -= (sideways.x * deltaX - upwards.x * deltaY) * scale;
    target.y -= (sideways.y * deltaX - upwards.y * deltaY) * scale;
    target.z -= (sideways.z * deltaX - upwards.z * deltaY) * scale;
}

void ViewportCamera::dolly(float wheelDelta) noexcept {
    // wheelDelta is measured in wheel notches (1.0 per notch, negative when
    // scrolling back), so a fast flick zooms proportionally instead of one step.
    const float notches = std::clamp(wheelDelta, -4.0F, 4.0F);
    distance = std::clamp(distance * std::pow(0.9F, notches), kMinDistance, kMaxDistance);
}

void ViewportCamera::frameTarget(const math::Vec3f& point, float framedDistance) noexcept {
    target = point;
    distance = std::clamp(framedDistance, kMinDistance, kMaxDistance);
}

void ViewportCamera::fly(float rightAmount, float upAmount, float forwardAmount) noexcept {
    // Slide the orbit target along the camera basis; the eye follows rigidly
    // because it is derived from target + orbit offset. This keeps orbiting
    // and flying consistent -- no mode switch, no gimbal surprises.
    const math::Vec3f sideways = right();
    const math::Vec3f upwards = up();
    const math::Vec3f forwards = forward();
    target.x += sideways.x * rightAmount + upwards.x * upAmount + forwards.x * forwardAmount;
    target.y += sideways.y * rightAmount + upwards.y * upAmount + forwards.y * forwardAmount;
    target.z += sideways.z * rightAmount + upwards.z * upAmount + forwards.z * forwardAmount;
}

void ViewportCamera::dollyTowardCursor(float wheelDelta, float screenX, float screenY, float width,
                                      float height) noexcept {
    const float notches = std::clamp(wheelDelta, -4.0F, 4.0F);
    if (notches == 0.0F) {
        return;
    }
    const float before = distance;
    dolly(notches);
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }
    const float factor = distance / before; // < 1 when zooming in
    // Where the cursor ray crosses the plane through the current target: that is
    // the point the author is aiming at, at the depth they are already framing.
    const Ray ray = screenToRay(screenX, screenY, width, height);
    const math::Vec3f facing = forward();
    const math::Vec3f eyePosition = eye();
    const math::Vec3f toTarget{target.x - eyePosition.x, target.y - eyePosition.y,
                               target.z - eyePosition.z};
    const float denominator = math::Vec3f::dot(ray.direction, facing);
    if (std::abs(denominator) < 0.000001F) {
        return; // ray parallel to the view plane: plain dolly is the right answer
    }
    const float along = math::Vec3f::dot(toTarget, facing) / denominator;
    if (!(along > 0.0F)) {
        return;
    }
    const math::Vec3f hit{ray.origin.x + ray.direction.x * along,
                          ray.origin.y + ray.direction.y * along,
                          ray.origin.z + ray.direction.z * along};
    // Pull the pivot by the same proportion the camera moved, clamped so a fast
    // zoom-out widens the view instead of flinging the target off-screen.
    const float pull = std::clamp(1.0F - factor, -0.35F, 0.35F);
    target = {target.x + (hit.x - target.x) * pull, target.y + (hit.y - target.y) * pull,
              target.z + (hit.z - target.z) * pull};
}

CameraPose ViewportCamera::pose() const noexcept {
    return CameraPose{target, distance, yawRadians, pitchRadians};
}

void ViewportCamera::setPose(const CameraPose& pose) noexcept {
    target = pose.target;
    distance = std::clamp(pose.distance, kMinDistance, kMaxDistance);
    yawRadians = pose.yawRadians;
    pitchRadians = std::clamp(pose.pitchRadians, -kMaxPitch, kMaxPitch);
}

float ViewportCamera::nearPlane() const noexcept {
    // 1% of the orbit distance, clamped into a safe band. The hard floor is
    // 10 units: large galaxy bounds otherwise make the projection denominator
    // too small for the precision required by the depth buffer.
    return std::clamp(distance * 0.01F, kNearPlane, kMaxDynamicNear);
}

float ViewportCamera::farPlane(float sceneRadius) const noexcept {
    // Cover the orbit radius plus the scene radius several times over, then
    // clamp: far/near stays bounded at every zoom, so 24-bit depth never
    // z-fights at editor-relevant distances.
    float far = distance * 4.0F + std::max(sceneRadius, 0.0F) * 4.0F + 10000.0F;
    far = std::min(far, kMaxDynamicFar);
    return std::max(far, nearPlane() * 4.0F);
}

bool ViewportCamera::worldToScreen(const math::Vec3f& point, float width, float height, float& outX,
                                   float& outY) const noexcept {
    if (width <= 0.0F || height <= 0.0F) {
        return false;
    }
    const math::Matrix4 view = viewMatrix();
    const math::Vec3f viewPoint = view.transformPoint(point);
    // Cull against the *dynamic* near plane the renderer actually clips at,
    // so a label can never float over an object that was near-clipped away.
    if (viewPoint.z >= -nearPlane()) {
        return false;
    }
    const float aspect = width / height;
    const float half = kFieldOfView * 0.5F;
    const float f = 1.0F / std::tan(half);
    outX = width * 0.5F * (1.0F + (viewPoint.x * f / aspect) / -viewPoint.z);
    outY = height * 0.5F * (1.0F - (viewPoint.y * f) / -viewPoint.z);
    return true;
}

GridSpec gridSpec(float distance) noexcept {
    // The visible ground spans ~1.4x the orbit distance (frustum half-angle
    // 35deg); 2x keeps wide aspects covered with margin. Split into
    // kGridHalfLines*2 intervals, rounding the step UP to a 1/2/5x10^n value
    // so the patch is never smaller than the view (the old fixed patch always
    // ended in a hard cut mid-screen). The renderer fades the outer rings, so
    // "covering" only has to reach -- no visible edge.
    const float visible = std::max(distance, 0.0F) * 2.0F;
    const float rawStep = std::max(visible, 1.0F) /
                          static_cast<float>(ViewportCamera::kGridHalfLines * 2);
    const float pow10 = std::pow(10.0F, std::floor(std::log10(rawStep)));
    const float norm = rawStep / pow10;
    const float step = (norm <= 1.0F ? 1.0F : norm <= 2.0F ? 2.0F : norm <= 5.0F ? 5.0F : 10.0F) * pow10;
    return GridSpec{step, step * static_cast<float>(ViewportCamera::kGridHalfLines)};
}

} // namespace whitehole::render
