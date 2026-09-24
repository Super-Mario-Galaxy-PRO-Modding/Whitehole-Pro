#include "whitehole/render/camera_controller.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace whitehole::render {

NavMapping mappingForPreset(NavPreset preset) noexcept {
    NavMapping m{};
    switch (preset) {
    case NavPreset::Blender:
        // Blender: MMB orbits, MMB+Shift pans, wheel dollies to the cursor.
        m.altLeftOrbits = false;
        m.middleOrbits = true;
        m.middlePans = false;
        m.altMiddlePans = true;
        m.wheelToCursor = true;
        break;
    case NavPreset::Maya:
        // Maya: Alt+LMB orbits, Alt+MMB pans; plain MMB stays free.
        m.altLeftOrbits = true;
        m.middleOrbits = false;
        m.middlePans = false;
        m.altMiddlePans = true;
        m.wheelToCursor = true;
        break;
    case NavPreset::Custom:
    case NavPreset::UnrealUnity:
    default:
        m.altLeftOrbits = true;
        m.middleOrbits = true;
        m.middlePans = true;
        m.altMiddlePans = true;
        m.wheelToCursor = true;
        break;
    }
    return m;
}

const char* navPresetLabel(NavPreset preset) noexcept {
    switch (preset) {
    case NavPreset::Blender: return "Blender Style";
    case NavPreset::Maya: return "Maya Style";
    case NavPreset::Custom: return "Custom";
    case NavPreset::UnrealUnity:
    default: return "Unreal/Unity Default";
    }
}

const char* navPresetKey(NavPreset preset) noexcept {
    switch (preset) {
    case NavPreset::Blender: return "blender";
    case NavPreset::Maya: return "maya";
    case NavPreset::Custom: return "custom";
    case NavPreset::UnrealUnity:
    default: return "unreal";
    }
}

NavPreset navPresetFromKey(std::string_view key) noexcept {
    if (key == "blender") {
        return NavPreset::Blender;
    }
    if (key == "maya") {
        return NavPreset::Maya;
    }
    if (key == "custom") {
        return NavPreset::Custom;
    }
    return NavPreset::UnrealUnity;
}

void CameraController::setPreset(NavPreset preset) noexcept {
    settings.preset = preset;
    settings.mapping = mappingForPreset(preset);
}

void CameraController::adjustFlySpeed(float notches) noexcept {
    if (notches == 0.0F) {
        return;
    }
    const float gain = settings.flyWheelGain > 0.0F ? settings.flyWheelGain : 1.25F;
    flyMultiplier = std::clamp(flyMultiplier * std::pow(gain, notches), 0.1F, 32.0F);
}

float flyStep(float distance, float dt, bool shift, bool ctrl, float baseSpeed,
              float multiplier) noexcept {
    const float safeDt = std::clamp(dt, 0.0F, 0.1F);
    const float safeDist = std::max(distance, 1.0F);
    float speed = baseSpeed * (safeDist / 800.0F) * multiplier * safeDt;
    if (shift) {
        speed *= 3.0F;
    }
    if (ctrl) {
        speed *= 0.25F;
    }
    return speed;
}

#include "camera_controller_resolve.inc"
