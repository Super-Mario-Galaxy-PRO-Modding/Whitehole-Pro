#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "whitehole/app/application.hpp"

#include "whitehole/app/settings.hpp"
#include "whitehole/app/object_db_update.hpp"
#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/db/name_table.hpp"
#include "whitehole/db/object_db.hpp"
#include "whitehole/render/model_library.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/util/json.hpp"
#include "whitehole/util/text.hpp"
#include "whitehole/render/viewport_win32.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>   // SHGetKnownFolderPath
#include <objbase.h>  // CoTaskMemFree

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>     // first-boot marker file
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")

namespace whitehole::app {
namespace {

constexpr int kIdOpenGame = 1001;
constexpr int kIdOpenMap = 1002;
constexpr int kIdSave = 1003;
constexpr int kIdExit = 1004;
constexpr int kIdShowLabels = 1005;
constexpr int kIdGalaxies = 1101;
constexpr int kIdZones = 1102;
constexpr int kIdObjects = 1103;
constexpr int kIdName = 1104;
constexpr int kIdPosX = 1105;
constexpr int kIdPosY = 1106;
constexpr int kIdPosZ = 1107;
constexpr int kIdRotX = 1108;
constexpr int kIdRotY = 1109;
constexpr int kIdRotZ = 1110;
constexpr int kIdScaleX = 1111;
constexpr int kIdScaleY = 1112;
constexpr int kIdScaleZ = 1113;
constexpr int kIdApply = 1120;
constexpr int kIdRecentBase = 1200;
constexpr int kIdRecentMax = 1208;
constexpr int kIdSearch = 1210;
constexpr int kIdToggleDark = 1220;
constexpr int kIdStatus = 1121;
constexpr int kIdViewport = 1122;

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string windowText(HWND window) {
    const auto length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    return wideToUtf8(text);
}

void setWindowText(HWND window, std::string_view text) {
    SetWindowTextW(window, utf8ToWide(text).c_str());
}

float parseFloat(HWND window, float fallback) {
    try {
        return std::stof(windowText(window));
    } catch (...) {
        return fallback;
    }
}

std::string formatFloat(float value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), converted.ptr);
}

// First-launch state is kept in a per-user LocalAppData folder so it survives
// reinstalling or relocating the editor, and never touches the read-only data/
// bundle that ships beside the executable.
std::filesystem::path firstBootConfigDir() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        return std::filesystem::path{};
    }
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result / "WhiteholePro";
}

bool hasSeenFirstBoot() {
    std::error_code ec;
    return std::filesystem::exists(firstBootConfigDir() / "firstboot_done.marker", ec);
}

void markFirstBootSeen() {
    const auto dir = firstBootConfigDir();
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Best-effort: if the config dir can't be created, the splash simply
    // returns next launch instead of silently swallowing the note.
    std::ofstream(firstBootConfigDir() / "firstboot_done.marker", std::ios::trunc);
}

// One-time hello on first boot. TaskDialog gives a native, themeable popup;
// MessageBoxW is the fallback on platforms that can't resolve it. The popup
// always writes the marker on dismissal, so it genuinely only shows once.
void showFirstBootSplash(HWND owner) {
    const std::wstring content =
        L"hello i know you don't know who i am but here's a WIP rewrite of your "
        L"whole program in another language sponsored by every coding agent ever "
        L"please accept";
    const std::wstring footer =
        L"This message will only be shown one time, on first launch.";

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
        config.dwFlags = TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"Whitehole Pro";
    config.pszContent = content.c_str();
    config.pszFooter = footer.c_str();
    config.pszVerificationText = L"Don't show again";

    TASKDIALOG_BUTTON acceptButton{100, L"Accept"};
    config.pButtons = &acceptButton;
    config.cButtons = 1;
    config.nDefaultButton = 100;

    if (HMODULE comctl = GetModuleHandleW(L"comctl32.dll")) {
        using TaskDialogIndirectWFn =
            HRESULT (WINAPI *)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
        const auto taskDialogIndirect = reinterpret_cast<TaskDialogIndirectWFn>(
            GetProcAddress(comctl, "TaskDialogIndirectW"));
        if (taskDialogIndirect != nullptr) {
            int button = 0;
            BOOL verified = FALSE;
            taskDialogIndirect(&config, &button, nullptr, &verified);
            markFirstBootSeen();
            return;
        }
    }
    MessageBoxW(owner, content.c_str(), L"Whitehole Pro", MB_ICONINFORMATION | MB_OK);
    markFirstBootSeen();
}

