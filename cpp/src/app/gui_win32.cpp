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
#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/db/name_table.hpp"
#include "whitehole/db/object_db.hpp"
#include "whitehole/edit/commands.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/render/model_library.hpp"
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
    db::ModelSubstitutions modelSubstitutions;
    render::ModelLibrary modelLibrary;
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
    // create() failed (no OpenGL): the panel shows an explanation instead of a
    // blank hole, and every camera/picking action stays disabled.
    bool viewportCreateFailed{false};
    bool syncingSelection{false};
    bool showLabels{false};

    // Layout + visibility for the hosted WGL child window. The child is a real
    // HWND, so it always paints above the ImGui framebuffer: `placeViewportChild`
    // records where it should go, and `syncViewportChild` applies position and
    // show/hide in one place so nothing can fight over it.
    RECT viewportRect{};
    bool viewportRectValid{false};
    bool viewportChildVisible{false};
    // The rectangle last handed to MoveWindow. Comparing against this (rather
    // than GetWindowRect, which reports *screen* coordinates for a child window
    // while viewportRect is parent-client) is what keeps a settled viewport from
    // being re-positioned, and its GL surface re-allocated, every single frame.
    RECT viewportRectApplied{};

    // --- UI bookkeeping -----------------------------------------------------
    int selectedGalaxy{-1};
    int selectedZone{-1};
    std::optional<std::size_t> selectedObject;
    char searchBuf[160]{};   // object list filter
    char nameBuf[160]{};     // selected object name (committed on Enter)
    float transform[9]{};    // pos.xyz, rot.xyz, scale.xyz (display values)
    // The object the widgets started from. While a drag is in flight `transform`
    // may differ from it, which is how the panel knows it has local edits; when
    // a drag commits, the before/after pair becomes one TransformCommand.
    smg::PlacementObject dragStart{};
    bool draggingTransform{false};
    bool unsaved{false};

    // --- Undo: one stack per editor session, cleared on load -----------------
    edit::UndoStack undoStack;

    // --- Tutorials panel (Help > Tutorials; first run opens it) ------------
    bool showTutorials{false};
    char tutorialSearch[96]{};
    int tutorialTopic{-1};
    bool tutorialsSeen{false}; // persisted via a marker file, like first boot

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
    // True on first boot (no layout.ini yet) or after View > Reset Layout: the
    // default dock arrangement is rebuilt once via DockBuilder, then ImGui's
    // .ini persistence keeps the user's arrangement across launches.
    bool buildDefaultLayout{false};
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

// Monitor scale of the main window. It can only be resolved once the HWND
// exists (before that the process has no monitor to ask), so it starts at 1.0
// and is filled in during runGui's boot. Menu items and dialogs re-apply the
// theme through dpiScaleFactor() so toggling dark mode never resets scaling.
float g_dpiScale = 1.0F;

float dpiScaleFactor() {
    return g_dpiScale;
}

// Slurps a font file into a process-lifetime buffer. The atlas is handed a
// pointer into this memory (FontDataOwnedByAtlas stays false), so it has to
// outlive the atlas — a function-local static is freed at process exit.
bool readFontBytes(const std::filesystem::path& path, std::vector<unsigned char>& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return file.gcount() == static_cast<std::streamsize>(out.size());
}

// Loads the UI font once, at a readable base size. Prefer the bundled Inter
// (data/fonts, copied next to the executable), fall back to Segoe UI from the
// Windows font directory, and finally to ImGui's embedded vector font so text
// is legible even from a checkout with no data folder. Size scaling for
// high-DPI monitors is left to ImGui (io.ConfigDpiScaleFonts), which re-bakes
// glyphs when the window moves between displays.
void loadEditorFonts(ImGuiIO& io, const std::filesystem::path& dataRoot) {
    static std::vector<unsigned char> fontBytes; // must outlive io.Fonts
    constexpr float kBaseFontSize = 16.0F;

    std::vector<std::filesystem::path> candidates;
    if (!dataRoot.empty()) {
        candidates.push_back(dataRoot / "fonts" / "Inter.ttf");
    }
    std::array<wchar_t, MAX_PATH> windowsDir{};
    if (GetWindowsDirectoryW(windowsDir.data(), MAX_PATH) != 0) {
        candidates.push_back(std::filesystem::path(windowsDir.data()) / "Fonts" / "segoeui.ttf");
    }

    for (const auto& path : candidates) {
        if (!readFontBytes(path, fontBytes)) {
            continue;
        }
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        config.FontDataOwnedByAtlas = false; // fontBytes owns the memory
        if (io.Fonts->AddFontFromMemoryTTF(fontBytes.data(),
                                           static_cast<int>(fontBytes.size()),
                                           kBaseFontSize, &config) != nullptr) {
            return;
        }
    }
    io.Fonts->AddFontDefaultVector();
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
    state.viewport.setShowLabels(state.showLabels);
    if (frame) {
        state.viewport.frameAll();
    } else {
        state.viewport.invalidate();
    }
}

