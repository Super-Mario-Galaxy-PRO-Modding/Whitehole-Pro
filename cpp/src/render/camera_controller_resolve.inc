CameraDelta resolveCameraDelta(CameraController& ctl, const InputIntent& in) noexcept {
    CameraDelta out{};
    const float dt = std::clamp(in.dt, 0.0F, 0.1F);
    const NavMapping& map = ctl.settings.mapping;
    const float inv = ctl.settings.invertOrbit ? -1.0F : 1.0F;

    if (in.rmbHeld) {
        out.flying = true;
        if (in.wheelNotches != 0.0F) {
            ctl.adjustFlySpeed(in.wheelNotches);
        }
        // Normalized-per-800-units intent; viewport scales by its own distance.
        const float norm =
            flyStep(800.0F, dt, in.shift, in.ctrl, ctl.settings.flyBaseSpeed, ctl.flyMultiplier) /
            800.0F;
        const float f = (in.forward ? norm : 0.0F) - (in.back ? norm : 0.0F);
        const float r = (in.right ? norm : 0.0F) - (in.left ? norm : 0.0F);
        const float u = (in.up ? norm : 0.0F) - (in.down ? norm : 0.0F);
        if (ctl.settings.smoothing && dt > 0.0F) {
            const float t = ctl.settings.smoothTime > 0.0F ? ctl.settings.smoothTime : 0.06F;
            const float k = 1.0F - std::exp(-dt / t);
            ctl.smoothFly = {ctl.smoothFly.x + (r - ctl.smoothFly.x) * k,
                             ctl.smoothFly.y + (u - ctl.smoothFly.y) * k,
                             ctl.smoothFly.z + (f - ctl.smoothFly.z) * k};
            out.flyRight = ctl.smoothFly.x * 800.0F;
            out.flyUp = ctl.smoothFly.y * 800.0F;
            out.flyForward = ctl.smoothFly.z * 800.0F;
            ctl.smoothOrbit = {
                ctl.smoothOrbit.x + ((in.mouseDX * ctl.settings.orbitSensitivity) - ctl.smoothOrbit.x) * k,
                ctl.smoothOrbit.y + ((in.mouseDY * ctl.settings.orbitSensitivity) - ctl.smoothOrbit.y) * k};
            out.orbitYaw = ctl.smoothOrbit.x * inv;
            out.orbitPitch = ctl.smoothOrbit.y * inv;
        } else {
            out.orbitYaw = in.mouseDX * ctl.settings.orbitSensitivity * inv;
            out.orbitPitch = in.mouseDY * ctl.settings.orbitSensitivity * inv;
            out.flyRight = r * 800.0F;
            out.flyUp = u * 800.0F;
            out.flyForward = f * 800.0F;
        }
        return out;
    }

    ctl.smoothFly = {};
    const bool orbitDrag = (in.alt && in.lmbHeld && map.altLeftOrbits) ||
                           (in.mmbHeld && !in.shift && !in.alt && map.middleOrbits);
    const bool panDrag = (in.mmbHeld && map.middlePans && !orbitDrag) ||
                         ((in.alt || in.shift) && in.mmbHeld && map.altMiddlePans);
    if (orbitDrag) {
        out.orbitYaw = in.mouseDX * ctl.settings.orbitSensitivity * inv;
        out.orbitPitch = in.mouseDY * ctl.settings.orbitSensitivity * inv;
    } else if (panDrag || (in.lmbHeld && !in.alt)) {
        const float pinv = ctl.settings.invertPan ? -1.0F : 1.0F;
        out.panX = in.mouseDX * pinv;
        out.panY = in.mouseDY * pinv;
    }
    if (in.wheelNotches != 0.0F) {
        out.dollyNotches = in.wheelNotches * (in.shift ? 3.0F : 1.0F);
        out.dollyToCursor = map.wheelToCursor;
    }
    if (in.forward || in.back || in.left || in.right || in.up || in.down) {
        const float norm =
            flyStep(800.0F, dt, in.shift, in.ctrl, ctl.settings.flyBaseSpeed, 1.0F) / 800.0F;
        out.flyRight = ((in.right ? norm : 0.0F) - (in.left ? norm : 0.0F)) * 800.0F;
        out.flyUp = ((in.up ? norm : 0.0F) - (in.down ? norm : 0.0F)) * 800.0F;
        out.flyForward = ((in.forward ? norm : 0.0F) - (in.back ? norm : 0.0F)) * 800.0F;
    }
    return out;
}

} // namespace whitehole::render
