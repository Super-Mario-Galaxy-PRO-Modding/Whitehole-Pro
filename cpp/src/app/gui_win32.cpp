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
#include "whitehole/app/theme_palette.hpp"
#include "whitehole/app/settings.hpp"
#include "whitehole/app/object_db_update.hpp"
#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/db/name_table.hpp"
#include "whitehole/db/object_db.hpp"
#include "whitehole/edit/authoring.hpp"
#include "whitehole/edit/commands.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/edit/validation.hpp"
#include "whitehole/render/model_library.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/smg/path.hpp"
#include "whitehole/util/text.hpp"
#include "whitehole/render/viewport_win32.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/object_model.hpp"
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
#include <chrono>      // polling a non-blocking future

#include <future>      // object database download runs off the UI thread

#include <charconv>
#include <cstring>
#include <fstream>     // first-boot marker file
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// MSVC needs these to pull in the shell/COM imports. Every other toolchain gets
// the same libraries from the CMake interface target (whitehole_win32), so the
// block is guarded: GCC otherwise reports the unknown pragma four times.
#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")
#endif

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

void showBootError(const wchar_t* what) {
    MessageBoxW(nullptr, what, L"Whitehole Pro — startup failed",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void showBootHresult(const wchar_t* what, HRESULT hr) {
    wchar_t buffer[512];
    swprintf(buffer, 512, L"%s\n\nHRESULT: 0x%08lX", what, static_cast<unsigned long>(hr));
    showBootError(buffer);
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
    // First-run database bootstrap. `data/objectdb.json` ships out-of-band (it is
    // regenerated from the community database), so a fresh install starts with
    // no object names and no parameter grid at all. The download runs on a
    // worker thread; pumpObjectDatabase() picks the result up once per frame.
    std::future<std::string> objectDbDownload;
    bool objectDbDownloadPending{false};

    db::ModelSubstitutions modelSubstitutions;
    render::ModelLibrary modelLibrary;
    Settings settings;
    std::optional<smg::GameArchive> game;
    std::vector<std::string> galaxies;
    std::vector<std::string> zones;
    std::optional<smg::StageArchive> stage;
    // Rails of the open zone, loaded beside the stage so the viewport rebuild
    // and the (future) Paths list share one snapshot per refresh.
    std::vector<smg::RailPath> stagePaths;
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
    char fieldFilter[96]{};    // field-grid filter box (Properties panel)
    bool draggingField{false}; // an ObjectModel setter is mid-float-drag,
                               // mirroring draggingTransform for the transform rows
    // Editing a rail point's position or a control vector, mirrored off the
    // transform drag gesture but against one BCSV row instead of a whole object.
    std::size_t railEditTableIndex{0};
    std::size_t railEditRowIndex{0};
    int railEditVector{0};      // 0 = position, 1 = control1, 2 = control2
    float railEditCurrent[9]{}; // live values for the three axes of the vector
    float railEditStart[9]{};   // baseline snapshot when the drag began
    bool draggingRailEdit{false};
    char railEditField[3][16]{}; // "pnt0_x".."pnt2_z", computed when the edit starts
    static constexpr const char* railEditVectorNames[3] = {"pnt0", "pnt1", "pnt2"};
    std::vector<smg::BcsvValue> railEditStartBcsv; // row values when the drag began

    // Selection on a rail (path) or one of its points. Mutually exclusive with
    // selectedObject so object and rail editing don't fight for the same slot.
    // Sentinel for "the path itself, not one of its points". Declared before
    // RailSelection so its default member initializer can name it.
    static constexpr std::size_t kNoRailPointIndex = static_cast<std::size_t>(-1);

    struct RailSelection {
        std::size_t pathIndex{0};
        std::size_t pointIndex{kNoRailPointIndex};
        int pointPart{0}; // 0 = the point, 1 = control1, 2 = control2
    };
    std::optional<RailSelection> selectedRail;
    char railNameBuf[160]{}; // path name (committed on Enter)
    // Undo position at the last save (or load). Dirty is "the cursor has moved
    // away from here", which is what makes undoing back to the saved state
    // report clean again instead of nagging forever.
    std::size_t savedUndoCursor{0};
    bool unsaved{false};

    // --- Closing guard -------------------------------------------------------
    // Exit (and opening a different archive) would throw away unsaved edits, so
    // those actions park themselves behind a confirmation instead of going
    // straight ahead.
    enum class PendingAction { None, Exit, OpenMap, OpenGame };
    bool confirmUnsaved{false};
    PendingAction pendingAction{PendingAction::None};


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
    bool showProblems{false}; // validation findings, docked beside the log
    // Validation is not free, so it runs only when the undo cursor moves.
    std::vector<edit::Finding> findings;
    std::size_t findingsCursor{static_cast<std::size_t>(-1)};
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

    // --- authoring: the Add Object picker ---------------------------------
    // Java opened a modal tree of every ObjectDB category before it would let
    // you place anything. Here the same database is searched live in one box,
    // and the placement list is picked from the ones this zone actually has.
    bool showAddObject{false};
    char addSearch[96]{};
    std::string addFilter;                  // last needle the matches were built for
    std::vector<const db::ObjectInfo*> addMatches;
    std::string addChosen;                  // internal name of the object to place
    int addTargetIndex{0};                  // row in the destination-list combo
    // The match list is rebuilt only when the needle changes (or the picker is
    // reopened), so a 10k-object database is filtered once per keystroke rather
    // than once per frame.
    bool addMatchesStale{true};
    // Set after create/duplicate/delete so the Objects panel scrolls the new row
    // into view on the next frame (it cannot scroll a row it has not drawn yet).
    bool scrollToSelected{false};

    // --- transform clipboard (Java Edit > Copy / Paste) -------------------
    // Tracked per group so pasting a position never disturbs a rotation or scale
    // the author set deliberately.
    bool hasCopiedPosition{false};
    bool hasCopiedRotation{false};
    bool hasCopiedScale{false};
    math::Vec3f copiedPosition{};
    math::Vec3f copiedRotation{};
    math::Vec3f copiedScale{1.0F, 1.0F, 1.0F};
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

// --- object database bootstrap ----------------------------------------------

// Kicks off (or re-kicks off) the community database download on a worker
// thread. WinHTTP is blocking, so it must never run on the UI thread; the
// future is polled once per frame by pumpObjectDatabase().
void startObjectDatabaseDownload(EditorState& state) {
    if (state.objectDbDownloadPending) {
        return;
    }
    if (!objectDatabaseDownloadAvailable()) {
        pushToast(state, "This build cannot download the object database; run "
                         "\"whitehole-pro-console objectdb update\" instead.", true);
        return;
    }
    state.objectDbDownloadPending = true;
    state.objectDbDownload = std::async(std::launch::async, [dataRoot = state.dataRoot] {
        return downloadObjectDatabase(dataRoot / "objectdb.json");
    });
}

// Collects a finished download and reloads the database on the UI thread.
// Called once per frame: an empty result means success, anything else is a
// human-readable reason. Nothing here is fatal -- the editor stays usable with
// raw object names and no parameter grid when the network is unavailable.
void pumpObjectDatabase(EditorState& state) {
    if (!state.objectDbDownloadPending || !state.objectDbDownload.valid()) {
        return;
    }
    if (state.objectDbDownload.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    state.objectDbDownloadPending = false;
    const std::string failure = state.objectDbDownload.get();
    if (!failure.empty()) {
        pushToast(state, "Object database download failed: " + failure, true);
        state.showLog = true;
        return;
    }
    std::error_code ec;
    const auto database = state.dataRoot / "objectdb.json";
    const auto cachePath = Settings::defaultConfigPath().parent_path() / "objectdb.cache";
    state.objectDb.load(database, cachePath);
    refreshObjects(state);
    // The Add Object picker keeps pointers into the database; a reload swaps the
    // whole map, so every match and the current choice have to go with it.
    state.addMatches.clear();
    state.addChosen.clear();
    state.addMatchesStale = true;
    if (state.objectDb.classCount() == 0 && state.objectDb.names().empty()) {
        pushToast(state, "The downloaded object database could not be parsed.", true);
        return;
    }
    pushToast(state, "Object database installed: " +
                         std::to_string(state.objectDb.names().size()) + " objects, " +
                         std::to_string(state.objectDb.classCount()) + " classes.");
}


void refreshViewport(EditorState& state, bool frame) {
    if (!state.viewportReady) {
        return;
    }
    if (state.stage) {
        // Rails load with every rebuild: cheap next to the BCSV work and it
        // keeps overlay geometry in step with path edits automatically.
        state.stagePaths = smg::loadPaths(*state.stage);
        render::OverlayFlags overlays;
        overlays.axis = state.settings.showAxis;
        overlays.areas = state.settings.showAreas;
        overlays.cameras = state.settings.showCameras;
        overlays.gravity = state.settings.showGravity;
        overlays.paths = state.settings.showPaths;
        // Game models come from the open workspace's ObjectData archives; a
        // standalone map archive without one keeps the placeholder shapes.
        if (state.modelLibrary.bound()) {
            state.modelLibrary.resetCounters();
            state.viewportScene.rebuild(state.stage->objects(), &state.modelLibrary, &state.stagePaths,
                                        overlays);
        } else {
            state.viewportScene.rebuild(state.stage->objects(), nullptr, &state.stagePaths, overlays);
        }
    } else {
        state.stagePaths.clear();
        state.viewportScene.clear();
        state.selectedRail.reset(); // no zone left to select a rail in
        state.viewport.setRailHighlight(std::nullopt);
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

bool markDirty(EditorState& state);

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
    markDirty(state);
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
void requestOpenMap(EditorState& state);
void requestOpenGame(EditorState& state);
// Re-reads the selected rail's row into the property widgets (defined with the
// other rail helpers below).
void refreshRailBuffers(EditorState& state);

// True when the undo cursor has moved away from the last save point. That is
// what makes undoing back to the saved state report clean again.
bool markDirty(EditorState& state) {
    state.unsaved = state.undoStack.cursor() != state.savedUndoCursor;
    return state.unsaved;
}

// True when it is safe to drop the current document, i.e. nothing would be
// lost. Otherwise the confirmation dialog records what the user asked for so it
// can run once the choice is made.
bool safeToDiscard(EditorState& state, EditorState::PendingAction action) {
    if (!state.unsaved) {
        return true;
    }
    state.pendingAction = action;
    state.confirmUnsaved = true;
    return false;
}

// Runs the action a confirmation just approved.
void runPendingAction(EditorState& state, bool& done) {
    const EditorState::PendingAction action = state.pendingAction;
    state.pendingAction = EditorState::PendingAction::None;
    switch (action) {
    case EditorState::PendingAction::Exit:
        done = true;
        break;
    case EditorState::PendingAction::OpenMap:
        requestOpenMap(state);
        break;
    case EditorState::PendingAction::OpenGame:
        requestOpenGame(state);
        break;
    case EditorState::PendingAction::None:
        break;
    }
}

void performUndo(EditorState& state) {
    if (!state.undoStack.undo()) {
        return;
    }
    if (state.stage) {
        state.stage->rebuildObjects();
        refreshObjects(state);
        syncTransformBuffers(state);
        markDirty(state);
        refreshViewport(state, false);
        if (state.selectedRail) {
            refreshRailBuffers(state); // the undo may have moved the row being edited
        }
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
        markDirty(state);
        refreshViewport(state, false);
        if (state.selectedRail) {
            refreshRailBuffers(state); // the redo may have moved the row being edited
        }
        pushToast(state, "Redid " + state.undoStack.undoLabel() + ".");
    }
}

void syncViewportSelection(EditorState& state, std::optional<std::size_t> selected) {

// --- authoring ---
    state.viewportSelected = selected;
    if (state.viewportReady) {
        state.viewport.setSelected(selected);
    }
    // Always update the canonical selection: callers like the object list
    // wrap this function with syncingSelection=true, and the guard below
    // must only suppress the transform-sync + status side-effects, not the
    // assignment itself -- otherwise the Properties panel never sees the
    // row you clicked in the list.
    state.selectedObject = selected;
    // Object and rail selection are mutually exclusive: taking the object side
    // also drops any rail pick and the viewport highlight that marks it.
    state.selectedRail.reset();
    if (state.viewportReady) {
        state.viewport.setRailHighlight(std::nullopt);
    }
    if (state.syncingSelection) {
        return;
    }
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

// Rail point editing, mirroring applyTransform / commitDragAsUndo but for one
// BCSV row in a path's point table. Live writes during the drag keep the 3D
// model in step with the cursor; only the before/after snapshots land on the
// undo stack, so Ctrl+Z is one gesture per drag just like object transforms.

void syncRailBuffers(EditorState& state) {
    if (!state.selectedRail || !state.stage) {
        state.selectedRail.reset();
        state.viewport.setRailHighlight(std::nullopt);
        return;
    }
    const auto& sel = *state.selectedRail;
    if (sel.pathIndex >= state.stagePaths.size()) {
        state.selectedRail.reset();
        state.viewport.setRailHighlight(std::nullopt);
        return;
    }
    const auto& path = state.stagePaths[sel.pathIndex];
    if (sel.pointIndex != EditorState::kNoRailPointIndex
        && (path.points.empty() || sel.pointIndex >= path.points.size())) {
        state.selectedRail.reset();
        state.viewport.setRailHighlight(std::nullopt);
        return;
    }
    // Sync the path name into the edit buffer.
    std::snprintf(state.railNameBuf, sizeof(state.railNameBuf), "%s", path.name.c_str());
    if (sel.pointIndex != EditorState::kNoRailPointIndex) {
        if (path.pointTableIndex == smg::kNoPointTable
            || path.pointTableIndex >= state.stage->tables().size()) {
            // The points file never loaded: there is no row to edit, so drop the
            // pick instead of indexing tables()[SIZE_MAX].
            state.selectedRail.reset();
            state.viewport.setRailHighlight(std::nullopt);
            return;
        }
        state.railEditTableIndex = path.pointTableIndex;
        state.railEditRowIndex = path.points[sel.pointIndex].rowIndex;
        auto& table = state.stage->tables()[state.railEditTableIndex].table;
        auto& row = table.rows()[state.railEditRowIndex];
        // Baseline row values for the undo command's "before" snapshot.
        state.railEditStartBcsv = row.values;
        // Current point coordinates into the float edit buffers.
        const char* base = EditorState::railEditVectorNames[sel.pointPart];
        state.railEditCurrent[0] = table.getFloat(row, std::string(base) + "_x", 0.0F);
        state.railEditCurrent[1] = table.getFloat(row, std::string(base) + "_y", 0.0F);
        state.railEditCurrent[2] = table.getFloat(row, std::string(base) + "_z", 0.0F);
        // Cached field names for applyRailEdit.
        std::snprintf(state.railEditField[0], sizeof(state.railEditField[0]), "%s_x", base);
        std::snprintf(state.railEditField[1], sizeof(state.railEditField[1]), "%s_y", base);
        std::snprintf(state.railEditField[2], sizeof(state.railEditField[2]), "%s_z", base);
        state.railEditStart[0] = state.railEditCurrent[0];
        state.railEditStart[1] = state.railEditCurrent[1];
        state.railEditStart[2] = state.railEditCurrent[2];
    } else {
        state.railEditTableIndex = 0;
        state.railEditRowIndex = 0;
        state.railEditCurrent[0] = state.railEditCurrent[1] = state.railEditCurrent[2] = 0.0F;
    }
    state.draggingRailEdit = false;
}

void applyRailEdit(EditorState& state) {
    if (!state.stage || !state.draggingRailEdit ||
        state.railEditTableIndex >= state.stage->tables().size() ||
        state.railEditRowIndex >= state.stage->tables()[state.railEditTableIndex].table.rows().size()) {
        return;
    }
    auto& table = state.stage->tables()[state.railEditTableIndex].table;
    auto& row = table.rows()[state.railEditRowIndex];
    table.setFloat(row, state.railEditField[0], state.railEditCurrent[0]);
    table.setFloat(row, state.railEditField[1], state.railEditCurrent[1]);
    table.setFloat(row, state.railEditField[2], state.railEditCurrent[2]);
    state.stage->rebuildObjects();
    refreshViewport(state, false);
}

void commitRailEditAsUndo(EditorState& state, const char* label) {
    if (!state.draggingRailEdit || !state.stage ||
        state.railEditTableIndex >= state.stage->tables().size() ||
        state.railEditRowIndex >= state.stage->tables()[state.railEditTableIndex].table.rows().size()) {
        state.draggingRailEdit = false;
        return;
    }
    auto after = state.stage->tables()[state.railEditTableIndex].table.rows()[state.railEditRowIndex].values;
    if (state.railEditStartBcsv.empty() || state.railEditStartBcsv == after) {
        // A click that never moved the value must not push an empty "before"
        // row, which would corrupt the row if it were ever restored.
        state.draggingRailEdit = false;
        return;
    }
    auto command = std::make_unique<edit::RowEditCommand>(
        *state.stage, state.railEditTableIndex, state.railEditRowIndex,
        std::move(state.railEditStartBcsv), std::move(after), label);
    state.undoStack.push(std::move(command));
    state.draggingRailEdit = false;
}

void selectRail(EditorState& state, std::size_t pathIndex, std::size_t pointIndex, int pointPart) {
    if (!state.stage || pathIndex >= state.stagePaths.size()) {
        return; // stale pick from a zone that has since been closed or replaced
    }
    const auto previous = state.selectedRail;
    state.syncingSelection = true;
    state.selectedRail = EditorState::RailSelection{pathIndex, pointIndex, pointPart};
    syncRailBuffers(state);
    if (!state.selectedRail) {
        // syncRailBuffers rejected the pick (points file missing, index stale):
        // keep whatever was selected before and say why.
        state.selectedRail = previous;
        if (previous) {
            state.viewport.setRailHighlight(render::RailPointRef{
                previous->pathIndex, previous->pointIndex, previous->pointPart});
        }
        state.syncingSelection = false;
        pushToast(state, "That path's points file did not load, so its points cannot be edited.",
                  true);
        return;
    }
    state.selectedObject.reset();
    state.viewportSelected.reset();
    state.viewport.setSelected(std::nullopt);
    state.viewport.setRailHighlight(render::RailPointRef{pathIndex, pointIndex, pointPart});
    state.syncingSelection = false;
    const auto& path = state.stagePaths[pathIndex];
    std::string label = path.label();
    if (pointIndex != EditorState::kNoRailPointIndex) {
        label += " — Point " + std::to_string(pointIndex);
        if (pointPart == 1) {
            label += " (Control 1)";
        } else if (pointPart == 2) {
            label += " (Control 2)";
        }
    }
    setStatus(state, label);
}

void refreshRailBuffers(EditorState& state) {
    syncRailBuffers(state);
}

// Applies a new path name to the path's own CommonPathInfo row as one undo
// entry, mirroring how applyTransform records object edits.
void applyPathName(EditorState& state, std::size_t pathIndex, const char* name) {
    if (!state.stage || pathIndex >= state.stagePaths.size()) {
        return;
    }
    auto& path = state.stagePaths[pathIndex];
    if (name == nullptr || name[0] == '\0' || path.name == name) {
        return;
    }
    if (path.tableIndex >= state.stage->tables().size()) {
        pushToast(state, "That path has no readable row to rename.", true);
        return;
    }
    auto& table = state.stage->tables()[path.tableIndex].table;
    if (path.rowIndex >= table.rows().size() ||
        table.rawValue(table.rows()[path.rowIndex], "name") == nullptr) {
        pushToast(state, "That path has no name field to rename.", true);
        return;
    }
    auto before = table.rows()[path.rowIndex].values;
    table.setString(table.rows()[path.rowIndex], "name", name);
    auto after = table.rows()[path.rowIndex].values;
    const std::string newName(name);
    path.name = newName;
    state.undoStack.push(std::make_unique<edit::RowEditCommand>(
        *state.stage, path.tableIndex, path.rowIndex, std::move(before), std::move(after),
        "Rename path"));
    markDirty(state);
    // Relabels the list and viewport; reloads stagePaths, so no reference into
    // it may be held across this call.
    refreshViewport(state, false);
    pushToast(state, "Renamed path to " + newName + ".");
}

// Color for rail rendering / list chips: deterministic from the path, so a path
// keeps the same color across sessions (the Java viewer pre-multiplied once per
// draw frame and so never settled on a stable shade).
ImU32 paletteForPath(const smg::Path& path) {
    // Hash the path id into 0..2^24, then bin it into the editor palette so rails
    // are readable even when many share a stage.
    const std::size_t bucket = (static_cast<std::size_t>(path.lId) * 2654435761ULL) % 12;
    static const ImU32 palette[12] = {
        IM_COL32(0xE0, 0x6C, 0x00, 0xFF), IM_COL32(0x00, 0x9E, 0xDB, 0xFF),
        IM_COL32(0x7B, 0xC4, 0x00, 0xFF), IM_COL32(0x9B, 0x5D, 0xE5, 0xFF),
        IM_COL32(0xDC, 0x35, 0x45, 0xFF), IM_COL32(0x00, 0xBF, 0x7F, 0xFF),
        IM_COL32(0xFF, 0xB3, 0x00, 0xFF), IM_COL32(0x4A, 0x90, 0xD9, 0xFF),
        IM_COL32(0xC0, 0x39, 0x2B, 0xFF), IM_COL32(0x27, 0xAE, 0x60, 0xFF),
        IM_COL32(0x8E, 0x44, 0xAD, 0xFF), IM_COL32(0x17, 0xA5, 0x89, 0xFF),
    };
    return palette[bucket % 12];
}

// --- authoring ---------------------------------------------------------------
// Java's add/duplicate/delete live in GalaxyEditorForm behind a modal object
// picker. The work itself now sits in edit/authoring (one undo entry per action,
// shared with the CLI and the tests); what is left here is deciding *where* a new
// object goes and keeping the list, the property panel and the 3D scene in step.

// Spawn point for a new object: at the selected object when there is one, else at
// whatever the 3D camera is looking at, so the object lands in view instead of at
// the world origin where nobody can find it.
math::Vec3f spawnPosition(EditorState& state) {
    if (state.stage && state.selectedObject &&
        *state.selectedObject < state.stage->objects().size()) {
        return state.stage->objects()[*state.selectedObject].position;
    }
    if (state.viewportReady) {
        return state.viewport.camera().target;
    }
    return {};
}

// Re-derives everything that depends on the stage tables after an authoring edit:
// the object list, the dirty marker and the 3D scene. The Problems panel re-runs
// by itself once the undo cursor moves.
void afterAuthoringEdit(EditorState& state) {
    refreshObjects(state);
    markDirty(state);
    refreshViewport(state, false);
}

// Places `info` in the table at `tableIndex` and selects it. Returns false when
// the row could not be built, with the reason already reported to the user.
bool createObjectFromDatabase(EditorState& state, const db::ObjectInfo& info,
                              std::size_t tableIndex) {
    if (!state.stage || tableIndex >= state.stage->tables().size()) {
        pushToast(state, "Open a zone before adding objects.", true);
        return false;
    }
    const std::string kind = state.stage->tables()[tableIndex].kind;
    const std::string layer = state.stage->tables()[tableIndex].layer;

    edit::NewObject request;
    request.name = info.internalName;
    request.position = spawnPosition(state);
    try {
        const auto created =
            edit::createObject(*state.stage, state.undoStack, tableIndex, request);
        if (!created.has_value()) {
            pushToast(state, "That list cannot hold a new object.", true);
            return false;
        }
        selectObject(state, created->objectIndex);
        state.scrollToSelected = true;
        afterAuthoringEdit(state);
    } catch (const std::exception& error) {
        pushToast(state, error.what(), true);
        state.showLog = true;
        return false;
    }
    pushToast(state, "Added " + info.internalName + " to " + kind + "/" + layer + ".");
    return true;
}

// Duplicates the selected object next to itself and selects the copy.
bool duplicateSelection(EditorState& state) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        pushToast(state, "Select an object to duplicate first.", true);
        return false;
    }
    // Finish an in-flight drag so the copy inherits exactly what is on screen.
    if (state.draggingTransform) {
        commitDragAsUndo(state, "Edit object");
    }
    const std::string name = state.stage->objects()[*state.selectedObject].name;
    try {
        const auto created =
            edit::duplicateObject(*state.stage, state.undoStack, *state.selectedObject);
        if (!created.has_value()) {
            pushToast(state, "That object is no longer in this zone.", true);
            return false;
        }
        selectObject(state, created->objectIndex);
        state.scrollToSelected = true;
        afterAuthoringEdit(state);
    } catch (const std::exception& error) {
        pushToast(state, error.what(), true);
        return false;
    }
    // The copy keeps the same transform, so say so instead of leaving the author
    // hunting for an object hidden exactly behind the original.
    pushToast(state, "Duplicated " + name + ". Move it with the drag fields, or Ctrl+Z to undo.");
    return true;
}

// Deletes the selected object as one undoable step.
bool deleteSelection(EditorState& state) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        pushToast(state, "Select an object to delete first.", true);
        return false;
    }
    const std::string name = state.stage->objects()[*state.selectedObject].name;
    const auto removed = edit::deleteObjects(*state.stage, state.undoStack,
                                             {*state.selectedObject}, "Delete " + name);
    if (removed == 0) {
        pushToast(state, "That object is no longer in this zone.", true);
        return false;
    }
    selectObject(state, std::nullopt);
    afterAuthoringEdit(state);
    pushToast(state, "Deleted " + name + ". Ctrl+Z brings it back.");
    return true;
}

// Java Edit > Copy Position / Rotation / Scale, all in one menu. The groups are
// remembered separately so pasting a position never disturbs a deliberately set
// rotation or scale.
void copyTransform(EditorState& state, bool position, bool rotation, bool scale) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        return;
    }
    const auto& object = state.stage->objects()[*state.selectedObject];
    if (position) {
        state.copiedPosition = object.position;
        state.hasCopiedPosition = true;
    }
    if (rotation) {
        state.copiedRotation = object.rotation;
        state.hasCopiedRotation = true;
    }
    if (scale) {
        state.copiedScale = object.scale;
        state.hasCopiedScale = true;
    }
}

// Pastes the copied groups into the selected object as a single undo step.
bool pasteTransform(EditorState& state, bool position, bool rotation, bool scale) {
    if (!state.stage || !state.selectedObject ||
        *state.selectedObject >= state.stage->objects().size()) {
        pushToast(state, "Select an object to paste onto first.", true);
        return false;
    }
    const bool anyPosition = position && state.hasCopiedPosition;
    const bool anyRotation = rotation && state.hasCopiedRotation;
    const bool anyScale = scale && state.hasCopiedScale;
    if (!anyPosition && !anyRotation && !anyScale) {
        pushToast(state, "Copy a transform first.", true);
        return false;
    }
    // A live drag is finished first so the paste becomes its own undo step.
    if (state.draggingTransform) {
        commitDragAsUndo(state, "Edit object");
    }
    // The current transform is the "before" state of the undo entry, exactly the
    // same contract a drag follows.
    state.dragStart = state.stage->objects()[*state.selectedObject];
    state.draggingTransform = true;
    if (anyPosition) {
        state.transform[0] = state.copiedPosition.x;
        state.transform[1] = state.copiedPosition.y;
        state.transform[2] = state.copiedPosition.z;
    }
    if (anyRotation) {
        state.transform[3] = state.copiedRotation.x;
        state.transform[4] = state.copiedRotation.y;
        state.transform[5] = state.copiedRotation.z;
    }
    if (anyScale) {
        state.transform[6] = state.copiedScale.x;
        state.transform[7] = state.copiedScale.y;
        state.transform[8] = state.copiedScale.z;
    }
    applyTransform(state); // writes the BCSV row and repaints the viewport
    commitDragAsUndo(state, "Paste transform");

    std::string what = anyPosition ? "position" : "";
    if (anyRotation) {
        what += what.empty() ? "rotation" : " + rotation";
    }
    if (anyScale) {
        what += what.empty() ? "scale" : " + scale";
    }
    pushToast(state, "Pasted " + what + ".");
    return true;
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
    state.savedUndoCursor = 0;
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
    state.savedUndoCursor = 0;
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
    state.savedUndoCursor = 0;
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
    state.savedUndoCursor = state.undoStack.cursor();
    state.unsaved = false;
    pushToast(state, "Saved " + state.stage->sourcePath().string());
}

// --- UI panels ---------------------------------------------------------------

// Category chip in the object list, matching the viewport's color language.
ImVec4 categoryColor(const smg::PlacementObject& object) {
    const auto& style = render::objectStyle(object.kind, object.name);
    return ImVec4(style.color[0], style.color[1], style.color[2], 1.0F);
}

// The palette is the source of truth for every colour the UI paints by hand.
// ImGui's text helpers want an ImVec4, so the conversion lives in exactly one
// place and no call site invents a colour of its own.
ImVec4 toImVec4(const Rgba& color) {
    return ImVec4(color.r, color.g, color.b, color.a);
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

    // Authoring row: the three things a level editor is actually for. They sit
    // above the list rather than only in a context menu, so a new user finds them
    // without having to guess that rows are right-clickable.
    const float halfWidth = (ImGui::GetContentRegionAvail().x - 6.0F) * 0.5F;
    ImGui::BeginDisabled(!state.stage.has_value());
    if (ImGui::Button("Add Object…##objects", ImVec2(halfWidth, 0))) {
        state.showAddObject = true;
        state.addMatchesStale = true;
    }
    ImGui::SetItemTooltip("Place a new object in this zone  (Shift+A)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.selectedObject.has_value());
    if (ImGui::Button("Duplicate##objects", ImVec2(halfWidth, 0))) {
        duplicateSelection(state);
    }
    ImGui::SetItemTooltip("Copy the selected object next to itself  (Ctrl+D)");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!state.stage.has_value() || !state.selectedObject.has_value());
    if (ImGui::Button("Delete##objects", ImVec2(halfWidth, 0))) {
        deleteSelection(state);
    }
    ImGui::SetItemTooltip("Remove the selected object — undoable with Ctrl+Z  (Del)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("every change is undoable");
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
    // After an add/duplicate the new row must be visible even when it sits far
    // outside the current scroll window, so it is exempted from clipping.
    if (state.scrollToSelected && state.selectedObject.has_value()) {
        for (std::size_t row = 0; row < state.visibleObjects.size(); ++row) {
            if (state.visibleObjects[row] == *state.selectedObject) {
                clipper.IncludeItemByIndex(static_cast<int>(row));
                break;
            }
        }
    }
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
            if (selected && state.scrollToSelected) {
                ImGui::SetScrollHereY(0.5F);
            }
            // Right-click: the same authoring actions plus the view helpers.
            if (ImGui::BeginPopupContextItem("##objctx")) {
                if (ImGui::MenuItem("Focus in viewport", "F", false, state.viewportReady)) {
                    selectObject(state, stageIndex);
                    state.viewport.frameSelection();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                    selectObject(state, stageIndex);
                    duplicateSelection(state);
                }
                if (ImGui::MenuItem("Delete", "Del")) {
                    selectObject(state, stageIndex);
                    deleteSelection(state);
                }
                ImGui::Separator();
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
    state.scrollToSelected = false; // consumed once, for this frame's list only

    // Rail path list: every path this zone has. The path label selects the path
    // itself (name + parameters); each point row selects that point for editing
    // in the Properties panel.
    if (!state.stagePaths.empty()) {
        if (ImGui::CollapsingHeader("Paths", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (std::size_t p = 0; p < state.stagePaths.size(); ++p) {
                const auto& path = state.stagePaths[p];
                ImGui::PushID(static_cast<int>(p));
                const bool pathPicked = state.selectedRail &&
                                        state.selectedRail->pathIndex == p &&
                                        state.selectedRail->pointIndex == EditorState::kNoRailPointIndex;
                const ImU32 railInk = paletteForPath(path);
                if (pathPicked) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36F, 0.66F, 1.0F, 1.0F));
                } else {
                    // Explicit casts: the shifts yield unsigned int, and the
                    // implicit unsigned-int -> float step is exactly what
                    // -Wconversion is there to catch.
                    const float red = static_cast<float>((railInk >> 24) & 0xFFu) / 255.0F;
                    const float green = static_cast<float>((railInk >> 16) & 0xFFu) / 255.0F;
                    const float blue = static_cast<float>((railInk >> 8) & 0xFFu) / 255.0F;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(red, green, blue, 1.0F));
                }
                ImGui::TextUnformatted(path.label().c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nClick to edit this path's name and parameters.",
                                      path.label().c_str());
                }
                if (ImGui::IsItemClicked()) {
                    selectRail(state, p, EditorState::kNoRailPointIndex, 0);
                }
                ImGui::SameLine();
                const std::string pathLabel = path.type + " / " + path.usage + " / " +
                                              std::to_string(path.points.size()) + " pts";
                ImGui::TextDisabled("%s", pathLabel.c_str());
                ImGui::Separator();
                if (path.points.empty()) {
                    ImGui::TextDisabled("  No points.");
                } else {
                    for (std::size_t q = 0; q < path.points.size(); ++q) {
                        const auto& pt = path.points[q];
                        ImGui::PushID(static_cast<int>(p) * 10000 + static_cast<int>(q));
                        ImGui::Indent(ImGui::GetStyle().IndentSpacing);
                        const bool pointPicked = state.selectedRail &&
                                                 state.selectedRail->pathIndex == p &&
                                                 state.selectedRail->pointIndex == q;
                        if (pointPicked) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36F, 0.66F, 1.0F, 1.0F));
                        }
                        ImGui::Text("Point %zu", q);
                        if (pointPicked) {
                            ImGui::PopStyleColor();
                        }
                        const bool clickedPoint = ImGui::IsItemClicked();
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "id %d\npos (%.0f, %.0f, %.0f)\nc1 (%.0f, %.0f, %.0f)  "
                                "c2 (%.0f, %.0f, %.0f)\nClick to edit this point.",
                                static_cast<int>(pt.id), static_cast<double>(pt.position.x),
                                static_cast<double>(pt.position.y), static_cast<double>(pt.position.z),
                                static_cast<double>(pt.control1.x), static_cast<double>(pt.control1.y),
                                static_cast<double>(pt.control1.z), static_cast<double>(pt.control2.x),
                                static_cast<double>(pt.control2.y), static_cast<double>(pt.control2.z));
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%.0f, %.0f, %.0f)", static_cast<double>(pt.position.x),
                                            static_cast<double>(pt.position.y),
                                            static_cast<double>(pt.position.z));
                        if (clickedPoint) {
                            selectRail(state, p, q, 0);
                        }
                        ImGui::Unindent(ImGui::GetStyle().IndentSpacing);
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::End();

}

