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
    bool betterQuality{true};
    bool lowPolyModels{false};
    bool collisionModels{false};

    // Editor controls
    bool reverseRotation{false};
    bool wasdMovement{false};

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
