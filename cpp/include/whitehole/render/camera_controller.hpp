#pragma once

// Camera input engine: translates raw per-frame input intent into camera
// deltas with frame-rate independent timing. Pure math, no Win32/GL, so the
// preset mapping and speed curves are unit-testable.
//
// Dual-paradigm design:
//  - RMB-held flycam (Unreal/Unity): WASD + Q/E + mouselook, wheel = speed.
//  - Orbit/pan (Blender/Maya): Alt+LMB/MMB orbit around selection pivot,
//    Shift+MMB pan in the view plane, wheel = dolly toward cursor.

#include "whitehole/math/geometry.hpp"

#include <string_view>

namespace whitehole::render {

// Which mouse-button scheme the viewport follows.
enum class NavPreset : unsigned char { UnrealUnity, Blender, Maya, Custom };

// Per-preset button mapping. Custom lets every slot be reassigned.
// RMB always drives the flycam (that is the point of the hybrid paradigm), so
// the presets only decide which *other* gestures mean orbit and pan.
struct NavMapping {
    bool altLeftOrbits{true}; // Maya / Unreal: Alt+LMB orbit
    bool middleOrbits{true};  // Blender: MMB orbit
    bool middlePans{true};
    bool altMiddlePans{true}; // Shift/MMB or Alt/MMB pan along view plane
    bool wheelToCursor{true}; // dolly toward cursor instead of plain distance scale
};

[[nodiscard]] NavMapping mappingForPreset(NavPreset preset) noexcept;
[[nodiscard]] const char* navPresetLabel(NavPreset preset) noexcept;

// Machine key for a preset (settings JSON: "unreal" | "blender" | "maya" |
// "custom"), and the reverse lookup. Unknown keys fall back to UnrealUnity so a
// hand-edited settings file can never leave the viewport without controls.
[[nodiscard]] const char* navPresetKey(NavPreset preset) noexcept;
[[nodiscard]] NavPreset navPresetFromKey(std::string_view key) noexcept;

struct NavSettings {
    NavPreset preset{NavPreset::UnrealUnity};
    NavMapping mapping{};
    bool invertOrbit{false};
    bool invertPan{false};
    // Base fly speed in world-units/sec at distance == 800 (scales with zoom).
    float flyBaseSpeed{1200.0F};
    // Wheel gain while RMB-flying: multiplier step per notch.
    float flyWheelGain{1.25F};
    float orbitSensitivity{0.008F};
    float panSensitivity{0.0016F};
    bool smoothing{true};
    float smoothTime{0.06F}; // exponential approach time constant, seconds
    // Off (default) = WASD only flies while RMB is held, which is what lets
    // W/E/R double as the gizmo-mode keys. On = the legacy free-fly (WASD moves
    // the camera whenever the viewport has focus), and the gizmo modes move to
    // the number row in the editor UI.
    bool freeFlyWithoutRmb{false};
};

// Raw intent sampled once per frame by the Win32 layer.
struct InputIntent {
    bool forward{false}; // W / Up
    bool back{false};    // S / Down
    bool left{false};    // A / Left
    bool right{false};   // D / Right
    bool up{false};      // E / PgUp
    bool down{false};    // Q / PgDn (fly-down wins over gizmo-hide while flying)
    bool shift{false};   // x3 fast
    bool ctrl{false};    // x0.25 slow (also snap modifier for transforms)
    bool alt{false};
    bool rmbHeld{false}; // flycam engaged
    bool lmbHeld{false};
    bool mmbHeld{false};
    float mouseDX{0.0F}; // mouselook / orbit pixels since last frame
    float mouseDY{0.0F};
    float wheelNotches{0.0F}; // signed, already accumulated to whole notches
    float cursorX{0.0F};
    float cursorY{0.0F};
    float width{1.0F};
    float height{1.0F};
    float dt{0.016F}; // seconds, clamped by producer to [0, 0.1]
};

// Resolved per-frame camera motion. The viewport applies it to ViewportCamera.
struct CameraDelta {
    float orbitYaw{0.0F};
    float orbitPitch{0.0F};
    float panX{0.0F};   // pixels
    float panY{0.0F};   // pixels
    float flyRight{0.0F};
    float flyUp{0.0F};
    float flyForward{0.0F}; // world units this frame
    float dollyNotches{0.0F};
    bool dollyToCursor{false};
    bool flying{false}; // RMB flycam active (drives HUD + Q-key routing)
};

// Mutable per-viewport controller state: fly speed multiplier + smoothing.
struct CameraController {
    NavSettings settings{};
    float flyMultiplier{1.0F}; // wheel-adjusted while flying, [0.1, 32]
    math::Vec3f smoothFly{};   // smoothed fly velocity accumulator (world/sec)
    math::Vec2f smoothOrbit{}; // smoothed orbit accumulator (rad/sec)

    void setPreset(NavPreset preset) noexcept;
    // Wheel while RMB-flying adjusts speed; returns human HUD label state.
    void adjustFlySpeed(float notches) noexcept;
    [[nodiscard]] float flySpeedLabel() const noexcept { return flyMultiplier; }
    void resetFlySpeed() noexcept { flyMultiplier = 1.0F; }
};

// Pure resolution: intent + controller -> delta. Never touches Win32.
[[nodiscard]] CameraDelta resolveCameraDelta(CameraController& ctl, const InputIntent& in) noexcept;

// Frame-rate independent fly step shared by tests and the viewport:
// speed scales with orbit distance so keys feel right at any zoom.
[[nodiscard]] float flyStep(float distance, float dt, bool shift, bool ctrl,
                            float baseSpeed = 1200.0F, float multiplier = 1.0F) noexcept;

} // namespace whitehole::render