// Copies the selected object into the property widgets. This is also the
// "committed" marker: drag edits always start from these exact values, so undo
// can restore them faithfully.
void syncTransformBuffers(EditorState& state) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        state.draggingTransform = false;
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
    state.dragStart = object;
    state.draggingTransform = false;
}

// True while the widgets hold values that were never written back to the stage.
bool transformWidgetsDirty(const EditorState& state) {
    if (!state.draggingTransform || !state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        return false;
    }
    return true;
}

// Pushes the property widgets into the selected object through the real write
// path (BCSV row, not just the in-memory copy), then repaints the scene.
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
    // The BCSV rows are the source of truth that save() serialises; writing the
    // row here (instead of only mutating the placement copy) is what makes
    // edits survive a reopen.
    state.stage->writeObject(object);
    state.stage->rebuildObjects();
    refreshObjects(state);
    state.unsaved = true;
    refreshViewport(state, false);
}

// Finishes an in-flight drag as one undoable step. Drags paint live (so the 3D
// model follows the cursor) but only the before/after snapshots land on the
// undo stack, keeping Ctrl+Z behaviour exactly one gesture per step.
void commitDragAsUndo(EditorState& state, const char* label) {
    if (!state.draggingTransform || !state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        state.draggingTransform = false;
        return;
    }
    auto after = state.stage->objects()[*state.selectedObject];
    auto command = std::make_unique<edit::TransformCommand>(*state.stage,
        std::vector<smg::PlacementObject>{state.dragStart},
        std::vector<smg::PlacementObject>{after}, label);
    state.undoStack.push(std::move(command));
    state.dragStart = after; // the new baseline for the next gesture
    state.draggingTransform = false;
}

// Perfoms undo()/redo() then rebuilds every dependent view from the tables,
// because TransformCommand mutates BCSV rows directly.
void performUndo(EditorState& state) {
    if (!state.undoStack.undo()) {
        return;
    }
    if (state.stage) {
        state.stage->rebuildObjects();
        refreshObjects(state);
        syncTransformBuffers(state);
        state.unsaved = true;
        refreshViewport(state, false);
        pushToast(state, "Undid " + state.undoStack.redoLabel() + ".");
    }
}

void performRedo(EditorState& state) {
    if (!state.undoStack.redo()) {
        return;
    }
    if (state.stage) {
        state.stage->rebuildObjects();
        refreshObjects(state);
        syncTransformBuffers(state);
        state.unsaved = true;
        refreshViewport(state, false);
        pushToast(state, "Redid " + state.undoStack.undoLabel() + ".");
    }
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
    state.undoStack.clear(); // a new map means a new history
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
        state.modelLibrary.bind(nullptr);
        throw std::runtime_error("That folder is not an SMG1/SMG2 workspace");
    }
    // ObjectData model archives live in the game workspace; bind them so the
    // viewport can show the real BMD models.
    state.modelLibrary.setSubstitutions(&state.modelSubstitutions);
    state.modelLibrary.setLowPoly(state.settings.lowPolyModels);
    state.modelLibrary.bind(&state.game->filesystem());
    state.galaxies = state.game->galaxies();
    state.zones = state.game->zones();
    state.stage.reset();
    state.selectedGalaxy = -1;
    state.selectedZone = -1;
    state.selectedObject.reset();
    state.undoStack.clear(); // a new workspace means a new history
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
    state.undoStack.clear(); // a new zone means a new history
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", state.settings.lastGameDir.c_str());
        }
    } else {
        ImGui::TextDisabled("No game directory open");
    }
    if (ImGui::Button("Open Game...")) {
        requestOpenGame(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Map...")) {
        requestOpenMap(state);
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
            if (ImGui::IsItemHovered()) {
                const auto& style = render::objectStyle(object.kind, object.name);
                ImGui::SetTooltip("%s  [%s / %s]", style.label,
                                  object.kind.c_str(), object.layer.c_str());
            }
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
            // Right-click: focus in the 3D view or discard local edits.
            if (ImGui::BeginPopupContextItem("##objctx")) {
                if (ImGui::MenuItem("Focus in viewport", "F", false, state.viewportReady)) {
                    selectObject(state, stageIndex);
                    state.viewport.frameSelection();
                }
                if (ImGui::MenuItem("Reset transform")) {
                    selectObject(state, stageIndex);
                    syncTransformBuffers(state);
                }
                ImGui::EndPopup();
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Object #%zu", *state.selectedObject);
    }
    ImGui::SeparatorText("Name");
    if (ImGui::InputTextWithHint("##name", "Object name (Enter to apply)", state.nameBuf,
                                 sizeof(state.nameBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        applyTransform(state);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Renames the object. Press Enter to commit.");
    }

    auto vecRow = [&](const char* label, const char* hint, float* values, float speed,
                      const char* undoLabel) {
        // Axis letter + hint per field; the drag widget itself carries the
        // tooltip so it appears exactly where the user is working.
        static constexpr const char* kAxes[3] = {"X", "Y", "Z"};
        ImGui::SeparatorText(label);
        ImGui::PushID(label);
        const float itemWidth = (ImGui::GetContentRegionAvail().x - 24.0F) / 3.0F;
        bool changed = false;
        bool active = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != 0) {
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(itemWidth);
            changed |= ImGui::DragFloat((std::string("##v") + std::to_string(axis)).c_str(),
                                        &values[axis], speed, 0.0F, 0.0F, "%.3f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s %s\n%s", label, kAxes[axis], hint);
            } else if (ImGui::IsItemActive()) {
                // Live readout while dragging, so values are readable even when
                // the field is too narrow to show all digits.
                ImGui::SetTooltip("%s %s = %.3f", label, kAxes[axis],
                                  static_cast<double>(values[axis]));
            }
            active |= ImGui::IsItemActive();
        }
        if (active && !state.draggingTransform) {
            state.draggingTransform = true; // a gesture started: baseline is dragStart
        }
        if (changed) {
            applyTransform(state);
        }
        // Releasing the mouse (or committing a typed value) finishes the gesture
        // as a single undoable step.
        if (!active && state.draggingTransform) {
            commitDragAsUndo(state, undoLabel);
        }
        ImGui::PopID();
    };

    vecRow("Position", "World position in game units. Drag or double-click a field to type.",
           &state.transform[0], 0.5F, "Move object");
    vecRow("Rotation", "Euler rotation in degrees.", &state.transform[3], 0.25F,
           "Rotate object");
    vecRow("Scale", "Per-axis scale. Double-click a field to type an exact value.",
           &state.transform[6], 0.02F, "Scale object");
    // A keyboard-committed value can finish the gesture after the last row,
    // so commit anything still in flight once no widget is active.
    if (!ImGui::IsAnyItemActive() && state.draggingTransform) {
        commitDragAsUndo(state, "Edit object");
    }

    ImGui::Separator();
    const float half = (ImGui::GetContentRegionAvail().x - 8.0F) / 2.0F;
    if (ImGui::Button("Reset", ImVec2(half, 0))) {
        commitDragAsUndo(state, "Edit object"); // finish any gesture first
        syncTransformBuffers(state);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Discard unsaved edits to this object.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Zone", ImVec2(-1, 0))) {
        try {
            saveStage(state);
        } catch (const std::exception& error) {
            pushToast(state, error.what(), true);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Write the zone back to disk  (Ctrl+S)");
    }
    if (state.unsaved) {
        ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.35F, 1.0F), "* Unsaved changes");
    }
    ImGui::End();
}

