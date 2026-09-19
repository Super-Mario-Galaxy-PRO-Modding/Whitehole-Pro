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

#include "whitehole/app/gui_theme.hpp"
#include "whitehole/app/settings.hpp"
#include "whitehole/app/object_db_update.hpp"
#include "whitehole/db/name_table.hpp"
#include "whitehole/db/object_db.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/util/text.hpp"
#include "whitehole/render/viewport_win32.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <d3d11.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder default layout (vendored, stable pin)
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

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

// ImGui's Win32 backend implements its input handler here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace whitehole::app {
namespace {

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

void showBootError(const wchar_t* what) {
    MessageBoxW(nullptr, what, L"Whitehole Pro — startup failed",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void showBootHresult(const wchar_t* what, HRESULT hr) {
    wchar_t buffer[512];
    swprintf(buffer, 512, L"%s\n\nHRESULT: 0x%08lX", what, static_cast<unsigned long>(hr));
    showBootError(buffer);
}

float parseFloatText(std::string_view text, float fallback) {
    try {
        return std::stof(std::string(text));
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
    std::ofstream(firstBootConfigDir() / "firstboot_done.marker", std::ios::trunc);
}

// One-time hello on first boot. TaskDialog gives a native, themeable popup;
// MessageBoxW is the fallback. The marker is always written on dismissal.
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
            taskDialogIndirect(&config, nullptr, nullptr, nullptr);
            markFirstBootSeen();
            return;
        }
    }
    MessageBoxW(owner, content.c_str(), L"Whitehole Pro", MB_ICONINFORMATION | MB_OK);
    markFirstBootSeen();
}

// ---------------------------------------------------------------------------
// Editor state: everything the immediate-mode UI reads and writes. Unlike the
// old control-based GUI there are no HWND widget fields here — panels render
// every frame straight from this struct.
// ---------------------------------------------------------------------------
struct Toast {
    std::string text;
    double born{0.0};
    bool error{false};
};

struct EditorState {
    std::filesystem::path dataRoot;
    db::NameTable galaxyNames;
    db::NameTable zoneNames;
    db::ObjectDatabase objectDb;
    Settings settings;
    std::optional<smg::GameArchive> game;
    std::vector<std::string> galaxies;
    std::vector<std::string> zones;
    std::optional<smg::StageArchive> stage;
    std::string filter;
    // Visible object rows after the search filter; maps list row -> stage index.
    std::vector<std::size_t> visibleObjects;

    HWND window{nullptr};
    render::ViewportWindow viewport;
    render::ViewportScene viewportScene;
    std::optional<std::size_t> viewportSelected;
    bool viewportReady{false};
    bool syncingSelection{false};
    bool showLabels{false};

    // --- UI bookkeeping -----------------------------------------------------
    int selectedGalaxy{-1};
    int selectedZone{-1};
    std::optional<std::size_t> selectedObject;
    char searchBuf[160]{};   // object list filter
    char nameBuf[160]{};     // selected object name (committed on Enter)
    float transform[9]{};    // pos.xyz, rot.xyz, scale.xyz (display values)
    bool transformDirty{false};
    bool unsaved{false};

    // --- Docked workspace visibility (View menu toggles, persisted) --------
    bool showProject{true};
    bool showObjects{true};
    bool showProperties{true};
    bool showViewport{true};
    bool showLog{false}; // bottom drawer, hidden until needed
    bool showStatusBar{true};
    bool showToolbar{true};
    bool showAbout{false};
    bool showPreferences{false};
    bool showShortcuts{false};
    bool focusSearch{false}; // set by Ctrl+F, consumed by the Objects panel
    bool firstFrame{true};   // default dock layout is built once via DockBuilder
    std::string statusText{
        "Drag a map archive onto the window, or use File > Open Game Directory."};
    std::vector<Toast> toasts;
    std::vector<std::string> logLines;
    std::string lastFilter; // cached so filtering only reruns on change
};

// --- DX11 plumbing -----------------------------------------------------------
// Dear ImGui renders through a swap chain on the main window; the OpenGL
// viewport runs its own context in a child window, so the two never touch each
// other's device state.

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
HRESULT g_lastDeviceHr = S_OK; // last D3D11CreateDeviceAndSwapChain result

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
    backBuffer->Release();
}

