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
    const math::Vec3f level{std::cos(pitchRadians) >= 0.0F ? math::Vec3f{0.0F, 1.0F, 0.0F}
                                                           : math::Vec3f{0.0F, -1.0F, 0.0F}};
    if (rollRadians == 0.0F) {
        return level;
    }
    // Roll about the view axis. World Y is only perpendicular to that axis when
    // the camera looks horizontally, so the level vector is projected away from
    // the view axis first -- rotating an un-projected up about the axis would
    // shear it into the view direction and give a skewed (non-orthogonal) basis.
    // right(), the view matrix, the picking ray and worldToScreen all derive from
    // the vector returned here, so one rotation here rolls the whole view, which
    // is what makes a previewed BCAM roll honest and still clickable.
    const math::Vec3f facing = forward();
    const float along = math::Vec3f::dot(level, facing);
    math::Vec3f base{level.x - facing.x * along, level.y - facing.y * along,
                     level.z - facing.z * along};
    if (base.length() < 0.000001F) {
        return level; // looking exactly along the up axis: nothing to roll about
    }
    base = base.normalized();
    const math::Vec3f sideways = math::Vec3f::cross(facing, base).normalized();
    const float cosRoll = std::cos(rollRadians);
    const float sinRoll = std::sin(rollRadians);
    return {base.x * cosRoll + sideways.x * sinRoll, base.y * cosRoll + sideways.y * sinRoll,
            base.z * cosRoll + sideways.z * sinRoll};
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