// Creates the hosted WGL viewport child window and routes its picking back into
// the editor selection. Returns false when OpenGL cannot be initialised, in
// which case the Viewport panel explains why instead of showing a blank hole.
bool initViewport(EditorState& state, HINSTANCE instance) {
    if (!state.viewport.create(state.window, 1001, instance)) {
        state.viewportCreateFailed = true;
        pushToast(state, "The 3D viewport could not start (OpenGL unavailable).", true);
        return false;
    }
    state.viewportReady = true;
    // Picking happens inside the viewport's own message handling, so it must not
    // re-enter the selection helpers in the middle of their own work.
    state.viewport.setOnSelect([&state](std::optional<std::size_t> picked) {
        if (state.syncingSelection) {
            return;
        }
        state.syncingSelection = true;
        state.selectedObject = picked;
        state.viewportSelected = picked;
        state.viewport.setSelected(picked);
        syncTransformBuffers(state);
        state.syncingSelection = false;
        if (picked.has_value() && state.stage && *picked < state.stage->objects().size()) {
            const auto& object = state.stage->objects()[*picked];
            const auto& style = render::objectStyle(object.kind, object.name);
            setStatus(state, std::string(object.name) + " — " + style.label +
                                 " (" + object.kind + "/" + object.layer + ")");
        }
    });
    state.viewport.setShowLabels(state.showLabels);
    refreshViewport(state, true); // paint real content on the very first frame
    return true;
}