void cleanupRenderTarget() {
    if (g_renderTarget != nullptr) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

float dpiScaleFactor() {
    // We don't have a window handle at call time in runGui, so fall back to
    // 96 DPI == 1.0 scale. The DPI is picked up per-frame from the window once
    // it exists and the ImGui backend handles per-monitor DPI automatically.
    return 1.0F;
}

bool createDeviceD3D(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT createHr = S_OK;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc,
        &g_swapChain, &g_device, &featureLevel, &g_context);
    if (FAILED(hr)) {
        // Hardware device failed (e.g. no GPU driver in a VM/RDP session);
        // fall back to the WARP software rasterizer so the editor still boots.
        createHr = hr;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc,
            &g_swapChain, &g_device, &featureLevel, &g_context);
    }
    g_lastDeviceHr = hr;
    if (FAILED(hr)) {
        showBootHresult(L"Could not create the Direct3D 11 device.\n"
                        L"Hardware attempt also reported an error.",
                        createHr);
        return false;
    }
    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain != nullptr) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
}

// --- user feedback helpers ---------------------------------------------------

void setStatus(EditorState& state, std::string_view text) {
    state.statusText = std::string(text);
}

void pushLog(EditorState& state, std::string_view text) {
    state.logLines.emplace_back(text);
    if (state.logLines.size() > 500) {
        state.logLines.erase(state.logLines.begin());
    }
}

void pushToast(EditorState& state, std::string_view text, bool error = false) {
    state.toasts.push_back(Toast{std::string(text), ImGui::GetTime(), error});
    pushLog(state, text);
    setStatus(state, text);
}

// --- file dialogs ------------------------------------------------------------

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

// --- data layer --------------------------------------------------------------

void refreshObjects(EditorState& state) {
    state.visibleObjects.clear();
    if (!state.stage) {
        return;
    }
    const std::string needle = whitehole::util::toLower(state.filter);
    const auto& objects = state.stage->objects();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        if (!needle.empty()) {
            const std::string hay = whitehole::util::toLower(
                object.name + " " + object.kind + " " + object.layer);
            if (hay.find(needle) == std::string::npos) continue;
        }
        state.visibleObjects.push_back(i);
    }
}

void refreshViewport(EditorState& state, bool frame) {
    if (!state.viewportReady) {
        return;
    }
    if (state.stage) {
        state.viewportScene.rebuild(state.stage->objects());
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
    state.viewport.setShowLabels(state.showLabels);
    if (frame) {
        state.viewport.frameAll();
    } else {
        state.viewport.invalidate();
    }
}

// Copies the selected object into the property widgets.
void syncTransformBuffers(EditorState& state) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        state.transformDirty = false;
        return;
    }
    const auto& object = state.stage->objects()[*state.selectedObject];
    std::snprintf(state.nameBuf, sizeof(state.nameBuf), "%s", object.name.c_str());
    state.transform[0] = object.position.x;
    state.transform[1] = object.position.y;
    state.transform[2] = object.position.z;
    state.transform[3] = object.rotation.x;
    state.transform[4] = object.rotation.y;
    state.transform[5] = object.rotation.z;
    state.transform[6] = object.scale.x;
    state.transform[7] = object.scale.y;
    state.transform[8] = object.scale.z;
    state.transformDirty = false;
}

// Pushes the property widgets into the selected object and repaints the scene.
void applyTransform(EditorState& state) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        return;
    }
    auto& object = state.stage->objects()[*state.selectedObject];
    if (const std::string newName = state.nameBuf;
        !newName.empty() && newName != object.name) {
        object.name = newName;
    }
    object.position.x = state.transform[0];
    object.position.y = state.transform[1];
    object.position.z = state.transform[2];
    object.rotation.x = state.transform[3];
    object.rotation.y = state.transform[4];
    object.rotation.z = state.transform[5];
    object.scale.x = state.transform[6];
    object.scale.y = state.transform[7];
    object.scale.z = state.transform[8];
    state.unsaved = true;
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
    state.selectedObject = selected;
    syncTransformBuffers(state);
    if (selected.has_value() && state.stage && *selected < state.stage->objects().size()) {
        const auto& object = state.stage->objects()[*selected];
        const auto& style = render::objectStyle(object.kind, object.name);
        setStatus(state, std::string(object.name) + " — " + style.label +
                             " (" + object.kind + "/" + object.layer + ")");
    }
}

void selectObject(EditorState& state, std::optional<std::size_t> stageIndex) {
    state.syncingSelection = true;
    syncViewportSelection(state, stageIndex);
    state.syncingSelection = false;
}

