#include "whitehole/app/settings.hpp"

#include <algorithm>
#include <optional>
#include "whitehole/util/json.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
namespace whitehole::app {
namespace {
std::string getS(const util::JsonValue& o, const char* k) { return o.at(k).asString(); }
bool getB(const util::JsonValue& o, const char* k, bool fb) { return o.at(k).asBool(fb); }
int getI(const util::JsonValue& o, const char* k, int fb) {
    return static_cast<int>(o.at(k).asNumber(static_cast<double>(fb)));
}
void putS(util::JsonObject& o, const char* k, const std::string& v) { o[k] = util::JsonValue(v); }
void putB(util::JsonObject& o, const char* k, bool v) { o[k] = util::JsonValue(v); }
void putI(util::JsonObject& o, const char* k, int v) { o[k] = util::JsonValue(static_cast<double>(v)); }
} // namespace
Settings& Settings::instance() {
    static Settings s;
    return s;
}
void Settings::setConfigPath(std::filesystem::path p) {
    configPath_ = std::move(p);
}
std::filesystem::path Settings::defaultConfigPath() {
#ifdef _WIN32
    const char* local = std::getenv("LOCALAPPDATA");
    if (local != nullptr && *local != '\0')
        return std::filesystem::path(local) / "WhiteholePro" / "settings.json";
    const char* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && *profile != '\0')
        return std::filesystem::path(profile) / "WhiteholePro" / "settings.json";
    return std::filesystem::path("WhiteholePro") / "settings.json";
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0')
        return std::filesystem::path(home) / ".config" / "whitehole-pro" / "settings.json";
    return std::filesystem::path("settings.json");
#endif
}
void Settings::load() {
    if (configPath_.empty()) configPath_ = defaultConfigPath();
    loaded_ = true;
    std::ifstream in(configPath_, std::ios::binary);
    if (!in) return;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string text = buf.str();
    // Files written by Notepad or PowerShell's Set-Content/Out-File carry a
    // UTF-8 BOM (EF BB BF). The JSON parser rejects the byte as a syntax error,
    // which used to make load() return silently and reset every preference to
    // its default: theme, recent maps and all toggles. Strip it if present.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    util::JsonValue root;
    try { root = util::parseJson(text); } catch (...) { return; }
    if (!root.isObject()) return;
    lastGameDir = getS(root, "lastGameDir");
    baseGameDir = getS(root, "baseGameDir");
    darkMode = getB(root, "darkMode", true);
    openMaximized = getB(root, "openMaximized", false);
    showAxis = getB(root, "showAxis", true);
    showAreas = getB(root, "showAreas", true);
    showCameras = getB(root, "showCameras", true);
    showGravity = getB(root, "showGravity", true);
    showPaths = getB(root, "showPaths", true);
    betterQuality = getB(root, "betterQuality", true);
    lowPolyModels = getB(root, "lowPolyModels", false);
    collisionModels = getB(root, "collisionModels", false);
    texturedModels = getB(root, "texturedModels", true);
    translucentModels = getB(root, "translucentModels", true);
    textureFilter = getS(root, "textureFilter");
    if (textureFilter != "nearest" && textureFilter != "linear") textureFilter = "linear";
    modelCacheSize = getI(root, "modelCacheSize", 256);
    if (modelCacheSize < 16) modelCacheSize = 16;
    if (modelCacheSize > 4096) modelCacheSize = 4096;
    reverseRotation = getB(root, "reverseRotation", false);
    wasdMovement = getB(root, "wasdMovement", false);
    allowFloatingPanels = getB(root, "allowFloatingPanels", false);
    recentMaps.clear();
    for (const auto& item : root.at("recentMaps").asArray()) {
        if (item.isString() && recentMaps.size() < 8) recentMaps.push_back(item.asString());
    }
}
void Settings::save() const {
    if (configPath_.empty()) return;
    std::error_code ec;
    if (configPath_.has_parent_path()) std::filesystem::create_directories(configPath_.parent_path(), ec);
    util::JsonObject o;
    putS(o, "lastGameDir", lastGameDir);
    putS(o, "baseGameDir", baseGameDir);
    putB(o, "darkMode", darkMode);
    putB(o, "openMaximized", openMaximized);
    putB(o, "showAxis", showAxis);
    putB(o, "showAreas", showAreas);
    putB(o, "showCameras", showCameras);
    putB(o, "showGravity", showGravity);
    putB(o, "showPaths", showPaths);
    putB(o, "betterQuality", betterQuality);
    putB(o, "lowPolyModels", lowPolyModels);
    putB(o, "collisionModels", collisionModels);
    putB(o, "texturedModels", texturedModels);
    putB(o, "translucentModels", translucentModels);
    putS(o, "textureFilter", textureFilter);
    putI(o, "modelCacheSize", modelCacheSize);
    putB(o, "reverseRotation", reverseRotation);
    putB(o, "wasdMovement", wasdMovement);
    putB(o, "allowFloatingPanels", allowFloatingPanels);
    util::JsonArray recent;
    for (const auto& m : recentMaps) recent.emplace_back(m);
    o["recentMaps"] = util::JsonValue(std::move(recent));
    std::ofstream out(configPath_, std::ios::binary | std::ios::trunc);
    if (out) out << util::serializeJson(util::JsonValue(std::move(o)));
}
void Settings::reset() {
    lastGameDir.clear();
    baseGameDir.clear();
    recentMaps.clear();
    darkMode = true;
    openMaximized = false;
    showAxis = showAreas = showCameras = showGravity = showPaths = true;
    betterQuality = true;
    lowPolyModels = collisionModels = false;
    texturedModels = translucentModels = true;
    textureFilter = "linear";
    modelCacheSize = 256;
    reverseRotation = wasdMovement = false;
    allowFloatingPanels = false;
    loaded_ = true;
}
void Settings::pushRecentMap(const std::string& path) {
    if (path.empty()) return;
    recentMaps.erase(std::remove(recentMaps.begin(), recentMaps.end(), path), recentMaps.end());
    recentMaps.insert(recentMaps.begin(), path);
    if (recentMaps.size() > 8) recentMaps.resize(8);
}
} // namespace whitehole::app