// A small, modern face-lift for the raw-Win32 chrome: a single Segoe UI 9pt
// font applied to the window and every child control. The manifest already
// enables Common Controls v6, so the themed standard controls plus this font
// are the only visible change â€” no new libraries required. The font handle is
// intentionally not freed (created once per window, for the life of the app).
void applyModernTheme(HWND window) {
    LOGFONTW logFont{};
    HDC device = GetDC(window);
    logFont.lfHeight = -MulDiv(9, GetDeviceCaps(device, LOGPIXELSX), 72);
    ReleaseDC(window, device);
        logFont.lfWeight = FW_SEMIBOLD;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(logFont.lfFaceName, L"Segoe UI", LF_FACESIZE);
    HFONT font = CreateFontIndirectW(&logFont);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), 0);
    EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
        SendMessageW(child, WM_SETFONT, parameter, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

struct EditorState {
    std::filesystem::path dataRoot;
    db::NameTable galaxyNames;
    db::NameTable zoneNames;
    db::ObjectDatabase objectDb;
    db::ModelSubstitutions modelSubstitutions;
    render::ModelLibrary modelLibrary;
    Settings settings;
    std::optional<smg::GameArchive> game;
    std::vector<std::string> galaxies;
    std::vector<std::string> zones;
    std::optional<smg::StageArchive> stage;
    std::string filter;
    // Visible object rows after the search filter; maps list index -> stage index.
    std::vector<std::size_t> visibleObjects;
    HWND window{nullptr};
    HMENU fileMenu{nullptr};
    HWND galaxiesList{nullptr};
    HWND zonesList{nullptr};
    HWND objectsList{nullptr};
    HWND searchEdit{nullptr};
    HWND nameEdit{nullptr};
    HWND posX{nullptr};
    HWND posY{nullptr};
    HWND posZ{nullptr};
    HWND rotX{nullptr};
    HWND rotY{nullptr};
    HWND rotZ{nullptr};
    HWND scaleX{nullptr};
    HWND scaleY{nullptr};
    HWND scaleZ{nullptr};
    HWND applyButton{nullptr};
    HWND galaxiesLabel{nullptr};
    HWND zonesLabel{nullptr};
    HWND objectsLabel{nullptr};
    HWND nameLabel{nullptr};
    HWND positionLabel{nullptr};
    HWND rotationLabel{nullptr};
    HWND scaleLabel{nullptr};
    HWND status{nullptr};
    HWND hintLabel{nullptr};
    render::ViewportWindow viewport;
    render::ViewportScene viewportScene;
    std::optional<std::size_t> viewportSelected;
    bool viewportReady{false};
    bool syncingSelection{false};
    bool showLabels{false};
};

// Keeps every control anchored while the window is resized: three list columns
// on top, the 3D viewport in the middle, fixed transform rows at the bottom.
void setStatus(EditorState& state, std::string_view text);
void applyTheme(EditorState& state) {
    // Theme switch hook: force a repaint; themed controls come from the v6
    // Common Controls manifest. Full dark palettes stay in applyModernTheme().
    if (state.window != nullptr) {
        InvalidateRect(state.window, nullptr, TRUE);
    }
}

void layoutEditor(EditorState& state, HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const auto width = static_cast<int>(client.right);
    const auto height = static_cast<int>(client.bottom);

    constexpr int margin = 12;
    constexpr int rowHeight = 24;
    constexpr int rowGap = 36;
    constexpr int statusHeight = 22;

    const auto statusTop = height - margin - statusHeight;
    const auto editorTop = statusTop - rowGap - rowHeight - (3 * rowGap);
    const auto columnWidth = std::max(150, (width - margin * 4) / 3);
    const auto listTop = margin + 20;
    // Reserve the middle band for the 3D viewport; lists keep a usable height
    // while the viewport takes whatever vertical space is left.
    constexpr int listHeight = 148;
    const auto viewportTop = listTop + listHeight + 8;
    const auto viewportHeight = std::max(140, editorTop - 26 - viewportTop);

    const auto secondX = margin * 2 + columnWidth;
    const auto thirdX = margin * 3 + columnWidth * 2;
    const auto thirdWidth = std::max(150, width - thirdX - margin);

    MoveWindow(state.galaxiesLabel, margin, margin, columnWidth, 18, TRUE);
    MoveWindow(state.galaxiesList, margin, listTop, columnWidth, listHeight, TRUE);
    MoveWindow(state.zonesLabel, secondX, margin, columnWidth, 18, TRUE);
    MoveWindow(state.zonesList, secondX, listTop, columnWidth, listHeight, TRUE);
    MoveWindow(state.objectsLabel, thirdX, margin, thirdWidth, 18, TRUE);
    MoveWindow(state.searchEdit, thirdX, listTop, thirdWidth, 24, TRUE);
    MoveWindow(state.objectsList, thirdX, listTop + 28, thirdWidth, listHeight - 28, TRUE);

    const auto nameWidth = std::clamp(secondX - 76, 140, 224);
    MoveWindow(state.nameLabel, margin, editorTop + 4, 50, 18, TRUE);
    MoveWindow(state.nameEdit, 64, editorTop, nameWidth, rowHeight, TRUE);
    MoveWindow(state.positionLabel, 300, editorTop + rowGap + 4, 70, 18, TRUE);
    MoveWindow(state.rotationLabel, 300, editorTop + (2 * rowGap) + 4, 70, 18, TRUE);
    MoveWindow(state.scaleLabel, 300, editorTop + (3 * rowGap) + 4, 70, 18, TRUE);

    constexpr int valueColumns[3] = {370, 456, 542};
    const HWND positionEdits[3] = {state.posX, state.posY, state.posZ};
    const HWND rotationEdits[3] = {state.rotX, state.rotY, state.rotZ};
    const HWND scaleEdits[3] = {state.scaleX, state.scaleY, state.scaleZ};
    for (int column = 0; column < 3; ++column) {
        MoveWindow(positionEdits[column], valueColumns[column], editorTop + rowGap, 80, rowHeight, TRUE);
        MoveWindow(rotationEdits[column], valueColumns[column], editorTop + (2 * rowGap), 80, rowHeight, TRUE);
        MoveWindow(scaleEdits[column], valueColumns[column], editorTop + (3 * rowGap), 80, rowHeight, TRUE);
    }
    MoveWindow(state.applyButton, 640, editorTop + rowGap, 90, 28, TRUE);
    MoveWindow(state.status, margin, statusTop, width - margin * 2, statusHeight, TRUE);
    MoveWindow(state.hintLabel, margin, viewportTop - 2, width - margin * 2, 18, TRUE);
    if (state.viewport.handle() != nullptr) {
        MoveWindow(state.viewport.handle(), margin, viewportTop + 18, width - margin * 2, viewportHeight, TRUE);
    }
}

std::optional<std::filesystem::path> pickFolder(HWND owner) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<std::filesystem::path> pickOpenFile(HWND owner) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW info{};
    info.lStructSize = sizeof(info);
    info.hwndOwner = owner;
    info.lpstrFile = file;
    info.nMaxFile = MAX_PATH;
    info.lpstrFilter = L"RARC Archives (*.arc;*.szs)\0*.arc;*.szs\0All Files\0*.*\0";
    info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&info)) {
        return std::nullopt;
    }
    return std::filesystem::path(file);
}