math::Matrix4 reversedZProjectionMatrix(float aspect, float nearPlane, float farPlane,
                                        float fovRadians) noexcept {
    const float safeAspect = aspect > 0.000001F ? aspect : 1.0F;
    // The clip planes come from the camera's dynamic band, so they are already
    // positive and ordered; these guards only stop a degenerate caller from
    // dividing by zero (or leaking a NaN into the depth buffer).
    const float nearValue = nearPlane > 0.000001F ? nearPlane : 0.000001F;
    const float farValue = farPlane > nearValue ? farPlane : nearValue * 2.0F;
    const float depthRange = farValue - nearValue;
    // A previewed BCAM can ask for any fovy; keep it inside a sane band so a
    // bogus 0 or 180 degree value cannot make the frustum degenerate.
    const float safeFov = std::clamp(fovRadians > 0.0F ? fovRadians : ViewportCamera::kFieldOfView,
                                     0.0349066F, 2.9670598F); // 2 deg .. 170 deg
    const float f = 1.0F / std::tan(safeFov * 0.5F);
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

math::Matrix4 reversedZOrthographicMatrix(float aspect, float visibleHeight, float nearPlane,
                                         float farPlane) noexcept {
    const float safeAspect = aspect > 0.000001F ? aspect : 1.0F;
    // A zero or negative height would divide the whole frustum away; clamp to
    // something drawable so a bad value degrades to "very zoomed in" instead of
    // filling the matrix with NaN and poisoning every pixel.
    const float height = std::max(visibleHeight, 0.000001F);
    const float halfHeight = height * 0.5F;
    const float halfWidth = halfHeight * safeAspect;

    // Same depth ROW as the perspective path: near -> +1, far -> 0, so "nearer is
    // the larger value" and the GL_GEQUAL test behaves identically in both modes.
    //
    // BUT THE W ROW MUST DIFFER, and this is the whole subtlety of ortho:
    // perspective keeps w_clip = -z and lets the divide make depth change
    // magnification. Orthographic MUST set w_clip = 1 (w row zero, w column one),
    // so the divide is by a constant and x/y stop depending on depth. Copying
    // -z here -- the obvious "make it match" move -- silently reintroduces
    // perspective foreshortening while still looking like a correct ortho matrix.
    // With w = 1, solving a*(-near) + b = 1 and a*(-far) + b = 0 gives the row
    // below, which is also what glOrtho does with its -2/(f-n) mapping before the
    // reversed-Z flip.
    const float nearValue = nearPlane > 0.000001F ? nearPlane : 0.000001F;
    const float farValue = farPlane > nearValue ? farPlane : nearValue * 2.0F;
    const float depthRange = farValue - nearValue;

    math::Matrix4 projection;
    projection.values.fill(0.0F);
    // Orthographic x/y are a plain scale (2 / extent), NOT 1/tan(fov/2).
    projection.values[0] = 1.0F / halfWidth;
    projection.values[5] = 1.0F / halfHeight;
    projection.values[10] = 1.0F / depthRange;
    projection.values[14] = farValue / depthRange;
    // w_clip = 1: constant, so the divide cannot rescale x or y by depth.
    projection.values[11] = 0.0F;
    projection.values[15] = 1.0F;
    return projection;
}

math::Matrix4 ViewportCamera::projectionMatrix(float aspect) const noexcept {
    // Deliberately the same reversed-Z matrix the GL viewport loads, so nothing
    // projecting through this can disagree with the rendered image about which
    // way depth runs (the scene radius here only feeds the far plane, which the
    // renderer supplies itself).
    if (orthographic) {
        return reversedZOrthographicMatrix(aspect, orthoHeight, nearPlane(),
                                           farPlane(10000.0F));
    }
    return reversedZProjectionMatrix(aspect, nearPlane(), farPlane(10000.0F), fieldOfViewRadians);
}

Ray ViewportCamera::screenToRay(float screenX, float screenY, float width, float height) const noexcept {
    Ray ray;
    ray.origin = eye();
    if (width <= 0.0F || height <= 0.0F) {
        return ray;
    }
    const float aspect = width / height;
    // Same basis the view matrix (and therefore the renderer) uses, so a click
    // always ray-casts through exactly the pixel it points at.
    const math::Vec3f facing = forward();
    const math::Vec3f sideways = right();
    const math::Vec3f upwards = math::Vec3f::cross(sideways, facing);

    if (orthographic) {
        // Orthographic rays are PARALLEL: every pixel sends the same direction
        // (the view axis) from a different point on the eye plane. Getting this
        // wrong is the classic ortho-pick bug -- a perspective-style ray would
        // make clicking a far object select whatever lies along that converging
        // line instead, which reads as "picking is broken in ortho".
        //
        // The offset spans exactly the visible world height, centred on the eye,
        // so the ray starts on the plane the ortho frustum maps to screen edges.
        const float halfHeight = std::max(orthoHeight, 0.000001F) * 0.5F;
        const float halfWidth = halfHeight * aspect;
        const float ndcX = 2.0F * screenX / width - 1.0F;
        const float ndcY = 1.0F - 2.0F * screenY / height;
        const math::Vec3f pointOnPlane{eye().x + (sideways.x * ndcX * halfWidth +
                                                   upwards.x * ndcY * halfHeight),
                                       eye().y + (sideways.y * ndcX * halfWidth +
                                                   upwards.y * ndcY * halfHeight),
                                       eye().z + (sideways.z * ndcX * halfWidth +
                                                   upwards.z * ndcY * halfHeight)};
        ray.origin = pointOnPlane;
        ray.direction = facing;
        return ray;
    }

    // A caller that leaves the FOV unset (or zeroes it) must not collapse the
    // frustum to a single ray: fall back to the editor default, matching
    // reversedZProjectionMatrix().
    const float half = (fieldOfViewRadians > 0.0F ? fieldOfViewRadians : kFieldOfView) * 0.5F;
    const float tanHalf = std::tan(half);
    const float ndcX = (2.0F * screenX / width - 1.0F) * aspect * tanHalf;
    const float ndcY = (1.0F - 2.0F * screenY / height) * tanHalf;
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
    // Orthographic zoom changes the visible world HEIGHT, not the orbit distance.
    // Moving the eye instead would change what sits in front of it without
    // changing how much is visible -- and the near plane would then have to chase
    // it -- so a zoom in ortho would slide the scene sideways and clip it rather
    // than magnify it. Same 0.9^notches curve, so both modes feel identical.
    if (orthographic) {
        const float next = std::clamp(orthoHeight * std::pow(0.9F, notches),
                                      kMinOrthoHeight, kMaxOrthoHeight);
        orthoHeight = next;
        return;
    }
    distance = std::clamp(distance * std::pow(0.9F, notches), kMinDistance, kMaxDistance);
}

void ViewportCamera::frameTarget(const math::Vec3f& point, float framedDistance) noexcept {
    target = point;
    distance = std::clamp(framedDistance, kMinDistance, kMaxDistance);
}

void ViewportCamera::lookAt(const math::Vec3f& eye, const math::Vec3f& at, float fovRadians,
                            float rollRadians) noexcept {
    target = at;
    const math::Vec3f toEye{eye.x - at.x, eye.y - at.y, eye.z - at.z};
    const float length = toEye.length();
    if (length < 0.000001F) {
        // No direction to look along: keep the current angles and just move the
        // pivot, which is what a zero-length shot would mean anyway.
        distance = kMinDistance;
    } else {
        const math::Vec3f direction{toEye.x / length, toEye.y / length, toEye.z / length};
        distance = std::clamp(length, kMinDistance, kMaxDistance);
        // Inverse of eye() = target + dist * (cos p cos y, sin p, cos p sin y).
        pitchRadians = std::clamp(std::asin(std::clamp(direction.y, -1.0F, 1.0F)), -kMaxPitch,
                                  kMaxPitch);
        yawRadians = std::atan2(direction.z, direction.x);
    }
    if (fovRadians >= 0.0F) {
        fieldOfViewRadians = fovRadians;
    }
    if (rollRadians >= 0.0F) {
        this->rollRadians = rollRadians;
    }
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
    if (orthographic) {
        // Orthographic project: divide by the frustum half-extents, with NO
        // perspective divide. Two points at different depths therefore project
        // to the same pixel when they share x/y, which is the property labels and
        // the gizmo rely on to stay honest.
        const float halfHeight = std::max(orthoHeight, 0.000001F) * 0.5F;
        const float halfWidth = halfHeight * aspect;
        outX = width * 0.5F * (1.0F + viewPoint.x / halfWidth);
        outY = height * 0.5F * (1.0F - viewPoint.y / halfHeight);
        return true;
    }
    const float half = fieldOfViewRadians * 0.5F;
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