void rememberMap(EditorState& state, const std::filesystem::path& path) {
    state.settings.pushRecentMap(path.string());
    state.settings.save();
}

void openMap(EditorState& state, const std::filesystem::path& path);
void openGame(EditorState& state, const std::filesystem::path& path);
void requestOpenMap(EditorState& state);
void requestOpenGame(EditorState& state);
void requestSave(EditorState& state);

void openMapImpl(EditorState& state, const std::filesystem::path& path) {
    state.stage = smg::StageArchive::openMapFile(path);
    state.zones = {state.stage->stageName()};
    state.selectedZone = 0;
    state.filter.clear();
    state.searchBuf[0] = '\0';
    state.lastFilter.clear();
    refreshObjects(state);
    selectObject(state, std::nullopt);
    refreshViewport(state, true);
    rememberMap(state, path);
    state.unsaved = false;
    pushToast(state, "Opened map archive with " +
                         std::to_string(state.stage->objects().size()) + " objects.");
}

void openGameImpl(EditorState& state, const std::filesystem::path& path) {
    state.game.emplace(path);
    if (state.game->gameType() == 0) {
        state.game.reset();
        throw std::runtime_error("That folder is not an SMG1/SMG2 workspace");
    }
    state.galaxies = state.game->galaxies();
    state.zones = state.game->zones();
    state.stage.reset();
    state.selectedGalaxy = -1;
    state.selectedZone = -1;
    state.selectedObject.reset();
    state.settings.lastGameDir = path.string();
    state.settings.save();
    refreshViewport(state, false);
    pushToast(state, "Opened SMG" + std::to_string(state.game->gameType()) +
                         " workspace with " + std::to_string(state.galaxies.size()) +
                         " galaxies.");
}

void selectGalaxy(EditorState& state, int index) {
    if (!state.game || index < 0 || static_cast<std::size_t>(index) >= state.galaxies.size()) {
        return;
    }
    state.selectedGalaxy = index;
    const auto galaxy = state.game->openGalaxy(state.galaxies[static_cast<std::size_t>(index)]);
    state.zones = galaxy.zones();
    state.selectedZone = -1;
    setStatus(state, "Galaxy " + galaxy.name() + " has " +
                         std::to_string(state.zones.size()) + " zones.");
}

void selectZone(EditorState& state, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= state.zones.size()) {
        return;
    }
    state.selectedZone = index;
    const auto& zone = state.zones[static_cast<std::size_t>(index)];
    if (state.game) {
        state.stage = smg::StageArchive::open(state.game->filesystem(), zone,
                                              state.game->gameType());
    }
    state.filter.clear();
    state.searchBuf[0] = '\0';
    state.lastFilter.clear();
    refreshObjects(state);
    selectObject(state, std::nullopt);
    refreshViewport(state, true);
    if (state.stage) {
        pushToast(state, "Loaded " + zone + " (" +
                             std::to_string(state.stage->objects().size()) + " objects).");
    }
}

void saveStage(EditorState& state) {
    if (!state.stage) {
        pushToast(state, "No zone is loaded.", true);
        return;
    }
    applyTransform(state);
    state.stage->save();
    state.unsaved = false;
    pushToast(state, "Saved " + state.stage->sourcePath().string());
}

// --- UI panels ---------------------------------------------------------------

// Category chip in the object list, matching the viewport's color language.
ImVec4 categoryColor(const smg::PlacementObject& object) {
    const auto& style = render::objectStyle(object.kind, object.name);
    return ImVec4(style.color[0], style.color[1], style.color[2], 1.0F);
}