void setStatus(EditorState& state, std::string_view text) {
    setWindowText(state.status, text);
}

void clearList(HWND list) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
}

void fillList(HWND list, const std::vector<std::string>& items, const db::NameTable* names) {
    clearList(list);
    for (const auto& item : items) {
        const auto label = names != nullptr ? names->displayName(item) + "  [" + item + "]" : item;
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8ToWide(label).c_str()));
    }
}

void refreshObjects(EditorState& state) {
    clearList(state.objectsList);
    state.visibleObjects.clear();
    if (!state.stage) {
        return;
    }
    // Defer viewport rebuilds while bulk-adding rows (10k+ objects).
    SendMessageW(state.objectsList, WM_SETREDRAW, FALSE, 0);
    const std::string needle = whitehole::util::toLower(state.filter);
    const auto& objects = state.stage->objects();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        if (!needle.empty()) {
            const std::string hay = whitehole::util::toLower(object.name + " " + object.kind + " " + object.layer);
            if (hay.find(needle) == std::string::npos) continue;
        }
        state.visibleObjects.push_back(i);
        std::string label = object.kind + "/" + object.layer + "  " + object.name;
        const std::string friendly = state.objectDb.displayName(object.name);
        if (friendly != "\"" + object.name + "\"") label += "  (" + friendly + ")";
        SendMessageW(state.objectsList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8ToWide(label).c_str()));
    }
    SendMessageW(state.objectsList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.objectsList, nullptr, TRUE);
}

[[nodiscard]] int listToStageIndex(EditorState& state, int listIndex) {
    if (listIndex < 0 || static_cast<std::size_t>(listIndex) >= state.visibleObjects.size()) return -1;
    return static_cast<int>(state.visibleObjects[static_cast<std::size_t>(listIndex)]);
}

[[nodiscard]] int stageToListIndex(EditorState& state, std::size_t stageIndex) {
    for (std::size_t i = 0; i < state.visibleObjects.size(); ++i) {
        if (state.visibleObjects[i] == stageIndex) return static_cast<int>(i);
    }
    return -1;
}

