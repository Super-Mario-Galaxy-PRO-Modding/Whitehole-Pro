#include "whitehole/render/camera_tween.hpp"

#include <algorithm>
#include <cmath>

#include "whitehole/render/camera.hpp"

namespace whitehole::render {

void CameraTween::start(const CameraPose& from, const CameraPose& to, float duration) noexcept {
    from_ = from;
    to_ = to;
    t_ = 0.0F;
    duration_ = duration > 0.0F ? duration : 0.45F;
    active_ = true;
}

CameraPose CameraTween::update(float dt) noexcept {
    if (!active_) {
        return to_;
    }
    t_ += dt / duration_;
    if (t_ >= 1.0F) {
        t_ = 1.0F;
        active_ = false;
    }
    const float e = easeInOutCubic(t_);
    CameraPose out;
    out.target = math::Vec3f::lerp(from_.target, to_.target, e);
    out.distance = from_.distance + (to_.distance - from_.distance) * e;
    out.yawRadians = from_.yawRadians + (to_.yawRadians - from_.yawRadians) * e;
    out.pitchRadians = from_.pitchRadians + (to_.pitchRadians - from_.pitchRadians) * e;
    return out;
}

float easeInOutCubic(float t) noexcept {
    const float c = std::clamp(t, 0.0F, 1.0F);
    if (c < 0.5F) {
        return 4.0F * c * c * c;
    }
    const float u = -2.0F * c + 2.0F;
    return 1.0F - (u * u * u) / 2.0F;
}

float frameDistanceForRadius(float radius) noexcept {
    if (!(radius > 0.0F)) {
        return 300.0F;
    }
    const float framed = radius / std::tan(ViewportCamera::kFieldOfView * 0.5F) * 1.6F;
    return std::max(framed, 300.0F);
}

} // namespace whitehole::render