void drawGalaxyZonePanel(EditorState& state) {
    if (!state.showProject) {
        return;
    }
    if (!ImGui::Begin("Project", &state.showProject)) {
        ImGui::End();
        return;
    }
    if (state.game.has_value()) {
        ImGui::TextDisabled("SMG%d workspace", state.game->gameType());
    } else {
        ImGui::TextDisabled("No game directory open");
    }

    // Galaxies section
    ImGui::SeparatorText("Galaxies");
    if (ImGui::BeginListBox("##galaxies", ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8))) {
        for (std::size_t i = 0; i < state.galaxies.size(); ++i) {
            const std::string label = state.galaxyNames.displayName(state.galaxies[i]);
            const bool selected = state.selectedGalaxy == static_cast<int>(i);
            if (ImGui::Selectable(label.c_str(), selected)) {
                try {
                    selectGalaxy(state, static_cast<int>(i));
                } catch (const std::exception& error) {
                    pushToast(state, error.what(), true);
                }
            }
        }
        ImGui::EndListBox();
    }

    // Zones section
    ImGui::SeparatorText("Zones");
    if (ImGui::BeginListBox("##zones", ImVec2(-FLT_MIN, -1.0F))) {
        for (std::size_t i = 0; i < state.zones.size(); ++i) {
            std::string label = state.zoneNames.displayName(state.zones[i]);
            if (label != state.zones[i]) {
                label += "  [" + state.zones[i] + "]";
            }
            const bool selected = state.selectedZone == static_cast<int>(i);
            if (ImGui::Selectable(label.c_str(), selected)) {
                try {
                    selectZone(state, static_cast<int>(i));
                } catch (const std::exception& error) {
                    pushToast(state, error.what(), true);
                }
            }
        }
        ImGui::EndListBox();
    }
    ImGui::End();
}

