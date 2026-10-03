#pragma once

// Persistent user settings. Java used java.util.prefs.Preferences; here we
// use a small JSON file under %LOCALAPPDATA%/WhiteholePro (or
// ~/.config/whitehole-pro on other platforms) so settings survive reinstalls,
// are human-editable, and work without a registry dependency.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::app {

struct ColorSetting {
    std::uint8_t r{255};
    std::uint8_t g{255};
    std::uint8_t b{255};
    std::uint8_t a{255};
};

class Settings {
public:
    Settings() = default;
    static Settings& instance();

    void setConfigPath(std::filesystem::path path);
    void load();
    void save() const;
    void reset();

    // General
    std::string lastGameDir;
    std::string baseGameDir;
    std::vector<std::string> recentMaps; // most-recent-first, max 8
    bool darkMode{true};
    bool openMaximized{false};

    // Rendering toggles (honored by friend's renderer via accessors)
    bool showAxis{true};
    bool showAreas{true};
    bool showCameras{true};
    bool showGravity{true};
    bool showPaths{true};
    // Draw the game's exact collision (the KCL) as a wireframe. Off by default:
    // a new overlay must never appear uninvited in someone's zone. It is also
    // genuinely expensive -- a dense zone is ~200k triangles -- so it is built
    // once on toggle and cached, not per frame.
    bool showCollision{false};
    // Which scenario layers are VISIBLE, as a bit per LayerA..LayerP (bit 0 =
    // LayerA). All-on by default: a saved mask defaulting to nothing would open
    // every project on an empty viewport.
    //
    // A raw mask rather than a LayerFilter, deliberately: settings.hpp must not
    // depend on the render layer, and this is the same "bit per scenarioLayerBit"
    // shape createLayerMask already uses in the GUI. The panel converts with
    // LayerFilter::setMask()/mask(). Only the low 16 bits are ever meaningful.
    //
    // This is a VIEW preference, not authored data: it is never written to a zone
    // and is deliberately not undoable, exactly like showAxis.
    int visibleLayerMask{0xFFFF};
    bool betterQuality{true};
    bool lowPolyModels{false};
    bool collisionModels{false};
    // Textured models need the TEX1 upload lane; translucency needs the
    // sorted blended pass. Both default on; turning them off falls back to
    // flat material colours and the single opaque pass respectively.
    bool texturedModels{true};
    bool translucentModels{true};
    // GL texture filtering for model textures: "nearest" or "linear".
    std::string textureFilter{"linear"};
    // Maximum cached model meshes before the library evicts the
    // least-recently-used entries (with their GL resources).
    int modelCacheSize{256};

    // Editor controls
    bool reverseRotation{false};

    // --- 3D viewport navigation ---------------------------------------------
    // Control preset key: "unreal" | "blender" | "maya" | "custom".
    std::string navPreset{"unreal"};
    // Base fly speed in world units/second at an orbit distance of 800 units;
    // the speed scales with zoom so the same keys feel right at every scale.
    float flySpeed{1200.0F};
    // Wheel gain while RMB-flying: fly speed multiplier per wheel notch.
    float wheelSpeedGain{1.25F};
    // Look/orbit sensitivity in radians per pixel, and pan in distance-relative
    // pixels. Exposed so a trackpad user can slow the camera down.
    float orbitSensitivity{0.008F};
    float panSensitivity{0.0016F};
    // Exponential smoothing of fly/orbit motion for a friction-free feel.
    bool smoothCamera{true};
    // Wheel dollies toward the cursor instead of straight down the view axis.
    bool wheelZoomToCursor{true};
    // Legacy free-fly: WASD moves the camera whenever the viewport has focus.
    // Off by default so W/E/R can be the gizmo-mode keys; on, the modes move to
    // the number row in the Viewport panel.
    bool freeFlyWithoutRmb{false};

    // --- Transform snapping --------------------------------------------------
    bool snapEnabled{true};
    // false (default) = the Snap toggle plus Shift for fine drags, the long
    // standing behaviour. true = snapping only engages while Ctrl is held, the
    // Unreal/Unity/Blender convention.
    bool snapRequiresCtrl{false};
    float snapTranslate{10.0F}; // world units (0 disables the channel)
    float snapRotate{15.0F};    // degrees
    float snapScale{0.1F};      // factor step
    // Arrow-key nudge step in world units; 0 follows snapTranslate.
    float nudgeStep{0.0F};
    // 'End' drop-to-surface: also tilt the object onto the surface normal, and
    // how far above the hit point the object should rest.
    bool dropAlignToNormal{false};
    float dropStandOff{0.0F};
    // While dragging with the gizmo, keep the selection on the surface beneath
    // it (snap-to-object-tops) instead of moving freely through geometry.
    bool dropToSurfaceWhileDragging{false};

    // --- Test in Dolphin (Riivolution) ---------------------------------------
    // Where Dolphin's Riivolution root lives, the mod folder name an export
    // uses, and the region picked last. The GAME comes from the open workspace,
    // but the region cannot be: an extracted folder has no disc header left to
    // read the disc id from, and a patch for the wrong id is silently ignored.
    // So it is asked once and remembered.
    std::string dolphinSdRoot;
    std::string playtestName{"WhiteholePro"};
    std::string playtestRegion{"E"};

    // Layout: when false (default) docked panels can be rearranged inside the
    // workspace but never torn off into floating OS windows, which is what made
    // the old UI feel messy.
    bool allowFloatingPanels{false};

    void pushRecentMap(const std::string& path);

    [[nodiscard]] static std::filesystem::path defaultConfigPath();

private:
    std::filesystem::path configPath_;
    bool loaded_{false};
};

} // namespace whitehole::app