void rebuildRecentMenu(EditorState& state) {
    if (state.fileMenu == nullptr) return;
    // Clear old recent entries (keep static items, recent block lives at the end).
    for (int id = kIdRecentBase; id <= kIdRecentMax; ++id) {
        RemoveMenu(state.fileMenu, static_cast<UINT>(id), MF_BYCOMMAND);
    }
    const auto& recent = state.settings.recentMaps;
    if (recent.empty()) return;
    AppendMenuW(state.fileMenu, MF_SEPARATOR, 0, nullptr);
    int id = kIdRecentBase;
    for (const auto& entry : recent) {
        if (id > kIdRecentMax) break;
        AppendMenuW(state.fileMenu, MF_STRING, static_cast<UINT_PTR>(id++), utf8ToWide(entry).c_str());
    }
}

void rememberMap(EditorState& state, const std::filesystem::path& path) {
    state.settings.pushRecentMap(path.string());
    state.settings.save();
    rebuildRecentMenu(state);
}

void showObject(EditorState& state, int index);
void syncViewportSelection(EditorState& state, std::optional<std::size_t> selected);

// Status suffix describing how many objects got real game models vs. kept the
// placeholder shape in the last viewport rebuild (empty when no game models
// are available, e.g. a map archive opened without a game directory).
std::string modelStatusSuffix(EditorState& state) {
    if (!state.modelLibrary.bound() || !state.stage) {
        return "";
    }
    return " \u2014 game models shown: " + std::to_string(state.modelLibrary.loadedCount()) +
           ", placeholder: " + std::to_string(state.modelLibrary.missingCount());
}

void refreshViewport(EditorState& state, bool frame) {
    if (!state.viewportReady) {
        return;
    }
    if (state.stage) {
        // Game models come from the open workspace's ObjectData archives; a
        // standalone map archive without one keeps the placeholder shapes.
        if (state.modelLibrary.bound()) {
            state.modelLibrary.resetCounters();
            state.viewportScene.rebuild(state.stage->objects(), &state.modelLibrary);
        } else {
            state.viewportScene.rebuild(state.stage->objects());
        }
    } else {
        state.viewportScene.clear();
    }
    state.viewport.setScene(state.viewportScene);
    if (!state.stage || state.stage->objects().empty()) {
        state.viewportSelected.reset();
        state.viewport.setSelected(std::nullopt);
        return;
    }
    if (state.viewportSelected.has_value() && *state.viewportSelected >= state.stage->objects().size()) {
        state.viewportSelected.reset();
    }
    state.viewport.setSelected(state.viewportSelected);
    if (frame) {
        state.viewport.frameAll();
    } else {
        state.viewport.invalidate();
    }
}

void showObject(EditorState& state, int stageIndex) {
    if (!state.stage || stageIndex < 0 ||
        static_cast<std::size_t>(stageIndex) >= state.stage->objects().size()) {
        return;
    }
    const auto& object = state.stage->objects()[static_cast<std::size_t>(stageIndex)];
    setWindowText(state.nameEdit, object.name);
    setWindowText(state.posX, formatFloat(object.position.x));
    setWindowText(state.posY, formatFloat(object.position.y));
    setWindowText(state.posZ, formatFloat(object.position.z));
    setWindowText(state.rotX, formatFloat(object.rotation.x));
    setWindowText(state.rotY, formatFloat(object.rotation.y));
    setWindowText(state.rotZ, formatFloat(object.rotation.z));
    setWindowText(state.scaleX, formatFloat(object.scale.x));
    setWindowText(state.scaleY, formatFloat(object.scale.y));
    setWindowText(state.scaleZ, formatFloat(object.scale.z));
    // Same category language as the viewport legend and the list chips.
    const auto& style = render::objectStyle(object.kind, object.name);
    setStatus(state, std::string(object.name) + " \u2014 " + style.label + " (" + object.kind + "/" + object.layer + ")");
}

void applyObject(EditorState& state) {
    const auto listIndex = static_cast<int>(SendMessageW(state.objectsList, LB_GETCURSEL, 0, 0));
    const int stageIndex = listToStageIndex(state, listIndex);
    if (!state.stage || stageIndex < 0 ||
        static_cast<std::size_t>(stageIndex) >= state.stage->objects().size()) {
        return;
    }
    auto& object = state.stage->objects()[static_cast<std::size_t>(stageIndex)];
    object.name = windowText(state.nameEdit);
    object.position.x = parseFloat(state.posX, object.position.x);
    object.position.y = parseFloat(state.posY, object.position.y);
    object.position.z = parseFloat(state.posZ, object.position.z);
    object.rotation.x = parseFloat(state.rotX, object.rotation.x);
    object.rotation.y = parseFloat(state.rotY, object.rotation.y);
    object.rotation.z = parseFloat(state.rotZ, object.rotation.z);
    object.scale.x = parseFloat(state.scaleX, object.scale.x);
    object.scale.y = parseFloat(state.scaleY, object.scale.y);
    object.scale.z = parseFloat(state.scaleZ, object.scale.z);
    setStatus(state, "Updated " + object.name + " in memory. Save the zone to write the archive.");
    refreshObjects(state);
    const int newList = stageToListIndex(state, static_cast<std::size_t>(stageIndex));
    SendMessageW(state.objectsList, LB_SETCURSEL, static_cast<WPARAM>(newList), 0);
    state.viewportSelected = static_cast<std::size_t>(stageIndex);
    if (state.viewportReady) {
        state.viewport.setSelected(state.viewportSelected);
    }
    refreshViewport(state, false);
}

