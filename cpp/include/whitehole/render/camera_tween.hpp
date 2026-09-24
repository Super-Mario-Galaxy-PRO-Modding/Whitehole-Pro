#pragma once

// Smooth focus tween for the 'F' key: interpolates orbit target + distance
// (+ optional yaw/pitch) with an ease-in-out curve. Pure math, no Win32.

#include "whitehole/math/geometry.hpp"

namespace whitehole::render {

struct CameraPose {
    math::Vec3f target{};
    float distance{800.0F};
    float yawRadians{0.7853982F};
    float pitchRadians{0.5F};
};

class CameraTween {
public:
    void start(const CameraPose& from, const CameraPose& to, float duration = 0.45F) noexcept;
    void cancel() noexcept { active_ = false; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    // Advances by dt seconds; returns the interpolated pose. Deactivates at t=1.
    CameraPose update(float dt) noexcept;

private:
    CameraPose from_{};
    CameraPose to_{};
    float t_{0.0F};
    float duration_{0.45F};
    bool active_{false};
};

[[nodiscard]] float easeInOutCubic(float t) noexcept;
// Bounding-sphere framing distance for a radius, matching frameSelection math.
[[nodiscard]] float frameDistanceForRadius(float radius) noexcept;

} // namespace whitehole::render