void drawObjectsPanel(EditorState& state) {
    if (!state.showObjects) {
        return;
    }
    if (!ImGui::Begin("Objects", &state.showObjects)) {
        ImGui::End();
        return;
    }
    if (state.focusSearch) {
        ImGui::SetWindowFocus();
        state.focusSearch = false;
    }

    // Filter box with a clear button; filtering reruns only when the text
    // actually changes so 10k-object stages stay smooth.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0F);
    if (ImGui::InputTextWithHint("##search", "Search objects… (Ctrl+F)", state.searchBuf,
                                 sizeof(state.searchBuf))) {
        state.filter = state.searchBuf;
    }
    ImGui::SameLine();
    if (ImGui::Button("X##clear", ImVec2(24, 0))) {
        state.searchBuf[0] = '\0';
        state.filter.clear();
    }
    if (state.filter != state.lastFilter) {
        refreshObjects(state);
        state.lastFilter = state.filter;
    }
    ImGui::TextDisabled("%d of %d", static_cast<int>(state.visibleObjects.size()),
                        state.stage ? static_cast<int>(state.stage->objects().size()) : 0);
    ImGui::Separator();

    if (!state.stage) {
        ImGui::TextDisabled("Open a zone to list its objects.");
        ImGui::End();
        return;
    }

    const auto& objects = state.stage->objects();
    ImGui::BeginChild("##objectlist");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.visibleObjects.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::size_t stageIndex = state.visibleObjects[static_cast<std::size_t>(row)];
            const auto& object = objects[stageIndex];
            const bool selected = state.selectedObject == stageIndex;
            // Category color chip.
            ImGui::PushStyleColor(ImGuiCol_Text, categoryColor(object));
            ImGui::TextUnformatted("*");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            std::string label = object.name;
            const std::string friendly = state.objectDb.displayName(object.name);
            if (friendly != "\"" + object.name + "\"") {
                label += "  (" + friendly + ")";
            }
            ImGui::PushID(static_cast<int>(stageIndex));
            if (ImGui::Selectable(label.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selectObject(state, stageIndex);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && state.viewportReady) {
                    state.viewport.frameSelection();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void drawPropertiesPanel(EditorState& state) {
    if (!state.showProperties) {
        return;
    }
    if (!ImGui::Begin("Properties", &state.showProperties)) {
        ImGui::End();
        return;
    }
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        ImGui::TextDisabled("Select an object to edit its transform.");
        ImGui::End();
        return;
    }
    const auto& object = state.stage->objects()[*state.selectedObject];

    // Identity header: friendly name from the object database.
    const std::string friendly = state.objectDb.displayName(object.name);
    if (friendly != "\"" + object.name + "\"") {
        ImGui::TextUnformatted(friendly.c_str());
    }
    ImGui::TextDisabled("%s / %s", object.kind.c_str(), object.layer.c_str());
    ImGui::SeparatorText("Name");
    if (ImGui::InputText("##name", state.nameBuf, sizeof(state.nameBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        applyTransform(state);
    }

    auto vecRow = [&](const char* label, float* values, float speed) {
        ImGui::SeparatorText(label);
        ImGui::PushID(label);
        const float itemWidth = (ImGui::GetContentRegionAvail().x - 24.0F) / 3.0F;
        bool changed = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != 0) {
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(itemWidth);
            changed |= ImGui::DragFloat(("##v" + std::to_string(axis)).c_str(),
                                        &values[axis], speed, 0.0F, 0.0F, "%.1f");
        }
        if (changed) {
            applyTransform(state);
        }
        ImGui::PopID();
    };

    vecRow("Position", &state.transform[0], 0.5F);
    vecRow("Rotation", &state.transform[3], 0.25F);
    vecRow("Scale", &state.transform[6], 0.02F);

    ImGui::Separator();
    const float half = (ImGui::GetContentRegionAvail().x - 8.0F) / 2.0F;
    if (ImGui::Button("Reset", ImVec2(half, 0))) {
        syncTransformBuffers(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Zone", ImVec2(-1, 0))) {
        try {
            saveStage(state);
        } catch (const std::exception& error) {
            pushToast(state, error.what(), true);
        }
    }
    if (state.unsaved) {
        ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.35F, 1.0F), "* Unsaved changes");
    }
    ImGui::End();
}

// Keeps the hosted WGL viewport child window aligned with its docked ImGui
// window. The viewport handles its own input, so ImGui never sees mouse events
// while the cursor is over it (exactly what a 3D viewport needs).
void placeViewportChild(EditorState& state) {
    if (!state.viewportReady || !state.showViewport) {
        return;
    }
    if (!ImGui::Begin("Viewport", &state.showViewport)) {
        ImGui::End();
        return;
    }
    if (!state.stage) {
        ImGui::TextDisabled("No zone loaded.");
        ImGui::TextDisabled("File > Open Map Archive... or drop a .arc file.");
        if (ImGui::Button("Open Map...")) {
            requestOpenMap(state);
        }
        ImGui::End();
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::End();
    if (size.x < 8 || size.y < 8) {
        return;
    }
    const int x = static_cast<int>(origin.x);
    const int y = static_cast<int>(origin.y);
    const int w = static_cast<int>(size.x);
    const int h = static_cast<int>(size.y);
    RECT existing{};
    if (GetWindowRect(state.viewport.handle(), &existing)) {
        if (existing.left != x || existing.top != y ||
            existing.right - existing.left != w || existing.bottom - existing.top != h) {
            MoveWindow(state.viewport.handle(), x, y, w, h, TRUE);
        }
    }
}

// --- Shell: menu bar, toolbar, dockspace, status bar --------------------------
// These turn the floating demo windows into one docked editor workspace with
// a real File/Edit/View/Settings/Help menu and a quick-action toolbar.

void requestOpenMap(EditorState& state) {
    if (auto picked = pickOpenFile(state.window); picked.has_value()) {
        try {
            openMap(state, *picked);
        } catch (const std::exception& error) {
            pushToast(state, error.what(), true);
            state.showLog = true;
        }
    }
}

void requestOpenGame(EditorState& state) {
    if (auto picked = pickFolder(state.window); picked.has_value()) {
        try {
            openGame(state, *picked);
        } catch (const std::exception& error) {
            pushToast(state, error.what(), true);
            state.showLog = true;
        }
    }
}

void requestSave(EditorState& state) {
    try {
        saveStage(state);
    } catch (const std::exception& error) {
        pushToast(state, error.what(), true);
        state.showLog = true;
    }
}

void openMap(EditorState& state, const std::filesystem::path& path) {
    openMapImpl(state, path);
}

void openGame(EditorState& state, const std::filesystem::path& path) {
    openGameImpl(state, path);
}

void drawMenuBar(EditorState& state, bool& done) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Map Archive...", "Ctrl+O")) {
            requestOpenMap(state);
        }
        if (ImGui::MenuItem("Open Game Directory...")) {
            requestOpenGame(state);
        }
        if (ImGui::BeginMenu("Recent Maps")) {
            if (state.settings.recentMaps.empty()) {
                ImGui::MenuItem("(none yet)", nullptr, false, false);
            }
            for (const auto& recent : state.settings.recentMaps) {
                if (ImGui::MenuItem(recent.c_str())) {
                    try {
                        openMap(state, std::filesystem::path(recent));
                    } catch (const std::exception& error) {
                        pushToast(state, error.what(), true);
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        const bool canSave = state.stage.has_value();
        if (ImGui::MenuItem("Save Zone", "Ctrl+S", false, canSave)) {
            requestSave(state);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            done = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Focus Search", "Ctrl+F", false, state.stage.has_value())) {
            state.focusSearch = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Object Transform", nullptr, false,
                            state.selectedObject.has_value())) {
            syncTransformBuffers(state);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Toolbar", nullptr, &state.showToolbar);
        ImGui::MenuItem("Status Bar", nullptr, &state.showStatusBar);
        ImGui::Separator();
        ImGui::MenuItem("Project", nullptr, &state.showProject);
        ImGui::MenuItem("Objects", nullptr, &state.showObjects);
        ImGui::MenuItem("Properties", nullptr, &state.showProperties);
        ImGui::MenuItem("3D Viewport", nullptr, &state.showViewport);
        ImGui::MenuItem("Log", nullptr, &state.showLog);
        ImGui::Separator();
        ImGui::MenuItem("Object Labels", nullptr, &state.showLabels);
        ImGui::Separator();
        ImGui::TextDisabled("Overlays");
        ImGui::MenuItem("Axis", nullptr, &state.settings.showAxis);
        ImGui::MenuItem("Cameras", nullptr, &state.settings.showCameras);
        ImGui::MenuItem("Paths", nullptr, &state.settings.showPaths);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Whitehole Pro")) {
            state.showAbout = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void drawToasts(EditorState& state) {
    const double now = ImGui::GetTime();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Bottom-right card stack above the status bar.
    float y = viewport->WorkPos.y + viewport->WorkSize.y - 56.0F;
    for (std::size_t i = state.toasts.size(); i-- > 0;) {
        const auto& toast = state.toasts[i];
        const double age = now - toast.born;
        if (age > 4.0) {
            state.toasts.erase(state.toasts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const float alpha = static_cast<float>(
            age < 3.5 ? 1.0 : 1.0 - (age - 3.5) / 0.5);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 16.0F, y),
                                ImGuiCond_Always, ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.90F * alpha);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing;
        char title[16];
        std::snprintf(title, sizeof(title), "##toast%zu", i);
        if (ImGui::Begin(title, nullptr, flags)) {
            if (toast.error) {
                ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.45F, alpha), "%s", toast.text.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.31F, 0.76F, 0.97F, alpha), "%s", toast.text.c_str());
            }
        }
        ImGui::End();
        y -= ImGui::GetFrameHeight() * 2.2F;
    }
}

void drawToolbar(EditorState& state) {
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, 4.0F));
    if (ImGui::Begin("##toolbar", nullptr, flags)) {
        if (ImGui::Button("Open")) {
            requestOpenMap(state);
        }
        ImGui::SameLine();
        if (ImGui::Button("Game")) {
            requestOpenGame(state);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!state.stage.has_value());
        if (ImGui::Button("Save")) {
            requestSave(state);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::BeginDisabled(!state.viewportReady);
        if (ImGui::Button("Frame All")) {
            state.viewport.frameAll();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Labels", &state.showLabels)) {
            refreshViewport(state, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::TextDisabled("Search:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::InputTextWithHint("##toolbar-search", "Filter objects...",
                                     state.searchBuf, sizeof(state.searchBuf))) {
            state.filter = state.searchBuf;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void drawStatusBar(EditorState& state) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight() + 8.0F;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                   viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##statusbar", nullptr, flags)) {
        if (state.unsaved) {
            ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.35F, 1.0F), "*");
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", state.statusText.c_str());
        // Right-aligned context: game type, object count, selection.
        ImGui::SameLine();
        const float rightWidth = 340.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImGui::GetContentRegionAvail().x - rightWidth);
        std::string context;
        if (state.game) {
            context += "SMG" + std::to_string(state.game->gameType()) + "  |  ";
        }
        if (state.stage) {
            context += std::to_string(state.stage->objects().size()) + " objects";
            if (state.selectedObject) {
                context += "  |  sel #" + std::to_string(*state.selectedObject);
            }
        } else {
            context += "No zone loaded";
        }
        ImGui::TextDisabled("%s", context.c_str());
    }
    ImGui::End();
}

void drawAboutDialog(EditorState& state) {
    if (!state.showAbout) {
        return;
    }
    ImGui::OpenPopup("About Whitehole Pro");
    if (ImGui::BeginPopupModal("About Whitehole Pro", &state.showAbout,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Whitehole Pro  (C++ rewrite)");
        ImGui::TextDisabled("Super Mario Galaxy 1 / 2 stage editor");
        ImGui::Separator();
        ImGui::BulletText("File > Open Map Archive...  (.arc / .szs)");
        ImGui::BulletText("File > Open Game Directory...  (extracted workspace)");
        ImGui::BulletText("Drag & drop a map archive onto the window");
        ImGui::Separator();
        if (ImGui::Button("Close")) {
            state.showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// drawStaleStatusBar: superseded by the context-aware drawStatusBar above.
// Kept temporarily so the old call sites keep compiling during the shell
// rework; delete once the dock-host loop is the only caller.
void drawStaleStatusBar(EditorState& state) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight() + 8.0F;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                   viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##statusbar", nullptr, flags)) {
        if (state.unsaved) {
            ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.35F, 1.0F), "*");
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", state.statusText.c_str());
    }
    ImGui::End();
}

void drawRealLogWindow(EditorState& state) {
    if (!state.showLog) {
        return;
    }
    if (!ImGui::Begin("Log", &state.showLog)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) {
        state.logLines.clear();
    }
    ImGui::BeginChild("##logscroll");
    for (const auto& line : state.logLines) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();
    ImGui::End();
}

// CHUNK-SENTINEL-1

} // namespace (anonymous: DX11 plumbing, EditorState, panel drawing)

// ---- Desktop editor entry point ----
// Called by runCli("gui") and winmain.cpp. Hosts the full ImGui + Win32 + DX11
// desktop editor loop.

int runGui(const std::filesystem::path& /*executable*/, const std::filesystem::path& initialFile);

// Forward declaration: WndProc is defined after runGui.
LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// ---- Desktop editor entry point ----
// Called by runCli("gui") and winmain.cpp. Hosts the full ImGui + Win32 + DX11
// desktop editor loop.

int runGui(const std::filesystem::path& /*executable*/, const std::filesystem::path& initialFile) {
    using namespace whitehole::app;

    Settings settings;
    settings.load();
    const float dpiScale = dpiScaleFactor();
    // NOTE: applyWhiteholeTheme must NOT be called here — it needs an ImGui
    // context (created below). Calling ImGui::GetStyle() with no context is a
    // null-pointer crash that silently kills the app on boot (WIN32 subsystem
    // shows no console). Theme is applied after CreateContext instead.

    HINSTANCE instance = GetModuleHandleW(nullptr);

    // --- Register window class ---
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    if (wc.hIcon == nullptr) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.lpszClassName = L"WhiteholePro";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) {
        showBootError(L"Could not register the WhiteholePro window class.");
        return 1;
    }

    // --- Create main window ---
    HWND hwnd = CreateWindowExW(0, L"WhiteholePro", L"Whitehole Pro",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        showBootError(L"Could not create the main editor window.");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // --- Init D3D11 + ImGui ---
    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        DestroyWindow(hwnd);
        // createDeviceD3D already showed the HRESULT dialog.
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // NOTE: multi-viewport (tearing windows off into OS windows) is OFF on
    // purpose — it is what made panels float around as separate windows.
    io.IniFilename = nullptr; // layout rebuilt by DockBuilder each launch
    applyWhiteholeTheme(settings.darkMode, dpiScale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // --- Editor state ---
    EditorState state;
    state.settings = settings;
    state.window = hwnd;

    // --- Load initial file if provided ---
    if (!initialFile.empty()) {
        try {
            if (std::filesystem::is_directory(initialFile)) {
                state.game.emplace(initialFile);
                if (state.game->gameType() == 0) {
                    state.game.reset();
                    pushToast(state, "That folder is not an SMG1/SMG2 workspace.", true);
                } else {
                    state.galaxies = state.game->galaxies();
                    state.zones = state.game->zones();
                    state.settings.lastGameDir = initialFile.string();
                    state.settings.save();
                    pushToast(state, "Opened SMG" + std::to_string(state.game->gameType()) +
                                    " workspace with " + std::to_string(state.galaxies.size()) + " galaxies.");
                }
            } else {
                openMap(state, initialFile);
            }
        } catch (const std::exception& e) {
            pushToast(state, e.what(), true);
        }
    }

    // --- Main message loop ---
    MSG msg{};
    bool done = false;
    while (!done) {
        // Pump Win32 messages
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                done = true;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done) break;

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // --- Shell: menu, toolbar, dockspace ---
        drawMenuBar(state, done);
        if (done) break;
        if (state.showToolbar) {
            drawToolbar(state);
        }

        // Fullscreen dock host. Built once via DockBuilder so the first run
        // already looks like an editor: project left, properties right,
        // viewport center, log drawer bottom.
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainViewport->WorkPos);
        ImGui::SetNextWindowSize(mainViewport->WorkSize);
        ImGui::SetNextWindowViewport(mainViewport->ID);
        const ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("##dockhost", nullptr, hostFlags);
        ImGui::PopStyleVar();
        const ImGuiID dockSpaceId = ImGui::GetID("WhiteholeDockSpace");
        ImGui::DockSpace(dockSpaceId, ImVec2(0.0F, 0.0F),
                         ImGuiDockNodeFlags_PassthruCentralNode);
        if (state.firstFrame) {
            state.firstFrame = false;
            ImGui::DockBuilderRemoveNode(dockSpaceId);
            ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockSpaceId, mainViewport->WorkSize);
            ImGuiID dockLeft = 0, dockCenter = 0, dockRight = 0, dockBottom = 0;
            ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Left, 0.20F,
                                        &dockLeft, &dockCenter);
            ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.28F,
                                        &dockRight, &dockCenter);
            ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.24F,
                                        &dockBottom, &dockCenter);
            ImGui::DockBuilderDockWindow("Project", dockLeft);
            ImGui::DockBuilderDockWindow("Objects", dockLeft);
            ImGui::DockBuilderDockWindow("Viewport", dockCenter);
            ImGui::DockBuilderDockWindow("Properties", dockRight);
            ImGui::DockBuilderDockWindow("Log", dockBottom);
            ImGui::DockBuilderFinish(dockSpaceId);
        }
        ImGui::End();

        // --- Panels ---
        drawGalaxyZonePanel(state);
        drawObjectsPanel(state);
        drawPropertiesPanel(state);
        placeViewportChild(state);
        if (state.showLog) {
            drawRealLogWindow(state);
        }
        drawAboutDialog(state);
        drawToasts(state);
        if (state.showStatusBar) {
            drawStatusBar(state);
        }

        // --- Keyboard shortcuts ---
        const bool ctrlDown =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrlDown && (GetAsyncKeyState('O') & 1)) {
            requestOpenMap(state);
        }
        if (ctrlDown && (GetAsyncKeyState('S') & 1) && state.stage) {
            requestSave(state);
        }
        if (ctrlDown && (GetAsyncKeyState('F') & 1)) {
            state.focusSearch = true;
            state.showObjects = true;
        }
        if ((GetAsyncKeyState('F') & 1) && !ctrlDown && state.viewportReady &&
            state.selectedObject) {
            state.viewport.frameSelection();
        }

        // --- Rendering ---
        ImGui::Render();
        const float clearColor[4] = {0.11f, 0.11f, 0.12f, 1.0f};

        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapChain->Present(1, 0); // VSync
    }

    // --- Cleanup ---
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(hwnd);

    state.settings.save();
    return 0;
}