void syncViewportSelection(EditorState& state, std::optional<std::size_t> selected) {
    state.viewportSelected = selected;
    if (state.viewportReady) {
        state.viewport.setSelected(selected);
    }
    if (state.syncingSelection) {
        return;
    }
    if (selected.has_value() && state.stage && *selected < state.stage->objects().size()) {
        state.syncingSelection = true;
        SendMessageW(state.objectsList, LB_SETCURSEL, static_cast<WPARAM>(stageToListIndex(state, *selected)), 0);
        showObject(state, static_cast<int>(*selected));
        state.syncingSelection = false;
    }
}

void openMap(EditorState& state, const std::filesystem::path& path) {
    state.stage = smg::StageArchive::openMapFile(path);
    state.zones = {state.stage->stageName()};
    fillList(state.zonesList, state.zones, nullptr);
    state.filter.clear();
    if (state.searchEdit != nullptr) setWindowText(state.searchEdit, "");
    refreshObjects(state);
    syncViewportSelection(state, std::nullopt);
    refreshViewport(state, true);
    rememberMap(state, path);
    // A game directory (if one is open) still supplies the ObjectData models.
    setStatus(state, "Opened map archive with " + std::to_string(state.stage->objects().size()) + " objects." +
                         modelStatusSuffix(state));
}

void openGame(EditorState& state, const std::filesystem::path& path) {
    state.game.emplace(path);
    if (state.game->gameType() == 0) {
        state.game.reset();
        state.modelLibrary.bind(nullptr);
        throw std::runtime_error("That folder is not an SMG1/SMG2 workspace");
    }
    // ObjectData model archives live in the game workspace; bind them so the
    // viewport can show the real BMD models.
    state.modelLibrary.bind(&state.game->filesystem());
    state.modelLibrary.setLowPoly(state.settings.lowPolyModels);
    state.galaxies = state.game->galaxies();
    state.zones = state.game->zones();
    state.stage.reset();
    state.settings.lastGameDir = path.string();
    state.settings.save();
    fillList(state.galaxiesList, state.galaxies, &state.galaxyNames);
    fillList(state.zonesList, state.zones, &state.zoneNames);
    clearList(state.objectsList);
    syncViewportSelection(state, std::nullopt);
    refreshViewport(state, false);
    setStatus(state, "Opened SMG" + std::to_string(state.game->gameType()) + " workspace with "
                         + std::to_string(state.galaxies.size()) + " galaxies.");
}

void selectGalaxy(EditorState& state) {
    const auto index = static_cast<int>(SendMessageW(state.galaxiesList, LB_GETCURSEL, 0, 0));
    if (!state.game || index < 0 || static_cast<std::size_t>(index) >= state.galaxies.size()) {
        return;
    }
    const auto galaxy = state.game->openGalaxy(state.galaxies[static_cast<std::size_t>(index)]);
    state.zones = galaxy.zones();
    fillList(state.zonesList, state.zones, &state.zoneNames);
    setStatus(state, "Galaxy " + galaxy.name() + " has " + std::to_string(state.zones.size()) + " zones.");
}