// The Add Object picker: Java's ObjectSelectForm rebuilt around live search. One
// box filters every object the database knows for this game, a second chooses
// which of the zone's placement lists the new row goes into, and Enter places it.
// Nothing here knows what any object *does* — all of that comes from the
// database, so a newly documented community object appears without a code change.
void drawAddObjectDialog(EditorState& state) {
    if (!state.showAddObject) {
        return;
    }
    const int gameType = state.game ? state.game->gameType() : 2;
    ImGui::OpenPopup("Add Object");
    ImGui::SetNextWindowSize(ImVec2(560.0F * dpiScaleFactor(), 430.0F * dpiScaleFactor()),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Add Object", &state.showAddObject,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const auto closeDialog = [&] {
        state.showAddObject = false;
        ImGui::CloseCurrentPopup();
    };

    // A fresh install has no database at all (it is downloaded on first run), and
    // without it there is no object list to pick from. Say so instead of showing
    // an empty box with a dead Add button.
    if (state.objectDb.names().empty()) {
        ImGui::TextUnformatted("No object database is installed yet.");
        ImGui::TextWrapped("Object names, their parameters and this list all come from the "
                           "community database. Install it once and this picker fills up.");
        ImGui::Separator();
        ImGui::BeginDisabled(state.objectDbDownloadPending);
        if (ImGui::Button("Download now", ImVec2(140, 0))) {
            startObjectDatabaseDownload(state);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            closeDialog();
        }
        ImGui::EndPopup();
        return;
    }

    const bool canPlace = state.stage.has_value();
    if (ImGui::IsWindowAppearing()) {
        // Typing is what the user came here to do, so start in the search box.
        ImGui::SetKeyboardFocusHere();
        state.addMatchesStale = true;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submitted =
        ImGui::InputTextWithHint("##addsearch", "Search an object: name, class or internal name",
                                 state.addSearch, sizeof(state.addSearch),
                                 ImGuiInputTextFlags_EnterReturnsTrue);

    const std::string needle = whitehole::util::toLower(state.addSearch);
    if (state.addMatchesStale || needle != state.addFilter) {
        state.addFilter = needle;
        state.addMatchesStale = false;
        // An empty needle matches everything, which is the "show me what I can
        // place" default Java's category tree opened with.
        state.addMatches = state.objectDb.search(needle, gameType);
    }
    ImGui::TextDisabled("%d object%s available for SMG%d", static_cast<int>(state.addMatches.size()),
                        state.addMatches.size() == 1 ? "" : "s", gameType);

    ImGui::BeginChild("##addlist", ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 13.0F),
                      ImGuiChildFlags_Borders);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.addMatches.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& info = *state.addMatches[static_cast<std::size_t>(row)];
            std::string label = info.internalName;
            if (info.name != info.internalName) {
                label += "  (" + info.name + ")";
            }
            if (const auto* objectClass = state.objectDb.classForObject(info.internalName, gameType);
                objectClass != nullptr && !objectClass->name.empty()) {
                label += "  — " + objectClass->name;
            }
            ImGui::PushID(row);
            if (ImGui::Selectable(label.c_str(), state.addChosen == info.internalName)) {
                if (state.addChosen != info.internalName) {
                    state.addChosen = info.internalName;
                    state.addTargetIndex = 0; // the destination list may differ per object
                }
            }
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                // Description plus class: the class is what tells two similarly
                // named objects apart, and it costs nothing (already in memory).
                const auto* infoClass = state.objectDb.classForObject(info.internalName, gameType);
                const std::string className = infoClass != nullptr ? infoClass->name : std::string();
                ImGui::SetTooltip("%s\n%s\n%s", info.internalName.c_str(),
                                  info.description.empty() ? "(no description in the database)"
                                                           : info.description.c_str(),
                                  className.empty() ? "(class unknown to the database)"
                                                    : className.c_str());
            }
        }
    }
    ImGui::EndChild();

    // Enter with nothing clicked yet picks the top match, so the full flow is:
    // type a few letters, press Enter, and the object is placed.
    if (submitted && state.addChosen.empty() && !state.addMatches.empty()) {
        state.addChosen = state.addMatches.front()->internalName;
    }

    // Where the row goes: the database names the placement list ("MapPartsInfo"),
    // and edit/authoring maps that onto the tables this zone actually has. Java
    // needed one hand-written branch per object type to do the same thing.
    const db::ObjectInfo* chosen =
        state.addChosen.empty() ? nullptr : state.objectDb.find(state.addChosen);
    std::vector<edit::PlacementTarget> targets;
    if (chosen != nullptr && canPlace) {
        targets = edit::placementTargets(*state.stage, edit::kindForList(chosen->list(gameType)));
    }
    if (state.addTargetIndex < 0 ||
        static_cast<std::size_t>(state.addTargetIndex) >= targets.size()) {
        state.addTargetIndex = 0;
    }
    const bool canAdd = canPlace && chosen != nullptr && !targets.empty();

    ImGui::Separator();
    if (!canPlace) {
        ImGui::TextWrapped("Open a zone or map archive first — a new object needs one of the "
                           "zone's placement lists to live in.");
    } else if (chosen == nullptr) {
        ImGui::TextDisabled("Pick an object above, then choose where it goes.");
    } else if (targets.empty()) {
        const std::string_view list = chosen->list(gameType);
        ImGui::TextWrapped("This zone has no \"%s\" list, so \"%s\" cannot be placed here.",
                           list.empty() ? "(unknown)" : std::string(list).c_str(),
                           chosen->internalName.c_str());
    } else {
        const auto& target = targets[static_cast<std::size_t>(state.addTargetIndex)];
        const std::string preview =
            target.kind + "/" + target.layer + "  (" + std::to_string(target.rowCount) + " rows)";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55F);
        if (ImGui::BeginCombo("##addtarget", preview.c_str())) {
            for (std::size_t index = 0; index < targets.size(); ++index) {
                const std::string label = targets[index].kind + "/" + targets[index].layer + "  (" +
                                          std::to_string(targets[index].rowCount) + " rows)";
                if (ImGui::Selectable(label.c_str(),
                                      static_cast<std::size_t>(state.addTargetIndex) == index)) {
                    state.addTargetIndex = static_cast<int>(index);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const math::Vec3f spawn = spawnPosition(state);
        ImGui::TextDisabled("at %.0f, %.0f, %.0f", static_cast<double>(spawn.x),
                            static_cast<double>(spawn.y), static_cast<double>(spawn.z));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("New objects land on the selected object, or on whatever the 3D "
                              "camera is looking at when nothing is selected.");
        }
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!canAdd);
    if (ImGui::Button("Add", ImVec2(120, 0)) || (submitted && canAdd)) {
        const auto tableIndex = targets[static_cast<std::size_t>(state.addTargetIndex)].tableIndex;
        if (createObjectFromDatabase(state, *chosen, tableIndex)) {
            closeDialog();
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Place the object and select it  (Enter)");
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        closeDialog();
    }
    if (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        closeDialog();
    }
    ImGui::EndPopup();
}

// How the field grid below shows one database value list entry. The database
// ships entries as "Value: Notes"; matching on the "Value:" prefix finds the
// entry for the stored value, and the full entry is previewed so 0 and 255
// stay distinguishable without guessing.
const char* fieldListPreview(const smg::ObjectField& field, const std::string& current) {
    for (const auto& option : field.values) {
        if (option.rfind(current + ":", 0) == 0) {
            return option.c_str();
        }
    }
    return current.c_str();
}

// Insertions into std::string combo entries use "Value: Notes" prefixes; the
// leading integer is the stored value, everything after the colon is the note.
bool parseListPrefix(const std::string& option, std::int32_t& out) {
    std::string digits;
    for (char ch : option) {
        if ((ch >= '0' && ch <= '9') || (ch == '-' && digits.empty())) {
            digits.push_back(ch);
        } else {
            break;
        }
    }
    if (digits.empty() || (digits == "-")) {
        return false;
    }
    try {
        out = static_cast<std::int32_t>(std::stol(digits));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// How the grid below commits one row back through the undo stack, then pulls
// the panels up to date the same way performUndo() does.
void commitFieldWrite(EditorState& state, bool wrote) {
    if (!wrote) {
        return;
    }
    state.stage->rebuildObjects();
    refreshObjects(state);
    syncTransformBuffers(state);
    markDirty(state);
    refreshViewport(state, false);
}

// Combos and free-text entry for every non-boolean, non-float kind.
bool drawFieldEntry(EditorState& state, smg::ObjectModel& model, std::size_t objectIndex,
                    const smg::ObjectField& field, const char* label,
                    const std::string& undoLabel);
// Honest annotation for the row, plus the database description as a tooltip.
void drawFieldStatus(EditorState& state, const smg::ObjectField& field,
                     bool widgetHovered);

// The field grid under the transform rows: every object parameter the archive
// stores, enumerated by the model and edited by the right widget for its
// declared kind. This function knows nothing about what Obj_arg0 means.
void drawObjectFieldGrid(EditorState& state, std::size_t objectIndex) {
    if (!state.stage || objectIndex >= state.stage->objects().size()) {
        return;
    }
    const int gameType = state.game ? state.game->gameType() : 2;
    smg::ObjectModel model(*state.stage, state.objectDb, gameType);
    // Cheap: a handful of small strings, rebuilt per frame like everything else
    // in this panel. Held by value so a setter mutating the row cannot disturb
    // the list a widget is iterating.
    const std::vector<smg::ObjectField> fields = model.fields(objectIndex);
    if (fields.empty()) {
        return;
    }

    ImGui::SeparatorText("Fields");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0F);
    ImGui::InputTextWithHint("##fieldfilter", "Filter fields…", state.fieldFilter,
                             sizeof(state.fieldFilter));
    ImGui::SameLine();
    if (ImGui::Button("X##clearfields", ImVec2(24, 0))) {
        state.fieldFilter[0] = '\0';
    }
    const std::string needle = whitehole::util::toLower(state.fieldFilter);

    for (std::size_t row = 0; row < fields.size(); ++row) {
        const smg::ObjectField& field = fields[row];
        if (!needle.empty()) {
            const std::string hay =
                whitehole::util::toLower(field.label + " " + field.identifier);
            if (hay.find(needle) == std::string::npos) {
                continue;
            }
        }
        ImGui::PushID(static_cast<int>(row));
        const char* label =
            field.label.empty() ? field.identifier.c_str() : field.label.c_str();
        const std::string undoLabel = "Set " + field.label;
        bool wrote = false;
        if (!field.present) {
            ImGui::BeginDisabled();
        }
        if (field.kind == db::PropertyKind::Boolean) {
            bool checked = field.flag();
            if (ImGui::Checkbox(label, &checked)) {
                wrote = model.setBool(objectIndex, field.identifier, checked,
                                      state.undoStack, undoLabel);
            }
        } else if (field.kind == db::PropertyKind::Float) {
            double number = 0.0;
            // A float row normally carries a number; the flag covers a malformed
            // archive where the value came back as text.
            const bool numeric = field.decimal(number);
            float value = static_cast<float>(number);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat(numeric ? label : "##unparsable", &value, 0.1F, 0.0F, 0.0F,
                             "%.4f");
            if (ImGui::IsItemActive()) {
                state.draggingField = true;
            } else if (state.draggingField) {
                // One drag is one undoable step, exactly like the transform rows.
                state.draggingField = false;
                wrote = model.setFloat(objectIndex, field.identifier, value,
                                       state.undoStack, undoLabel);
            }
        } else {
            wrote = drawFieldEntry(state, model, objectIndex, field, label, undoLabel);
        }
        if (!field.present) {
            ImGui::EndDisabled();
        }
        drawFieldStatus(state, field, ImGui::IsItemHovered());
        commitFieldWrite(state, wrote);
        ImGui::PopID();
    }
}

// Combos and free-text entry for every non-boolean, non-float kind.
bool drawFieldEntry(EditorState& state, smg::ObjectModel& model, std::size_t objectIndex,
                    const smg::ObjectField& field, const char* label,
                    const std::string& undoLabel) {
    bool wrote = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (field.kind == db::PropertyKind::List || field.kind == db::PropertyKind::IntList ||
        field.kind == db::PropertyKind::SwitchId) {
        std::int32_t current = 0;
        if (!model.getInt(objectIndex, field.identifier, current)) {
            // Absent or non-numeric: no combo to offer, so show the row inert.
            ImGui::TextDisabled("%s", label);
            return false;
        }
        const std::string currentText = std::to_string(current);
        if (ImGui::BeginCombo(label, fieldListPreview(field, currentText))) {
            for (const auto& option : field.values) {
                std::int32_t optionValue = 0;
                if (!parseListPrefix(option, optionValue)) {
                    continue;
                }
                const bool picked = optionValue == current;
                if (ImGui::Selectable(option.c_str(), picked)) {
                    wrote = model.setInt(objectIndex, field.identifier, optionValue,
                                         state.undoStack, undoLabel);
                }
                if (picked) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return wrote;
    }

    if (field.kind == db::PropertyKind::TextList ||
        field.kind == db::PropertyKind::ObjectName ||
        field.kind == db::PropertyKind::Text) {
        std::string current;
        if (!model.getString(objectIndex, field.identifier, current)) {
            ImGui::TextDisabled("%s", label);
            return false;
        }
        if (!field.values.empty()) {
            if (ImGui::BeginCombo(label, fieldListPreview(field, current))) {
                for (const auto& option : field.values) {
                    const bool picked = option == current;
                    if (ImGui::Selectable(option.c_str(), picked)) {
                        wrote = model.setString(objectIndex, field.identifier, option,
                                                state.undoStack, undoLabel);
                    }
                    if (picked) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            return wrote;
        }
        char scratch[256]{};
        std::snprintf(scratch, sizeof(scratch), "%s", current.c_str());
        if (ImGui::InputTextWithHint("##textvalue", field.identifier.c_str(), scratch,
                                     sizeof(scratch), ImGuiInputTextFlags_EnterReturnsTrue)) {
            wrote = model.setString(objectIndex, field.identifier, std::string(scratch),
                                    state.undoStack, undoLabel);
        }
        return wrote;
    }

    // Integers and bitfields: an InputInt, hex for bitfields, committed when the
    // field loses focus so a half-typed value never escapes.
    std::int32_t current = 0;
    if (!model.getInt(objectIndex, field.identifier, current)) {
        ImGui::TextDisabled("%s", label);
        return false;
    }
    int shown = static_cast<int>(current);
    const ImGuiInputTextFlags flags = field.kind == db::PropertyKind::Bitfield
                                          ? ImGuiInputTextFlags_CharsHexadecimal
                                          : ImGuiInputTextFlags_None;
    ImGui::InputInt(label, &shown, 1, 100, flags);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        wrote = model.setInt(objectIndex, field.identifier,
                             static_cast<std::int32_t>(shown), state.undoStack, undoLabel);
    }
    return wrote;
}

// Honest annotation for the row, plus the database description as a tooltip.
// `widgetHovered` is the widget's own hover state, captured by the caller
// right after the field widget renders — before EndDisabled() can shift the
// "current item" to the annotation text below. The description tooltip is
// therefore tied to hovering the widget, while the "(not in file)"/"(unused)"
// annotation on its own text is checked inside this function.
void drawFieldStatus(EditorState& state, const smg::ObjectField& field,
                     bool widgetHovered) {
    (void)state;
    if (!field.present) {
        ImGui::SameLine();
        ImGui::TextDisabled("(not in file)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "The database knows this field, but this archive does not store "
                "it. Editing it would mean rewriting the table layout, so it is "
                "read-only here.");
        }
    } else if (!field.used) {
        ImGui::SameLine();
        ImGui::TextDisabled("(unused)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "This parameter does not apply to this object in this game.");
        }
    }
    if (!field.description.empty() && widgetHovered &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", field.description.c_str());
    }
}

void drawPropertiesPanel(EditorState& state) {
    if (!state.showProperties) {
        return;
    }
    if (!ImGui::Begin("Properties", &state.showProperties)) {
        ImGui::End();
        return;
    }
    if (!state.stage) {
        ImGui::TextDisabled("No zone loaded.");
        ImGui::TextWrapped("Open a map archive, or pick a galaxy and zone in the "
                           "Project panel, to see object properties here.");
        ImGui::End();
        return;
    }
    if (state.selectedRail) {
        if (state.selectedRail->pathIndex >= state.stagePaths.size()) {
            // The zone was closed or swapped out from under the pick.
            state.selectedRail.reset();
            state.viewport.setRailHighlight(std::nullopt);
            ImGui::TextDisabled("Nothing selected.");
            ImGui::End();
            return;
        }
        const auto& sel = *state.selectedRail;
        const auto& path = state.stagePaths[sel.pathIndex];
        // Captured as a value: the rename box below can rebuild stagePaths and
        // invalidate the `path` reference mid-frame.
        const std::size_t pointCount = path.points.size();
        ImGui::SeparatorText(path.label().c_str());
        ImGui::TextDisabled("%s \u00b7 %d points \u00b7 usage %s", path.type.c_str(),
                            static_cast<int>(pointCount), path.usage.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", path.label().c_str());
        }

        // Path name: applies to the path row, not to any point.
        ImGui::SeparatorText("Path");
        if (ImGui::InputTextWithHint("##pathname", "Path name (Enter to apply)",
                                     state.railNameBuf, sizeof(state.railNameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            applyPathName(state, sel.pathIndex, state.railNameBuf);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Renames the path. Press Enter to commit.");
        }

        // Point-level editing: position + the two control vectors, each as its own
        // drag group so they can be worked independently.
        if (sel.pointIndex != EditorState::kNoRailPointIndex && sel.pointIndex < pointCount) {
            // Re-read through the index: the rename box above may have rebuilt
            // stagePaths, so the earlier `path` reference cannot be used here.
            const auto& pt = state.stagePaths[sel.pathIndex].points[sel.pointIndex];
            ImGui::SeparatorText(
                ("Point " + std::to_string(sel.pointIndex) + " (id " + std::to_string(pt.id) + ")")
                    .c_str());

            // The three vectors share one drag-gesture lifecycle: a gesture on any
            // of them is one undo step, exactly like object transform rows.
            auto railVecRow = [&](const char* label, int vectorPart, const char* undoLabel) {
                float values[3];
                values[0] = state.railEditCurrent[vectorPart * 3 + 0];
                values[1] = state.railEditCurrent[vectorPart * 3 + 1];
                values[2] = state.railEditCurrent[vectorPart * 3 + 2];
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
                                                &values[axis], 0.5F, 0.0F, 0.0F, "%.3f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s %s\nDrag or double-click a field to type.", label,
                                           kAxes[axis]);
                    } else if (ImGui::IsItemActive()) {
                        ImGui::SetTooltip("%s %s = %.3f", label, kAxes[axis],
                                           static_cast<double>(values[axis]));
                    }
                    active |= ImGui::IsItemActive();
                }
                if (active && !state.draggingRailEdit) {
                    state.draggingRailEdit = true;
                    state.railEditVector = vectorPart;
                    std::memcpy(state.railEditStart, state.railEditCurrent, sizeof(state.railEditStart));
                    // Fresh "before" snapshot for this gesture: the previous
                    // commit moved the last one away, and every drag must be
                    // exactly one undo step.
                    if (state.stage && state.railEditTableIndex < state.stage->tables().size() &&
                        state.railEditRowIndex < state.stage->tables()[state.railEditTableIndex]
                                                       .table.rows()
                                                       .size()) {
                        state.railEditStartBcsv =
                            state.stage->tables()[state.railEditTableIndex]
                                .table.rows()[state.railEditRowIndex]
                                .values;
                    }
                }
                if (changed) {
                    // The widgets edited the locals; push them back so the BCSV
                    // write carries the NEW numbers, not this frame's old ones.
                    for (int axis = 0; axis < 3; ++axis) {
                        state.railEditCurrent[vectorPart * 3 + axis] = values[axis];
                    }
                    applyRailEdit(state);
                }
                ImGui::PopID();
                if (!active && state.draggingRailEdit && state.railEditVector == vectorPart) {
                    // Only commit when the vector that started the gesture releases.
                    commitRailEditAsUndo(state, undoLabel);
                }
                return active;
            };

            railVecRow("Position", 0, "Move point");
            railVecRow("Control 1", 1, "Move control 1");
            railVecRow("Control 2", 2, "Move control 2");

            // Commit any gesture still in flight once no rail widget is active.
            if (!ImGui::IsAnyItemActive() && state.draggingRailEdit) {
                commitRailEditAsUndo(state, "Edit rail point");
            }
        } else if (sel.pointIndex == EditorState::kNoRailPointIndex) {
            // Path-level fields when the path itself is selected (no point).
            // Fresh fetch: the rename box above may have rebuilt stagePaths.
            const auto& pathNow = state.stagePaths[sel.pathIndex];
            ImGui::SeparatorText("Path parameters");
            ImGui::TextDisabled("Type: %s  Usage: %s  Points: %zu", pathNow.type.c_str(),
                                pathNow.usage.c_str(), pathNow.points.size());
            ImGui::TextDisabled("Closed: %s", pathNow.closed ? "yes" : "no");
            ImGui::TextDisabled("Path_ID: %d", pathNow.pathId);
            ImGui::TextDisabled("num_pnt: %d", static_cast<int>(pathNow.points.size()));
        }

        ImGui::End();
        return;
    }

    if (!state.selectedObject || *state.selectedObject >= state.stage->objects().size()) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextWrapped("Click a row in the Objects panel, or an object in the 3D "
                           "viewport, and its properties appear here.");
        ImGui::End();
        return;
    }
    const auto& object = state.stage->objects()[*state.selectedObject];

    // No object database yet: raw names, no parameter grid, no hint why. The
    // Settings menu offers Update Object Database…; this panel advertises it
    // inline so a user seeing either side of the UI is pointed at the fix.
    if (state.objectDb.names().empty() && state.objectDb.classCount() == 0) {
        ImGui::BulletText("No object database");
        ImGui::TextWrapped("Use Settings > Update Object Database… to fetch it from the "
                           "community build. Without it you only see the raw object "
                           "names and none of the per-object parameter fields.");
        if (ImGui::Button("Update Object Database…")) {
            setStatus(state, "Downloading the object database…");
            startObjectDatabaseDownload(state);
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

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

    // Every other parameter the object carries, driven by the object database.
    drawObjectFieldGrid(state, *state.selectedObject);

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
        ImGui::TextColored(toImVec4(themePalette(state.settings.darkMode).unsaved),
                           "* Unsaved changes");
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
        // Rail picks arrive through their own callback, so anything reaching
        // here is object-world (or empty space): drop any rail selection too.
        state.selectedRail.reset();
        state.viewport.setRailHighlight(std::nullopt);
        syncTransformBuffers(state);
        state.syncingSelection = false;
        if (picked.has_value() && state.stage && *picked < state.stage->objects().size()) {
            const auto& object = state.stage->objects()[*picked];
            const auto& style = render::objectStyle(object.kind, object.name);
            setStatus(state, std::string(object.name) + " — " + style.label +
                                 " (" + object.kind + "/" + object.layer + ")");
        }
    });
    state.viewport.setOnSelectRail([&state](const render::RailPointRef& hit) {
        if (state.syncingSelection) {
            return;
        }
        selectRail(state, hit.pathIndex, hit.pointIndex, hit.part);
    });
    state.viewport.setShowLabels(state.showLabels);
    state.viewport.setOverlayTheme(state.settings.darkMode);
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
// behind it, so the child is temporarily hidden while one of those is up.
// Tooltips are excluded: they flicker open and closed as the cursor travels, and
// hiding the GL surface for each one both blanks the viewport and leaves its
// contents undefined when it comes back.
bool blockingPopupOverViewport() {
    ImGuiContext& context = *GImGui;
    for (const ImGuiPopupData& popup : context.OpenPopupStack) {
        const ImGuiWindow* window = popup.Window;
        if (window == nullptr) {
            continue;
        }
        // ImGui tooltips live in the popup stack, but they carry the Tooltip
        // window flag; everything else (menus, modals, combo popups) is a
        // surface that would be drawn behind the child.
        if ((window->Flags & ImGuiWindowFlags_Tooltip) == 0) {
            return true;
        }
    }
    return false;
}

void syncViewportChild(EditorState& state) {
    if (!state.viewportReady) {
        return;
    }
    const bool popupOpen = blockingPopupOverViewport();
    const bool wantVisible = state.viewportRectValid && !popupOpen;
    bool moved = false;
    if (wantVisible) {
        const RECT& target = state.viewportRect;
        const RECT& applied = state.viewportRectApplied;
        if (target.left != applied.left || target.top != applied.top ||
            target.right != applied.right || target.bottom != applied.bottom) {
            MoveWindow(state.viewport.handle(), target.left, target.top,
                       target.right - target.left, target.bottom - target.top, TRUE);
            state.viewportRectApplied = target;
            moved = true;
        }
    }
    if (wantVisible != state.viewportChildVisible) {
        ShowWindow(state.viewport.handle(), wantVisible ? SW_SHOWNA : SW_HIDE);
        state.viewportChildVisible = wantVisible;
        moved = moved || wantVisible;
    }
    if (moved && wantVisible) {
        // Showing or resizing a GL surface orphans whatever it held: a hidden
        // window comes back with undefined contents, and a resized one keeps
        // the old size until something repaints. Windows only delivers WM_PAINT
        // when a window is uncovered, so without this the panel can sit blank
        // until the user happens to click inside it.
        RedrawWindow(state.viewport.handle(), nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        state.viewport.invalidate();
    }
}

// --- Shell: menu bar, toolbar, dockspace, status bar --------------------------
// These turn the floating demo windows into one docked editor workspace with
// a real File/Edit/View/Settings/Help menu and a quick-action toolbar.

void requestOpenMap(EditorState& state) {
    // Guarding here covers every entry point (menu, toolbar, tutorials,
    // drag-and-drop) in one place. The confirmation dialog re-enters this
    // function once the user has chosen, and by then nothing is unsaved.
    if (!safeToDiscard(state, EditorState::PendingAction::OpenMap)) {
        return;
    }
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
    if (!safeToDiscard(state, EditorState::PendingAction::OpenGame)) {
        return;
    }
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
            if (safeToDiscard(state, EditorState::PendingAction::Exit)) {
                done = true;
            }
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
        const bool hasStage = state.stage.has_value();
        const bool hasSelection = state.selectedObject.has_value();
        if (ImGui::MenuItem("Add Object...", "Shift+A", false, hasStage)) {
            state.showAddObject = true;
            state.addMatchesStale = true;
        }
        if (ImGui::MenuItem("Duplicate Object", "Ctrl+D", false, hasStage && hasSelection)) {
            duplicateSelection(state);
        }
        if (ImGui::MenuItem("Delete Object", "Del", false, hasStage && hasSelection)) {
            deleteSelection(state);
        }
        ImGui::Separator();
        // Java's Edit > Copy / Paste, one group at a time so aligning objects does
        // not disturb the rotations and scales they were given on purpose.
        if (ImGui::BeginMenu("Copy", hasStage && hasSelection)) {
            if (ImGui::MenuItem("Position")) {
                copyTransform(state, true, false, false);
            }
            if (ImGui::MenuItem("Rotation")) {
                copyTransform(state, false, true, false);
            }
            if (ImGui::MenuItem("Scale")) {
                copyTransform(state, false, false, true);
            }
            if (ImGui::MenuItem("All", "Ctrl+C")) {
                copyTransform(state, true, true, true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Paste", hasStage && hasSelection)) {
            const bool anyCopied =
                state.hasCopiedPosition || state.hasCopiedRotation || state.hasCopiedScale;
            if (ImGui::MenuItem("Position", nullptr, false, state.hasCopiedPosition)) {
                pasteTransform(state, true, false, false);
            }
            if (ImGui::MenuItem("Rotation", nullptr, false, state.hasCopiedRotation)) {
                pasteTransform(state, false, true, false);
            }
            if (ImGui::MenuItem("Scale", nullptr, false, state.hasCopiedScale)) {
                pasteTransform(state, false, false, true);
            }
            if (ImGui::MenuItem("All", "Ctrl+V", false, anyCopied)) {
                pasteTransform(state, true, true, true);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Focus Search", "Ctrl+F", false, hasStage)) {
            state.focusSearch = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Object Transform", nullptr, false, hasSelection)) {
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
        ImGui::MenuItem("Problems", nullptr, &state.showProblems);
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
        // Every overlay here rebuilds the viewport immediately: a toggle that
        // only took effect on the next edit would feel broken.
        if (ImGui::MenuItem("Axis", nullptr, &state.settings.showAxis)) {
            state.settings.save();
            refreshViewport(state, false);
        }
        if (ImGui::MenuItem("Areas", nullptr, &state.settings.showAreas)) {
            state.settings.save();
            refreshViewport(state, false);
        }
        if (ImGui::MenuItem("Cameras", nullptr, &state.settings.showCameras)) {
            state.settings.save();
            refreshViewport(state, false);
        }
        if (ImGui::MenuItem("Gravity", nullptr, &state.settings.showGravity)) {
            state.settings.save();
            refreshViewport(state, false);
        }
        if (ImGui::MenuItem("Paths", nullptr, &state.settings.showPaths)) {
            state.settings.save();
            refreshViewport(state, false);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Dark Theme", nullptr, &state.settings.darkMode)) {
            applyWhiteholeTheme(state.settings.darkMode, dpiScaleFactor());
            state.viewport.setOverlayTheme(state.settings.darkMode);
            state.settings.save();
        }
        if (ImGui::MenuItem("Preferences...")) {
            state.showPreferences = true;
        }
        if (ImGui::MenuItem("Update Object Database…")) {
            if (state.objectDbDownloadPending) {
                setStatus(state, "Retrying the object database download…");
                state.objectDbDownload = {};
                startObjectDatabaseDownload(state);
            } else {
                setStatus(state, "Downloading the object database…");
                startObjectDatabaseDownload(state);
            }
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

// Confirmation for any action that would drop unsaved edits. Deliberately a
// plain centred window rather than a popup modal: it cannot be dismissed by
// accident, so there is no path where the editor silently carries on and loses
// the document.
void drawUnsavedDialog(EditorState& state, bool& done) {
    if (!state.confirmUnsaved) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin("Unsaved changes", nullptr, flags)) {
        ImGui::TextUnformatted("This zone has unsaved changes.");
        ImGui::TextDisabled("Continuing now would throw them away.");
        ImGui::Separator();
        if (ImGui::Button("Save and continue", ImVec2(150, 0))) {
            bool saved = true;
            try {
                saveStage(state);
            } catch (const std::exception& error) {
                // A failed save keeps the dialog up so nothing is lost.
                saved = false;
                pushToast(state, error.what(), true);
            }
            if (saved) {
                state.confirmUnsaved = false;
                runPendingAction(state, done);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard changes", ImVec2(130, 0))) {
            state.unsaved = false;
            state.confirmUnsaved = false;
            runPendingAction(state, done);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) {
            state.confirmUnsaved = false;
            state.pendingAction = EditorState::PendingAction::None;
        }
    }
    ImGui::End();
}

// Re-runs stage validation, but only when something could have changed it.
// Walking every object against the database on every frame would be waste.
void refreshProblems(EditorState& state) {
    if (!state.stage) {
        state.findings.clear();
        state.findingsCursor = 0;
        return;
    }
    if (state.findingsCursor == state.undoStack.cursor()) {
        return;
    }
    const int gameType = state.game ? state.game->gameType() : 2;
    const edit::ValidationReport report =
        edit::validateStage(*state.stage, state.objectDb, gameType);
    state.findings = report.findings;
    state.findingsCursor = state.undoStack.cursor();
}

// The validation drawer. Clicking a finding jumps to the object it is about,
// which is the whole point: a warning you cannot act on is just noise.
void drawProblemsPanel(EditorState& state) {
    refreshProblems(state);
    if (!state.showProblems) {
        return;
    }
    if (!ImGui::Begin("Problems", &state.showProblems)) {
        ImGui::End();
        return;
    }
    if (!state.stage) {
        ImGui::TextDisabled("No zone loaded, so there is nothing to check.");
        ImGui::End();
        return;
    }
    if (state.findings.empty()) {
        ImGui::TextUnformatted("No problems found.");
        ImGui::TextDisabled("Checks: unknown objects, game mismatches, missing "
                            "required parameters, dangling switches, zero scale.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%d finding(s)", static_cast<int>(state.findings.size()));
    ImGui::Separator();
    ImGui::BeginChild("##findings");
    for (std::size_t index = 0; index < state.findings.size(); ++index) {
        const edit::Finding& finding = state.findings[index];
        // Severity drives the chip colour; the palette keeps both readable.
        const Rgba& ink = finding.severity == edit::Severity::Error
                              ? themePalette(state.settings.darkMode).error
                              : themePalette(state.settings.darkMode).unsaved;
        const std::string severity(edit::toString(finding.severity));
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(ink));
        ImGui::TextUnformatted(severity.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string label = finding.message;
        if (!finding.hint.empty()) {
            label += "  —  " + finding.hint;
        }
        if (ImGui::Selectable(label.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick) &&
            finding.objectIndex.has_value()) {
            selectObject(state, *finding.objectIndex);
            state.showProperties = true;
        }
        if (ImGui::IsItemHovered() && finding.objectIndex.has_value()) {
            ImGui::SetTooltip("Click to select object #%zu in this zone.",
                              *finding.objectIndex);
        }
    }
    ImGui::EndChild();
    ImGui::End();
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
            // Fade with the toast, but keep the palette's ink so both themes
            // stay readable -- the old hard-coded red was 2.65:1 on white.
            const Palette& palette = themePalette(state.settings.darkMode);
            const Rgba& ink = toast.error ? palette.error : palette.accentFg;
            ImGui::TextColored(ImVec4(ink.r, ink.g, ink.b, alpha), "%s", toast.text.c_str());
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
    // Authoring, reachable without taking a hand off the 3D view.
    ImGui::BeginDisabled(!state.stage.has_value());
    if (ImGui::Button("Add")) {
        state.showAddObject = true;
        state.addMatchesStale = true;
    }
    ImGui::SetItemTooltip("Add an object to this zone  (Shift+A)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.selectedObject.has_value());
    if (ImGui::Button("Duplicate")) {
        duplicateSelection(state);
    }
    ImGui::SetItemTooltip("Duplicate the selected object  (Ctrl+D)");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        deleteSelection(state);
    }
    ImGui::SetItemTooltip("Delete the selected object  (Del)");
    ImGui::EndDisabled();
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
    // One height for the reserved space in the dock host and for the strip
    // itself; the two used to differ by two pixels, which read as a dark hairline
    // between the dock area and the status bar in light mode.
    const float height = ImGui::GetFrameHeight() + 12.0F;
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
        ImGui::TextColored(toImVec4(themePalette(state.settings.darkMode).unsaved), "*");
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
        state.viewport.setOverlayTheme(state.settings.darkMode);
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
        // Overlay/label/quality toggles change what the viewport draws, so the
        // scene has to be rebuilt for them to show up without another edit.
        refreshViewport(state, false);
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
        {"Build a level", "add create place duplicate copy delete object author level build shift+a",
         {
             {"Open a zone, then press Shift+A (or Objects > Add Object…) to place something new.",
              TutorialAction::None},
             {"Search by name, class or internal name; Enter places the highlighted object.",
              TutorialAction::None},
             {"The destination box picks the list it goes into, e.g. obj/Common or obj/LayerA.",
              TutorialAction::None},
             {"New objects land on the selected object (or the camera's focus) so you can see them.",
              TutorialAction::None},
             {"Ctrl+D duplicates, Del deletes, and every one of them is a single Ctrl+Z away from undone.",
              TutorialAction::None},
         }},
        {"Keyboard shortcuts", "shortcut keys keyboard help",
         {
             {"Ctrl+O opens a map, Ctrl+S saves, Ctrl+F finds, Ctrl+Z / Ctrl+Y undo and redo.", TutorialAction::None},
             {"Shift+A adds an object, Ctrl+D duplicates it, Del deletes it.", TutorialAction::None},
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
        {"Shift+A", "Add an object to this zone"},
        {"Ctrl+D", "Duplicate the selected object"},
        {"Del", "Delete the selected object (undoable)"},
        {"Ctrl+C / Ctrl+V", "Copy / paste the selected object's transform"},
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
        // A fresh install has no database at all (it is deliberately not
        // committed), which used to leave the editor with raw object names and
        // an empty parameter grid and no hint why. Fetch it once, quietly.
        if (state.objectDb.names().empty() &&
            !std::filesystem::exists(state.dataRoot / "objectdb.json")) {
            setStatus(state, "Downloading the object database…");
            startObjectDatabaseDownload(state);
        }
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
        // The host paints ImGuiCol_WindowBg. That matters in light mode: it used
        // to be NoBackground, so every pixel the shell did not explicitly draw
        // (the spacing around the toolbar separator, the strip above the status
        // bar) showed the hard-coded dark D3D clear colour as two black bars.
        // The central dock node stays transparent via PassthruCentralNode, which
        // is what the 3D child needs, so nothing here hides the viewport.
        const ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("##dockhost", nullptr, hostFlags);
        ImGui::PopStyleVar();

        if (state.showToolbar) {
            // One strip owns the toolbar row *and* the separator below it, so
            // the item spacing between them is painted by the strip rather than
            // left transparent for the clear colour to show through.
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0F, 3.0F));
            const float stripHeight = ImGui::GetFrameHeight() + 14.0F;
            ImGui::BeginChild("##toolbarstrip",
                              ImVec2(mainViewport->WorkSize.x, stripHeight),
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar);
            drawToolbar(state);
            ImGui::Separator();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
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
            ImGui::DockBuilderDockWindow("Problems", dockBottom);
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
        pumpObjectDatabase(state);
        drawShortcutsDialog(state);
        drawAddObjectDialog(state);
        drawToasts(state);
        drawProblemsPanel(state);
        drawUnsavedDialog(state, done);
        // Position + show/hide the OpenGL child window last, once every popup for
        // this frame has been submitted. It is drawn after the frame is
        // presented (see the bottom of the loop).
        syncViewportChild(state);

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
        // Authoring shortcuts. They stay out of the way while a text field has
        // focus, so typing "A" in an object name never opens the picker and Del
        // never deletes an object while the author is editing text.
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ctrlDown && (GetAsyncKeyState('D') & 1)) {
            duplicateSelection(state);
        }
        if (!typing && ctrlDown && (GetAsyncKeyState('C') & 1)) {
            copyTransform(state, true, true, true);
        }
        if (!typing && ctrlDown && (GetAsyncKeyState('V') & 1) &&
            (state.hasCopiedPosition || state.hasCopiedRotation || state.hasCopiedScale)) {
            pasteTransform(state, true, true, true);
        }
        if (!typing && (GetAsyncKeyState(VK_DELETE) & 1) && state.selectedObject) {
            deleteSelection(state);
        }
        if (!typing && !ctrlDown && (GetAsyncKeyState('A') & 1) &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            state.showAddObject = true;
            state.addMatchesStale = true;
        }

        // --- Rendering ---
        ImGui::Render();
        // The shell clears to the palette's window colour, not a hard-coded
        // grey. ImGui does not paint every pixel of the window (dock spacing,
        // the transparent central node behind the 3D child), so whatever the
        // clear leaves behind is visible between panels; a dark clear in light
        // mode was the source of the black bars under the toolbar.
        const Rgba shellColor = app::shellBackground(state.settings.darkMode);
        const float clearColor[4] = {shellColor.r, shellColor.g, shellColor.b, 1.0F};

        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapChain->Present(1, 0); // VSync

        // Draw the 3D child last. The parent's present rewrites the whole
        // window surface after the shell, so a viewport drawn before it is
        // composited stale or not at all -- the "blank until I click or move
        // inside it" bug. Drawing after the present leaves the GL surface as
        // the newest thing in the frame.
        state.viewport.renderIfVisible();
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