// Lays out the hosted WGL viewport child window inside its docked ImGui window.
// The viewport handles its own input, so ImGui never sees mouse events while the
// cursor is over it (exactly what a 3D viewport needs). This only records the
// target rectangle; `syncViewportChild` applies it.
void placeViewportChild(EditorState& state) {
    state.viewportRectValid = false;
    if (!state.showViewport) {
        return;
    }
    if (!ImGui::Begin("Viewport", &state.showViewport)) {
        ImGui::End();
        return;
    }
    if (state.viewportCreateFailed) {
        ImGui::TextDisabled("The 3D viewport could not start.");
        ImGui::TextWrapped("OpenGL was unavailable on this system, so the viewport "
                           "child window could not be created. Everything else in the "
                           "editor still works.");
        ImGui::End();
        return;
    }
    if (!state.stage) {
        ImGui::TextDisabled("No zone loaded.");
        ImGui::TextWrapped("Open a map archive or a game directory to place objects here.");
        if (ImGui::Button("Open Map...")) {
            requestOpenMap(state);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Game...")) {
            requestOpenGame(state);
        }
        ImGui::End();
        return;
    }
    // Compact camera strip drawn *above* the child window: the child covers its
    // own rectangle, so anything inside it would be invisible.
    if (ImGui::Button("Frame All")) {
        state.viewport.frameAll();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Labels", &state.showLabels)) {
        refreshViewport(state, false);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Left-drag pan  ·  Right-drag orbit  ·  Wheel zoom  ·  Click select");

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::End();
    if (size.x < 8.0F || size.y < 8.0F) {
        return;
    }
    state.viewportRect.left = static_cast<LONG>(origin.x);
    state.viewportRect.top = static_cast<LONG>(origin.y);
    state.viewportRect.right = static_cast<LONG>(origin.x + size.x);
    state.viewportRect.bottom = static_cast<LONG>(origin.y + size.y);
    state.viewportRectValid = true;
}

// Applies the rectangle recorded by placeViewportChild and decides whether the
// child window should be on screen at all.
//
// The child is a real HWND, so it paints above the ImGui framebuffer. That means
// an open menu, modal or context menu drawn over the viewport would be hidden
// behind it, so the child is temporarily hidden while any popup is up.
void syncViewportChild(EditorState& state) {
    if (!state.viewportReady) {
        return;
    }
    const bool popupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool wantVisible = state.viewportRectValid && !popupOpen;
    if (wantVisible) {
        const RECT& target = state.viewportRect;
        const RECT& applied = state.viewportRectApplied;
        if (target.left != applied.left || target.top != applied.top ||
            target.right != applied.right || target.bottom != applied.bottom) {
            MoveWindow(state.viewport.handle(), target.left, target.top,
                       target.right - target.left, target.bottom - target.top, TRUE);
            state.viewportRectApplied = target;
        }
    }
    if (wantVisible == state.viewportChildVisible) {
        return;
    }
    ShowWindow(state.viewport.handle(), wantVisible ? SW_SHOWNA : SW_HIDE);
    state.viewportChildVisible = wantVisible;
    if (wantVisible) {
        // A hidden GL surface comes back undefined, so ask for a fresh frame the
        // instant the child is revealed instead of waiting for an interaction.
        state.viewport.invalidate();
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
        const bool canUndo = state.undoStack.canUndo();
        const bool canRedo = state.undoStack.canRedo();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            performUndo(state);
        }
        if (ImGui::IsItemHovered() && canUndo) {
            ImGui::SetTooltip("%s", state.undoStack.undoLabel().c_str());
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
            performRedo(state);
        }
        if (ImGui::IsItemHovered() && canRedo) {
            ImGui::SetTooltip("%s", state.undoStack.redoLabel().c_str());
        }
        ImGui::Separator();
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
        if (ImGui::MenuItem("Reset Layout")) {
            state.buildDefaultLayout = true;
            pushToast(state, "The default panel layout will be restored.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Object Labels", nullptr, &state.showLabels)) {
            refreshViewport(state, false);
        }
        if (ImGui::MenuItem("Low-Poly Models", nullptr, &state.settings.lowPolyModels)) {
            state.modelLibrary.setLowPoly(state.settings.lowPolyModels);
            state.settings.save();
            refreshViewport(state, false);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Overlays");
        ImGui::TextWrapped("Renderer overlays (axis, areas, cameras, gravity, paths) "
                           "are part of the 3D viewport owned by the renderer — "
                           "those toggles will light up once that support lands.");
        if (ImGui::MenuItem("Axis", nullptr, &state.settings.showAxis)) {
            state.settings.save();
        }
        if (ImGui::MenuItem("Cameras", nullptr, &state.settings.showCameras)) {
            state.settings.save();
        }
        if (ImGui::MenuItem("Paths", nullptr, &state.settings.showPaths)) {
            state.settings.save();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Dark Theme", nullptr, &state.settings.darkMode)) {
            applyWhiteholeTheme(state.settings.darkMode, dpiScaleFactor());
            state.settings.save();
        }
        if (ImGui::MenuItem("Preferences...")) {
            state.showPreferences = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Tutorials")) {
            state.showTutorials = true;
            state.tutorialTopic = state.tutorialTopic < 0 ? 0 : state.tutorialTopic;
        }
        if (ImGui::MenuItem("Keyboard Shortcuts...")) {
            state.showShortcuts = true;
        }
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

// Toolbar row. Drawn *inside* the dock host so it reserves its own strip and can
// never overlap the panels (it used to be a free-floating window).
void drawToolbar(EditorState& state) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0F, 6.0F));
    ImGui::Dummy(ImVec2(4.0F, 0.0F)); // left inset
    ImGui::SameLine();
    if (ImGui::Button("Open Map")) {
        requestOpenMap(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Game")) {
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
    // Search fills the remaining width so the toolbar has no ragged gap.
    const float searchWidth = ImGui::GetContentRegionAvail().x - 8.0F;
    if (searchWidth > 120.0F) {
        ImGui::SetNextItemWidth(searchWidth);
    }
    if (ImGui::InputTextWithHint("##toolbar-search", "Search objects…  (Ctrl+F)",
                                 state.searchBuf, sizeof(state.searchBuf))) {
        state.filter = state.searchBuf;
    }
    ImGui::PopStyleVar();
}

void drawStatusBar(EditorState& state) {
    const float height = ImGui::GetFrameHeight() + 10.0F;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Full-bleed strip at the bottom of the host window.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::BeginChild("##statusstrip", ImVec2(viewport->WorkSize.x, height),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::SetCursorPosY(5.0F);
    ImGui::Dummy(ImVec2(8.0F, 0.0F));
    ImGui::SameLine();
    if (state.unsaved) {
        ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.35F, 1.0F), "*");
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(state.statusText.c_str());
    // Right-aligned context: game type, object count, selection.
    ImGui::SameLine();
    const float rightWidth = 400.0F;
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
        // Friend's model stats: how many real BMD models vs placeholders.
        if (state.modelLibrary.bound()) {
            context += "  |  models " + std::to_string(state.modelLibrary.loadedCount()) +
                       "/" + std::to_string(state.modelLibrary.missingCount()) +
                       " ph";
        }
    } else {
        context += "No zone loaded";
    }
    ImGui::TextDisabled("%s", context.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawAboutDialog(EditorState& state) {
    if (!state.showAbout) {
        return;
    }
    ImGui::OpenPopup("About Whitehole Pro");
    ImGui::SetNextWindowSize(ImVec2(420.0F, 0.0F), ImGuiCond_FirstUseEver);
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

void drawPreferencesDialog(EditorState& state) {
    if (!state.showPreferences) {
        return;
    }
    ImGui::OpenPopup("Preferences");
    ImGui::SetNextWindowSize(ImVec2(460.0F, 0.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Preferences", &state.showPreferences,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    bool changed = false;
    ImGui::SeparatorText("Appearance");
    if (ImGui::Checkbox("Dark theme", &state.settings.darkMode)) {
        applyWhiteholeTheme(state.settings.darkMode, dpiScaleFactor());
        changed = true;
    }
    ImGui::SeparatorText("Viewport");
    changed |= ImGui::Checkbox("Object labels", &state.showLabels);
    if (ImGui::Checkbox("Low-poly models", &state.settings.lowPolyModels)) {
        state.modelLibrary.setLowPoly(state.settings.lowPolyModels);
        refreshViewport(state, false);
        changed = true;
    }
    changed |= ImGui::Checkbox("Better quality", &state.settings.betterQuality);
    ImGui::SeparatorText("Overlays");
    changed |= ImGui::Checkbox("Axis", &state.settings.showAxis);
    changed |= ImGui::Checkbox("Areas", &state.settings.showAreas);
    changed |= ImGui::Checkbox("Cameras", &state.settings.showCameras);
    changed |= ImGui::Checkbox("Gravity", &state.settings.showGravity);
    changed |= ImGui::Checkbox("Paths", &state.settings.showPaths);
    ImGui::SeparatorText("Editor controls");
    changed |= ImGui::Checkbox("Invert camera motion", &state.settings.reverseRotation);
    changed |= ImGui::Checkbox("WASD movement", &state.settings.wasdMovement);
    ImGui::SeparatorText("Layout");
    if (ImGui::Checkbox("Allow floating panels", &state.settings.allowFloatingPanels)) {
        changed = true;
        pushToast(state, state.settings.allowFloatingPanels
                                 ? "Panels can now be torn off into their own windows."
                                 : "Panels now stay docked in the workspace.");
    }
    ImGui::SeparatorText("Workspace");
    ImGui::TextDisabled("Game directory:");
    ImGui::TextUnformatted(state.settings.lastGameDir.empty() ? "(none)"
                                                              : state.settings.lastGameDir.c_str());
    if (ImGui::Button("Choose...")) {
        if (auto picked = pickFolder(state.window); picked.has_value()) {
            try {
                openGame(state, *picked);
            } catch (const std::exception& error) {
                pushToast(state, error.what(), true);
                state.showLog = true;
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Close")) {
        state.showPreferences = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (changed) {
        state.settings.save();
    }
}

// --- Tutorials panel -----------------------------------------------------------
// A browsable, searchable guide to everything the editor can do. Each topic is
// a short list of steps, and steps that do something carry a button that does
// it right there, so the guide teaches by doing instead of describing.

enum class TutorialAction { None, OpenMap, OpenGame, FocusSearch, FrameAll, Save, ToggleLog };

struct TutorialStep {
    const char* text;
    TutorialAction action{TutorialAction::None};
    const char* button{nullptr};
};

struct TutorialTopic {
    const char* title;
    const char* keywords; // matched by the search box
    std::vector<TutorialStep> steps;
};

void runTutorialAction(EditorState& state, TutorialAction action) {
    switch (action) {
        case TutorialAction::OpenMap:
            requestOpenMap(state);
            break;
        case TutorialAction::OpenGame:
            requestOpenGame(state);
            break;
        case TutorialAction::FocusSearch:
            state.focusSearch = true;
            state.showObjects = true;
            break;
        case TutorialAction::FrameAll:
            if (state.viewportReady) {
                state.viewport.frameAll();
            }
            break;
        case TutorialAction::Save:
            requestSave(state);
            break;
        case TutorialAction::ToggleLog:
            state.showLog = true;
            break;
        case TutorialAction::None:
            break;
    }
}

// MARKER-TOPICS-INSERT
const std::vector<TutorialTopic>& tutorialTopics() {
    static const std::vector<TutorialTopic> topics = {
        {"Open your first map", "open map archive arc szs start begin load",
         {
             {"A map archive is one file holding a single zone's objects.",
              TutorialAction::OpenMap, "Open a map archive..."},
             {"No game dump needed: standalone archives work on their own.", TutorialAction::None},
             {"Prefer the full workspace? Open the extracted game folder instead.",
              TutorialAction::OpenGame, "Open a game directory..."},
         }},
        {"Browse galaxies and zones", "project galaxy zone browser navigate list smg1 smg2",
         {
             {"The Project panel shows the workspace: pick a galaxy, then a zone.", TutorialAction::None},
             {"Friendly names come from galaxies.json / zones.json; the raw name is in brackets.", TutorialAction::None},
             {"Loading a zone lists every placed object in the Objects panel.", TutorialAction::None},
         }},
        {"Find and select objects", "objects list search filter select find ctrl+f",
         {
             {"Type in the search box (toolbar or Objects panel) — the list filters live.",
              TutorialAction::FocusSearch, "Focus the search"},
             {"Click a row to select it; its transform loads into Properties.", TutorialAction::None},
             {"Double-click a row to fly the 3D camera to that object.", TutorialAction::None},
             {"Right-click a row for focus-in-viewport and reset shortcuts.", TutorialAction::None},
         }},
        {"Move, rotate and scale", "transform position rotation scale properties drag",
         {
             {"Drag the X/Y/Z fields in Properties — the 3D model follows live.", TutorialAction::None},
             {"Double-click a field to type an exact value (3-decimal precision).", TutorialAction::None},
             {"Each drag is one undo step — Ctrl+Z walks back gesture by gesture.", TutorialAction::None},
             {"Reset restores the file values; Save Zone writes everything to disk.",
              TutorialAction::Save, "Save the zone"},
         }},
        {"Undo and redo", "undo redo ctrl+z ctrl+y history revert",
         {
             {"Every transform edit is recorded when you release the mouse.", TutorialAction::None},
             {"Ctrl+Z undoes one gesture; Ctrl+Y (or Ctrl+R) redoes it.", TutorialAction::None},
             {"Edit > Undo / Redo name the step they would run.", TutorialAction::None},
             {"Loading another zone or map starts a fresh history.", TutorialAction::None},
         }},
        {"Use the 3D viewport", "viewport 3d camera orbit pan zoom frame select opengl",
         {
             {"Left-drag pans, right-drag orbits, wheel zooms; click selects.", TutorialAction::None},
             {"Frame All fits the zone; F (or double-click the list) frames the selection.",
              TutorialAction::FrameAll, "Frame the whole zone"},
             {"Labels toggles the floating object names in the 3D view.", TutorialAction::None},
             {"Real game models appear with a game directory open; archives alone show placeholders.", TutorialAction::None},
         }},
        {"Save your work", "save write disk ctrl+s unsaved",
         {
             {"Ctrl+S or File > Save Zone writes the zone back to its archive.",
              TutorialAction::Save, "Save the zone"},
             {"The asterisk in the status bar means unsaved edits.", TutorialAction::None},
             {"Edits go through the BCSV rows, so saves reopen exactly.", TutorialAction::None},
         }},
        {"Real models vs placeholders", "model bmd low poly placeholder objectdata workspace",
         {
             {"Open a game directory and the viewport resolves real BMD models from ObjectData.",
              TutorialAction::OpenGame, "Open a game directory..."},
             {"The status bar counts them as models N / M ph (real / placeholder).", TutorialAction::None},
             {"Low-Poly Models (View menu) prefers the Low/Middle variants.", TutorialAction::None},
         }},
        {"Customise the workspace", "layout theme dark light panels toolbar status preferences reset",
         {
             {"Drag tabs to rearrange; panels stay docked unless floating is allowed.", TutorialAction::None},
             {"View > Reset Layout restores the default arrangement.", TutorialAction::None},
             {"Your layout is saved automatically and restored on launch.", TutorialAction::None},
             {"Settings > Dark Theme switches the whole palette instantly.", TutorialAction::None},
         }},
        {"Keyboard shortcuts", "shortcut keys keyboard help",
         {
             {"Ctrl+O opens a map, Ctrl+S saves, Ctrl+F finds, Ctrl+Z / Ctrl+Y undo and redo.", TutorialAction::None},
             {"F frames the selected object in the 3D view.", TutorialAction::None},
             {"The full list lives under Help > Keyboard Shortcuts...", TutorialAction::None},
         }}}; // MARKER-MORE-TOPICS
    return topics;
}

void drawTutorialsPanel(EditorState& state) {
    if (!state.showTutorials) {
        return;
    }
    if (!ImGui::Begin("Tutorials", &state.showTutorials)) {
        ImGui::End();
        return;
    }
    // Left: searchable topic list. Right: the selected topic's steps.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##tutorial-search", "Search tutorials...",
                             state.tutorialSearch, sizeof(state.tutorialSearch));
    const std::string needle = whitehole::util::toLower(state.tutorialSearch);
    ImGui::Separator();
    const auto& topics = tutorialTopics();
    if (ImGui::BeginChild("##tutorial-list", ImVec2(200.0F, 0.0F), true)) {
        for (std::size_t i = 0; i < topics.size(); ++i) {
            if (!needle.empty()) {
                const std::string hay = whitehole::util::toLower(
                    std::string(topics[i].title) + " " + topics[i].keywords);
                if (hay.find(needle) == std::string::npos) {
                    continue;
                }
            }
            if (ImGui::Selectable(topics[i].title,
                                  state.tutorialTopic == static_cast<int>(i))) {
                state.tutorialTopic = static_cast<int>(i);
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##tutorial-body", ImVec2(0.0F, 0.0F), false)) {
        if (state.tutorialTopic < 0 ||
            static_cast<std::size_t>(state.tutorialTopic) >= topics.size()) {
            ImGui::TextDisabled("Pick a topic to learn the editor step by step.");
            ImGui::TextDisabled("The buttons inside a topic do the step for you.");
        } else {
            const auto& topic = topics[static_cast<std::size_t>(state.tutorialTopic)];
            ImGui::SeparatorText(topic.title);
            for (std::size_t s = 0; s < topic.steps.size(); ++s) {
                const auto& step = topic.steps[s];
                ImGui::PushID(static_cast<int>(s));
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", step.text);
                if (step.action != TutorialAction::None && step.button != nullptr) {
                    ImGui::Indent(24.0F);
                    if (ImGui::Button(step.button)) {
                        runTutorialAction(state, step.action);
                    }
                    ImGui::Unindent(24.0F);
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void drawShortcutsDialog(EditorState& state) {
    if (!state.showShortcuts) {
        return;
    }
    ImGui::OpenPopup("Keyboard Shortcuts");
    if (!ImGui::BeginPopupModal("Keyboard Shortcuts", &state.showShortcuts,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    struct Row {
        const char* keys;
        const char* action;
    };
    constexpr Row rows[] = {
        {"Ctrl+O", "Open map archive"},
        {"Ctrl+S", "Save current zone"},
        {"Ctrl+F", "Focus the object search"},
        {"F", "Frame the selected object"},
        {"Double-click", "Frame object in the 3D viewport"},
    };
    if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", row.keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.action);
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::Button("Close")) {
        state.showShortcuts = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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

int runGui(const std::filesystem::path& executable, const std::filesystem::path& initialFile) {
    using namespace whitehole::app;

    // Opt into per-monitor DPI awareness before any window exists, so Windows
    // stops bitmap-stretching the app (the main cause of blurry text) and the
    // backend reports truthful monitor scales to ImGui.
    ImGui_ImplWin32_EnableDpiAwareness();

    Settings settings;
    settings.load();
    // NOTE: applyWhiteholeTheme must NOT be called here — it needs an ImGui
    // context (created below). Calling ImGui::GetStyle() with no context is a
    // null-pointer crash that silently kills the app on boot (WIN32 subsystem
    // shows no console). Theme and fonts are applied after CreateContext.

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
    // With per-monitor DPI awareness these are physical pixels, so a fixed
    // 1280x720 would look cramped on a 150% display. Scale the first window for
    // the primary monitor; after that the user's own size is remembered by
    // Windows, and ImGui scales its own geometry per frame.
    int windowWidth = 1280;
    int windowHeight = 720;
    if (HDC screen = GetDC(nullptr)) {
        const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (dpi > 0) {
            const float bootScale = static_cast<float>(dpi) / 96.0F;
            windowWidth = static_cast<int>(1280.0F * bootScale);
            windowHeight = static_cast<int>(720.0F * bootScale);
        }
    }

    HWND hwnd = CreateWindowExW(0, L"WhiteholePro", L"Whitehole Pro",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                windowWidth, windowHeight,
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
    // Let ImGui re-bake glyphs at the current monitor's scale, so text stays
    // crisp and correctly sized when the window moves between displays.
    io.ConfigDpiScaleFonts = true;
    // NOTE: multi-viewport (tearing windows off into OS windows) is OFF on
    // purpose — it is what made panels float around as separate windows.

    // The dock layout persists across launches so a carefully arranged workspace
    // survives a restart (ImGui keeps this pointer, so it lives in a static).
    // When no layout exists yet — or after View > Reset Layout — the default
    // DockBuilder arrangement is used instead.
    static std::string layoutIniPath;
    {
        std::error_code ec;
        const auto configDir = Settings::defaultConfigPath().parent_path();
        std::filesystem::create_directories(configDir, ec);
        layoutIniPath = (configDir / "layout.ini").string();
    }
    io.IniFilename = layoutIniPath.c_str();

    // Resolve the real monitor scale from the window now that it exists, then
    // load fonts and apply the theme with it. Geometry (padding, rounding,
    // scrollbars) scales here; glyph sizes are handled by ImGui itself.
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (!(g_dpiScale > 0.5F) || g_dpiScale > 4.0F) {
        g_dpiScale = 1.0F; // implausible reading: fall back to 100%
    }

    // data/ ships beside the executable (the UI font plus the JSON databases).
    const auto dataRoot = dataDirectory(executable);
    loadEditorFonts(io, dataRoot);
    applyWhiteholeTheme(settings.darkMode, g_dpiScale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // --- Editor state ---
    EditorState state;
    state.settings = settings;
    state.window = hwnd;
    state.buildDefaultLayout = !std::filesystem::exists(layoutIniPath);
    state.dataRoot = dataRoot;
    if (!state.dataRoot.empty()) {
        state.galaxyNames.loadJson(state.dataRoot / "galaxies.json");
        state.zoneNames.loadJson(state.dataRoot / "zones.json");
        const auto cachePath =
            Settings::defaultConfigPath().parent_path() / "objectdb.cache";
        state.objectDb.load(state.dataRoot / "objectdb.json", cachePath);
    }

    // --- Hosted 3D viewport (creates the OpenGL child window) ---
    initViewport(state, instance);

    // First launch: open the Tutorials beside the log so new users learn the
    // editor instead of staring at an empty workspace. A marker file remembers
    // the choice, matching the first-boot splash behaviour.
    {
        std::error_code ec;
        const auto seenFile =
            firstBootConfigDir() / "tutorials_seen.marker";
        state.tutorialsSeen = std::filesystem::exists(seenFile, ec);
        if (!state.tutorialsSeen) {
            state.showTutorials = true;
            state.tutorialTopic = 0;
            std::filesystem::create_directories(firstBootConfigDir(), ec);
            std::ofstream(seenFile, std::ios::trunc);
        }
    }

    // --- Load initial file if provided ---
    if (!initialFile.empty()) {
        try {
            if (std::filesystem::is_directory(initialFile)) {
                openGame(state, initialFile);
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

        // --- Shell: menu, then a single host that stacks toolbar, dockspace
        // and status bar top-to-bottom, so nothing can overlap anything.
        drawMenuBar(state, done);
        if (done) break;

        // Fullscreen dock host. The default arrangement is built once via
        // DockBuilder (project left, properties right, viewport center, log
        // drawer bottom); afterwards the layout.ini keeps the user's setup.
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

        if (state.showToolbar) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0F, 3.0F));
            drawToolbar(state);
            ImGui::Separator();
            ImGui::PopStyleVar();
        }

        const float statusHeight = state.showStatusBar ? ImGui::GetFrameHeight() + 12.0F : 0.0F;
        ImVec2 dockAvail = ImGui::GetContentRegionAvail();
        dockAvail.y -= statusHeight;
        if (dockAvail.y < 64.0F) {
            dockAvail.y = 64.0F;
        }
        // Panels stay docked by default; ripping one off into a floating OS
        // window is opt-in via Preferences.
        const ImGuiID dockSpaceId = ImGui::GetID("WhiteholeDockSpace");
        ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
        if (!state.settings.allowFloatingPanels) {
            dockFlags |= ImGuiDockNodeFlags_NoUndocking;
        }
        ImGui::DockSpace(dockSpaceId, dockAvail, dockFlags);
        if (state.buildDefaultLayout) {
            state.buildDefaultLayout = false;
            ImGui::DockBuilderRemoveNode(dockSpaceId);
            ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockSpaceId, dockAvail);
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
            ImGui::DockBuilderDockWindow("Tutorials", dockBottom);
            ImGui::DockBuilderFinish(dockSpaceId);
        }

        if (state.showStatusBar) {
            drawStatusBar(state);
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
        drawTutorialsPanel(state);
        drawAboutDialog(state);
        drawPreferencesDialog(state);
        drawShortcutsDialog(state);
        drawToasts(state);
        // Position + show/hide the OpenGL child window last, once every popup for
        // this frame has been submitted.
        syncViewportChild(state);
        // Then draw it. The child has no timer and Windows only sends WM_PAINT
        // when it gets uncovered, so without driving the redraw from here a
        // static scene keeps whatever was drawn last -- and shows a stale buffer
        // after the popup logic above re-shows the window.
        state.viewport.renderIfDirty();

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
        if (ctrlDown && (GetAsyncKeyState('Z') & 1)) {
            performUndo(state);
        }
        if (ctrlDown && ((GetAsyncKeyState('Y') & 1) || (GetAsyncKeyState('R') & 1))) {
            performRedo(state);
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
    state.viewport.destroy(); // GL child window must go before its parent
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