// ---- Window procedure ----
// Handles Win32 messages forwarded to ImGui, plus resize/backbuffer recreation
// and window destruction.

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return 1;
    }
    switch (message) {
        case WM_SIZE:
            if (g_swapChain != nullptr) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)),
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0);
                createRenderTarget();
            }
            return 0;
        case WM_DESTROY:
            cleanupDeviceD3D();
            PostQuitMessage(0);
            return 0;
        case WM_DPICHANGED:
            if (g_swapChain != nullptr) {
                cleanupRenderTarget();
                const RECT* rect = reinterpret_cast<RECT*>(lParam);
                g_swapChain->ResizeBuffers(0, static_cast<UINT>(rect->right - rect->left),
                                           static_cast<UINT>(rect->bottom - rect->top),
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0);
                createRenderTarget();
            }
            return 0;
        case WM_CREATE:
            DragAcceptFiles(window, TRUE);
            return 0;
        case WM_DROPFILES: {
            wchar_t buffer[MAX_PATH];
            if (DragQueryFileW(reinterpret_cast<HDROP>(wParam), 0, buffer, MAX_PATH) > 0) {
                DragFinish(reinterpret_cast<HDROP>(wParam));
                const std::filesystem::path dropped = buffer;
                if (std::filesystem::exists(dropped)) {
                    MessageBoxW(window,
                                (utf8ToWide(dropped.string())).c_str(),
                                L"Whitehole Pro", MB_OK | MB_ICONINFORMATION);
                }
            }
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace whitehole::app