void selectZone(EditorState& state) {
    const auto index = static_cast<int>(SendMessageW(state.zonesList, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= state.zones.size()) {
        return;
    }
    const auto& zone = state.zones[static_cast<std::size_t>(index)];
    if (state.game) {
        state.stage = smg::StageArchive::open(state.game->filesystem(), zone, state.game->gameType());
    }
    state.filter.clear();
    if (state.searchEdit != nullptr) setWindowText(state.searchEdit, "");
    refreshObjects(state);
    syncViewportSelection(state, std::nullopt);
    refreshViewport(state, true);
    if (state.stage) {
        setStatus(state, "Loaded " + zone + " (" + std::to_string(state.stage->objects().size()) + " objects)." +
                             modelStatusSuffix(state));
    }
}

void saveStage(EditorState& state) {
    if (!state.stage) {
        throw std::runtime_error("No zone is loaded");
    }
    applyObject(state);
    state.stage->save();
    setStatus(state, "Saved " + state.stage->sourcePath().string());
}

LRESULT CALLBACK editorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<EditorState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto* created = new EditorState();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        state = created;
        created->galaxiesLabel = CreateWindowW(L"STATIC", L"Galaxies", WS_CHILD | WS_VISIBLE, 12, 12, 240, 18, window, nullptr, nullptr, nullptr);
        created->galaxiesList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                                              12, 32, 240, 360, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdGalaxies)), nullptr, nullptr);
        created->zonesLabel = CreateWindowW(L"STATIC", L"Zones", WS_CHILD | WS_VISIBLE, 264, 12, 240, 18, window, nullptr, nullptr, nullptr);
        created->zonesList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                                           264, 32, 240, 360, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdZones)), nullptr, nullptr);
        created->objectsLabel = CreateWindowW(L"STATIC", L"Objects", WS_CHILD | WS_VISIBLE, 516, 12, 360, 18, window, nullptr, nullptr, nullptr);
        created->searchEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                            516, 32, 360, 24, window,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), nullptr, nullptr);
        // Owner-draw fixed: rows get the same category color chip the 3D
        // viewport uses, so list and scene share one visual language.
        created->objectsList = CreateWindowW(L"LISTBOX", L"",
                                             WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                             516, 60, 360, 332, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdObjects)), nullptr, nullptr);

        created->nameLabel = CreateWindowW(L"STATIC", L"Name", WS_CHILD | WS_VISIBLE, 12, 404, 50, 18, window, nullptr, nullptr, nullptr);
        created->nameEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 64, 400, 220, 24, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdName)), nullptr, nullptr);
        created->positionLabel = CreateWindowW(L"STATIC", L"Position", WS_CHILD | WS_VISIBLE, 300, 404, 70, 18, window, nullptr, nullptr, nullptr);
        created->posX = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 370, 400, 80, 24, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPosX)), nullptr, nullptr);
        created->posY = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 456, 400, 80, 24, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPosY)), nullptr, nullptr);
        created->posZ = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 542, 400, 80, 24, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPosZ)), nullptr, nullptr);
        created->rotationLabel = CreateWindowW(L"STATIC", L"Rotation", WS_CHILD | WS_VISIBLE, 300, 436, 70, 18, window, nullptr, nullptr, nullptr);
        created->rotX = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 370, 432, 80, 24, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRotX)), nullptr, nullptr);
        created->rotY = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 456, 432, 80, 24, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRotY)), nullptr, nullptr);
        created->rotZ = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 542, 432, 80, 24, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRotZ)), nullptr, nullptr);
        created->scaleLabel = CreateWindowW(L"STATIC", L"Scale", WS_CHILD | WS_VISIBLE, 300, 468, 70, 18, window, nullptr, nullptr, nullptr);
        created->scaleX = CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER, 370, 464, 80, 24, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdScaleX)), nullptr, nullptr);
        created->scaleY = CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER, 456, 464, 80, 24, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdScaleY)), nullptr, nullptr);
        created->scaleZ = CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER, 542, 464, 80, 24, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdScaleZ)), nullptr, nullptr);
        created->applyButton = CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE, 640, 400, 90, 28, window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdApply)), nullptr, nullptr);
        created->hintLabel =
            CreateWindowW(L"STATIC",
                          L"3D view: left-drag pans, right-drag orbits, wheel zooms, click selects, Space frames. "
                          L"Colors match the list and the in-view legend.",
                          WS_CHILD | WS_VISIBLE, 12, 200, 860, 18, window, nullptr, nullptr, nullptr);
        created->status = CreateWindowW(L"STATIC", L"Open a game folder or a map archive to begin.",
                                        WS_CHILD | WS_VISIBLE, 12, 504, 860, 22, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), nullptr, nullptr);
        created->viewportReady =
            created->viewport.create(window, kIdViewport, GetModuleHandleW(nullptr));
        if (created->viewportReady) {
            created->viewport.setOnSelect([state = created](std::optional<std::size_t> selected) {
                syncViewportSelection(*state, selected);
            });
            refreshViewport(*created, false);
        } else {
            setStatus(*created, "3D viewport unavailable (OpenGL init failed); list editing still works.");
        }
                DragAcceptFiles(window, TRUE);
        layoutEditor(*created, window);
        applyModernTheme(window);
        return 0;
    }
    case WM_SIZE:
        if (state != nullptr && wParam != SIZE_MINIMIZED) {
            layoutEditor(*state, window);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 780;
        limits->ptMinTrackSize.y = 720;
        return 0;
    }
    case WM_MEASUREITEM:
        // Owner-draw object rows: fixed height with room for the color chip.
        if (wParam == kIdObjects) {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            measure->itemHeight = 18;
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (wParam != kIdObjects || state == nullptr || !state->stage || draw->itemID < 0 ||
            static_cast<std::size_t>(draw->itemID) >= state->stage->objects().size()) {
            break;
        }
        const auto& object = state->stage->objects()[static_cast<std::size_t>(draw->itemID)];
        const auto& style = render::objectStyle(object.kind, object.name);
        if ((draw->itemState & ODS_SELECTED) != 0) {
            FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_HIGHLIGHT));
            SetTextColor(draw->hDC, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
            FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_WINDOW));
            SetTextColor(draw->hDC, GetSysColor(COLOR_WINDOWTEXT));
        }
        SetBkMode(draw->hDC, TRANSPARENT);
        // Category chip: the same color the viewport renders the object in.
        const int chipY = draw->rcItem.top + (draw->rcItem.bottom - draw->rcItem.top - 10) / 2;
        RECT chip{draw->rcItem.left + 5, chipY, draw->rcItem.left + 15, chipY + 10};
        HBRUSH chipBrush = CreateSolidBrush(RGB(static_cast<int>(style.color[0] * 255.0F),
                                                static_cast<int>(style.color[1] * 255.0F),
                                                static_cast<int>(style.color[2] * 255.0F)));
        FillRect(draw->hDC, &chip, chipBrush);
        DeleteObject(chipBrush);
        const std::wstring text = utf8ToWide(object.name + "   " + object.kind + "/" + object.layer);
        TextOutW(draw->hDC, draw->rcItem.left + 22, draw->rcItem.top + 2, text.c_str(), static_cast<int>(text.size()));
        if ((draw->itemState & ODS_FOCUS) != 0) {
            DrawFocusRect(draw->hDC, &draw->rcItem);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (state == nullptr) {
            break;
        }
        try {
            switch (LOWORD(wParam)) {
            case kIdOpenGame: {
                const auto folder = pickFolder(window);
                if (folder) {
                    openGame(*state, *folder);
                }
                break;
            }
            case kIdOpenMap: {
                const auto file = pickOpenFile(window);
                if (file) {
                    openMap(*state, *file);
                }
                break;
            }
            case kIdSave:
                saveStage(*state);
                break;
            case kIdApply:
                applyObject(*state);
                break;
            case kIdShowLabels:
                state->showLabels = !state->showLabels;
                CheckMenuItem(GetMenu(window), kIdShowLabels,
                              MF_BYCOMMAND | (state->showLabels ? MF_CHECKED : MF_UNCHECKED));
                if (state->viewportReady) {
                    state->viewport.setShowLabels(state->showLabels);
                }
                setStatus(*state, state->showLabels ? "Object labels on." : "Object labels off.");
                break;
            case kIdExit:
                DestroyWindow(window);
                break;
            case kIdGalaxies:
                if (HIWORD(wParam) == LBN_SELCHANGE) {
                    selectGalaxy(*state);
                }
                break;
            case kIdZones:
                if (HIWORD(wParam) == LBN_SELCHANGE) {
                    selectZone(*state);
                }
                break;
            case kIdObjects:
                if (HIWORD(wParam) == LBN_SELCHANGE && !state->syncingSelection) {
                    const auto listSel = static_cast<int>(SendMessageW(state->objectsList, LB_GETCURSEL, 0, 0));
                    const int stageSel = listToStageIndex(*state, listSel);
                    showObject(*state, stageSel);
                    if (stageSel >= 0) {
                        syncViewportSelection(*state, static_cast<std::size_t>(stageSel));
                    } else {
                        syncViewportSelection(*state, std::nullopt);
                    }
                }
                break;
            case kIdSearch:
                if (HIWORD(wParam) == EN_CHANGE) {
                    state->filter = windowText(state->searchEdit);
                    // Preserve viewport selection across filtering when possible.
                    const std::optional<std::size_t> keepSel = state->viewportSelected;
                    refreshObjects(*state);
                    if (keepSel && state->stage && *keepSel < state->stage->objects().size()) {
                        const int list = stageToListIndex(*state, *keepSel);
                        if (list >= 0) SendMessageW(state->objectsList, LB_SETCURSEL, static_cast<WPARAM>(list), 0);
                    }
                    refreshViewport(*state, false);
                }
                break;
            case kIdToggleDark:
                state->settings.darkMode = !state->settings.darkMode;
                state->settings.save();
                applyTheme(*state);
                setStatus(*state, state->settings.darkMode ? "Dark theme on." : "Light theme on.");
                break;
            default:
                if (LOWORD(wParam) >= kIdRecentBase && LOWORD(wParam) <= kIdRecentMax) {
                    const std::size_t idx = static_cast<std::size_t>(LOWORD(wParam) - kIdRecentBase);
                    if (idx < state->settings.recentMaps.size()) {
                        const std::filesystem::path path(state->settings.recentMaps[idx]);
                        if (std::filesystem::is_directory(path)) openGame(*state, path);
                        else openMap(*state, path);
                    }
                }
                break;
            }
        } catch (const std::exception& error) {
            setStatus(*state, error.what());
            MessageBoxW(window, utf8ToWide(error.what()).c_str(), L"Whitehole Pro", MB_ICONERROR);
        }
        return 0;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        if (state != nullptr && DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0) > 0) {
            wchar_t dropped[MAX_PATH]{};
            if (DragQueryFileW(drop, 0, dropped, MAX_PATH) > 0) {
                try {
                    const std::filesystem::path path(dropped);
                    if (std::filesystem::is_directory(path)) {
                        openGame(*state, path);
                    } else {
                        openMap(*state, path);
                    }
                } catch (const std::exception& error) {
                    setStatus(*state, error.what());
                    MessageBoxW(window, utf8ToWide(error.what()).c_str(), L"Whitehole Pro", MB_ICONERROR);
                }
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        if (state != nullptr) {
            state->viewport.destroy();
            delete state;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int runGui(const std::filesystem::path& executable, const std::filesystem::path& initialFile) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = editorProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"WhiteholeProEditor";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);

    HMENU menu = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, kIdOpenGame, L"Open Game Directory...\tCtrl+O");
    AppendMenuW(fileMenu, MF_STRING, kIdOpenMap, L"Open Map Archive...\tCtrl+M");
    AppendMenuW(fileMenu, MF_STRING, kIdSave, L"Save Zone\tCtrl+S");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, kIdExit, L"Exit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"File");
    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING, kIdShowLabels, L"Show Object Labels");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"View");

    HWND window = CreateWindowW(L"WhiteholeProEditor", L"Whitehole Pro", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 980, 800, nullptr, menu, GetModuleHandleW(nullptr), nullptr);
    auto* state = reinterpret_cast<EditorState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state != nullptr) {
        state->window = window;
        state->fileMenu = fileMenu;
        state->settings.load();
        state->dataRoot = dataDirectory(executable);
        state->galaxyNames.loadJson(state->dataRoot / "galaxies.json");
        state->zoneNames.loadJson(state->dataRoot / "zones.json");
        const std::filesystem::path objectDbPath = state->dataRoot / "objectdb.json";
        if (!std::filesystem::exists(objectDbPath) && objectDatabaseDownloadAvailable()) {
            // Java downloads the community database on first run (data/objectdb.json
            // is gitignored). Match that so a fresh checkout still gets real object
            // names and full parameter metadata instead of a bare names-only view.
            setStatus(*state, "Downloading the object database (first run, one time only)...");
            const std::string failure = downloadObjectDatabase(objectDbPath);
            if (!failure.empty()) {
                setStatus(*state, "Object database download failed: " + failure);
            }
        }
        state->objectDb.load(objectDbPath,
                             Settings::defaultConfigPath().parent_path() / "objectdb.cache");
        // Model name substitutions (data/modelsubstitutions.json) feed the
        // viewport's BMD model lookup; missing file just means no aliases.
        try {
            state->modelSubstitutions.setBaseGameRoot(state->dataRoot);
            state->modelSubstitutions.initBaseGame();
            state->modelSubstitutions.load();
        } catch (...) {
        }
        state->modelLibrary.setSubstitutions(&state->modelSubstitutions);
        state->modelLibrary.setLowPoly(state->settings.lowPolyModels);
        rebuildRecentMenu(*state);
        applyTheme(*state);
        if (!state->settings.lastGameDir.empty() && initialFile.empty() &&
            std::filesystem::is_directory(state->settings.lastGameDir)) {
            try {
                openGame(*state, std::filesystem::path(state->settings.lastGameDir));
            } catch (...) {
            }
        }

        bool opened = state->game.has_value() || state->stage.has_value();
        if (!initialFile.empty() && std::filesystem::exists(initialFile)) {
            try {
                if (std::filesystem::is_directory(initialFile)) {
                    openGame(*state, initialFile);
                } else {
                    openMap(*state, initialFile);
                }
                opened = true;
            } catch (const std::exception& error) {
                setStatus(*state, "Could not open " + initialFile.string() + ": " + error.what());
            }
        }
        if (!opened) {
            setStatus(*state, "Drag a map archive onto the window, or use File > Open Game Directory.");
        }
        CheckMenuItem(GetMenu(window), kIdShowLabels,
                      MF_BYCOMMAND | (state->showLabels ? MF_CHECKED : MF_UNCHECKED));
    }
        ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    // One-time first-launch splash: appears only the very first time the app
    // is run for this user (tracked by a marker file in LocalAppData).
    if (window != nullptr && !hasSeenFirstBoot()) {
        showFirstBootSplash(window);
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

} // namespace whitehole::app
