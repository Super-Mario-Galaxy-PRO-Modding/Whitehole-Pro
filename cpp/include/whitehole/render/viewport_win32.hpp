#pragma once

// Win32 + WGL viewport window. Depends on Win32/OpenGL by design; the pure
// camera + scene math above stays portable and unit-tested.
//
// Lifetime: create() makes a child window of the editor, destroy() tears the
// GL context down. setScene() copies oriented boxes in (cheap pointer-free
// structs). Input: left-drag pan, right-drag orbit, middle-drag pan, wheel
// dolly (Shift = fast), click select, WASD/arrows fly (Shift fast, Ctrl slow,
// E/Q or PgUp/PgDn vertical), 1/2/3 gizmo mode, Space/Home frame.
//
// Objects with a real game model (ViewportBox::model) are drawn from GL
// display lists compiled once per mesh, so a whole zone of BMD models still
// redraws cheaply; objects without one keep the category placeholder shapes.

#ifdef _WIN32

#include "whitehole/render/camera.hpp"
#include "whitehole/render/gizmo.hpp"
#include "whitehole/render/model_mesh.hpp"
#include "whitehole/render/viewport_scene.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

namespace whitehole::render {

// GL model-texture cache: uploads decoded TEX1/BTI base mip levels as RGBA8
// textures and hands out GL names per (mesh identity, texture slot).
//
// Why the mesh pointer is the key: BTI tables live on ModelMesh now, and the
// mesh shared_ptr outlives any single frame, so keying on it needs no archive
// plumbing in the renderer. Entries whose mesh died are dropped lazily on the
// next lookup (meshes outlive display lists, so no live texture is ever
// orphaned by the prune).
//
// Pure Win32+GL by design; the pure-data mesh side stays unit-testable. All
// upload state is save/restored (pixel store, bound texture, texture-enable)
// so the composited frame looks identical with the cache hot or cold.
class ModelTextureCache {
public:
    ModelTextureCache() = default;
    ~ModelTextureCache();

    ModelTextureCache(const ModelTextureCache&) = delete;
    ModelTextureCache& operator=(const ModelTextureCache&) = delete;

    // GL name for `mesh.textures[textureIndex]`, uploading on first use.
    // Returns 0 when texturing is unavailable or the slot is out of range
    // (caller draws flat-colored then). The shared_ptr is what anchors the
    // cache entry: the mesh outlives any frame via ViewportBox::model.
    [[nodiscard]] unsigned int textureFor(const std::shared_ptr<const ModelMesh>& mesh,
                                          std::size_t textureIndex, const char* filter);

    // Forgets every uploaded texture (context loss, settings change).
    void clear() noexcept;

private:
    struct Entry {
        std::weak_ptr<const ModelMesh> mesh;
        std::vector<unsigned int> names; // parallel to mesh.textures, 0 = not uploaded
    };
    std::vector<Entry> entries_;
};

class ViewportWindow {
public:
    ViewportWindow();
    ~ViewportWindow();

    ViewportWindow(const ViewportWindow&) = delete;
    ViewportWindow& operator=(const ViewportWindow&) = delete;
    ViewportWindow(ViewportWindow&&) = delete;
    ViewportWindow& operator=(ViewportWindow&&) = delete;

    using SelectCallback = std::function<void(std::optional<std::size_t>)>;
    // Multi-select callback: additive picks (Ctrl/Shift) arrive with
    // `additive` set, so the editor can extend rather than replace the set.
    using SelectManyCallback = std::function<void(std::optional<std::size_t>, bool additive)>;
    // Marquee box select: every object whose projected centre fell in the rect.
    using SelectRectCallback = std::function<void(std::vector<std::size_t>, bool additive)>;
    // A rail point picked in the 3D view (point cube or control handle).
    using SelectRailCallback = std::function<void(const RailPointRef&)>;
    // One message from a gizmo drag (Begin/Update/End), already in world units.
    using GizmoCallback = std::function<void(const GizmoEdit&)>;

    bool create(HWND parent, int controlId, HINSTANCE instance);
    void destroy() noexcept;
    [[nodiscard]] bool valid() const noexcept { return window_ != nullptr && glContext_ != nullptr; }
    [[nodiscard]] HWND handle() const noexcept { return window_; }

    void setScene(ViewportScene scene);
    void setSelected(std::optional<std::size_t> selected);
    // The full multi-selection. The gizmo anchors on the first entry.
    void setSelection(std::vector<std::size_t> selected);
    void setHover(std::optional<std::size_t> hover);
    void setShowLabels(bool showLabels) noexcept;
    // Which transform handles the gizmo shows: move, rotate or scale (1/2/3).
    // The old W/E/R scheme collided with the WASD fly keys, so the mode keys
    // moved to the number row.
    void setGizmoMode(GizmoMode mode) noexcept;
    // Hides the gizmo when no handle should be offered (no selection, or a
    // text field has focus).
    void setGizmoEnabled(bool enabled) noexcept;
    // Legend/label ink follows the theme instead of the hard-coded dark box the
    // overlay used to paint, which looked wrong inside a light-mode workspace.
    void setOverlayTheme(bool dark) noexcept;
    // Settings > "Invert camera motion": flips both orbit deltas.
    void setOrbitInverted(bool inverted) noexcept { orbitInverted_ = inverted; }
    void frameAll();
    void frameSelection();
    // Marks the surface as needing a redraw. Everything that can change what the
    // viewport looks like funnels through here, so the editor can poll one flag
    // instead of guessing when a repaint is due.
    void invalidate();
    // Draws when the surface is visible, and reports whether it did. Called once
    // per editor frame.
    //
    // Why "visible" and not "dirty": the child window is composited by Windows,
    // not by the editor's own frame, so a frame that is drawn but never
    // re-composited leaves the panel showing stale pixels -- the reported
    // "blank until I click or move inside it". Repainting every visible frame
    // costs one bool test when nothing changed and one GL frame otherwise,
    // which is what every 3D editor does; a hidden child is skipped outright,
    // so an occluded or closed viewport costs nothing at all.
    bool renderIfVisible();
    // Legacy entry point kept for callers that only want an explicit repaint
    // (tests, one-shot draws): draws only when something invalidated the scene.
    bool renderIfDirty();

    void setOnSelect(SelectCallback callback) { onSelect_ = std::move(callback); }
    void setOnSelectMany(SelectManyCallback callback) { onSelectMany_ = std::move(callback); }
    void setOnSelectRect(SelectRectCallback callback) { onSelectRect_ = std::move(callback); }
    void setOnSelectRail(SelectRailCallback callback) { onSelectRail_ = std::move(callback); }
    // Highlights one rail (pathIndex only) or one of its points (full ref);
    // nullopt clears. The draw pass thickens the matching batches and rings
    // the selected cube, so selection is visible without a gizmo.
    void setRailHighlight(std::optional<RailPointRef> highlight) noexcept;
    void setOnGizmo(GizmoCallback callback) { onGizmo_ = std::move(callback); }
    [[nodiscard]] ViewportCamera& camera() noexcept { return camera_; }
    // Drops every uploaded model texture (call when the GL context is about
    // to die, or after the texture setting/filter changes).
    void clearModelTextures() noexcept { textureCache_.clear(); }
    // Model texturing: on = per-material TEX1 textures modulated by triangle
    // color (Java parity); off = legacy flat material colors. Filter is
    // "nearest" or "linear" from settings.textureFilter.
    void setTexturedModels(bool textured) noexcept {
        if (textured_ == textured) { return; }
        textured_ = textured;
        invalidate();
    }
    void setTextureFilter(const char* filter) noexcept;
    // Model translucency: on = two-pass draw (opaque with depth writes, then
    // the blended pass with depth writes off); off = single opaque pass, the
    // legacy behaviour. Mirrors settings.translucentModels.
    void setTranslucentModels(bool translucent) noexcept {
        if (translucent_ == translucent) { return; }
        translucent_ = translucent;
        invalidate();
    }

private:
    // Cached GL display lists for one model mesh: `shaded` bakes the material
    // diffuse colors, `plain` leaves the color to the current GL state so
    // selection/hover can tint the whole model in one call.
    struct ModelListEntry {
        std::weak_ptr<const ModelMesh> mesh;
        unsigned int shaded{0};
        unsigned int plain{0};
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool initGL();
    void shutdownGL() noexcept;
    // The GL body of one frame, drawn ONLY from the editor's own loop (via
    // renderIfVisible/renderIfDirty, after the shell's Present) -- WM_PAINT
    // never draws, because anything composited before the present is erased
    // by it.
    void drawFrame();
    void paint();
    void updateSize(int width, int height);
    void applyCameraToGL(int width, int height);
    // Which scene-wide model pass is drawing. drawFrame runs one box loop per
    // pass (Java's GalaxyEditorForm renders OPAQUE then TRANSLUCENT through
    // the same renderAllObjects loop twice), so drawShape needs the pass to
    // skip the drawing that belongs to the other loop.
    enum class ModelPass { Opaque, Translucent };
    void drawShape(const ViewportBox& box, bool selected, bool hovered, ModelPass pass);
    void drawGizmo();
    void drawOverlays();
    // Flat path (display lists + untextured immediate). Textured models never
    // use display lists -- see drawTexturedModel below.
    void drawModelTriangles(const ModelMesh& mesh, bool bakeColors, const char* filter = nullptr,
                            ModelTextureCache* textures = nullptr);
    // Textured immediate path: per-triangle material texture binds from the
    // upload cache, modulated by triangle colour (Java BmdRenderer parity),
    // split into the opaque + blended translucent passes. selected/hovered
    // swap the material colour for the same highlight the flat path uses.
    void drawTexturedModel(const std::shared_ptr<const ModelMesh>& mesh, const char* filter,
                           bool selected, bool hovered);
    unsigned int modelDisplayList(const std::shared_ptr<const ModelMesh>& mesh, bool plain);
    void pruneModelLists() noexcept;
    void drawGrid();
    // Legend + name labels, drawn INSIDE the frame (before SwapBuffers) as
    // textured quads from a baked font atlas. The old path drew them with GDI
    // onto the front buffer after the swap, which raced the desktop compositor
    // and flickered -- worst with labels on.
    void drawLabels();
    // WASD/arrow fly movement, polled once per frame while this child holds
    // keyboard focus. Java's keyMask parity: E/Q (and PgUp/PgDn) vertical,
    // Shift x3 / Ctrl x0.25 speed modifiers.
    void pollFlyMovement();
    // Scene-aware pick reach: a galaxy framed past 20000 units used to become
    // unclickable beyond that distance.
    [[nodiscard]] float pickDistance() const noexcept;
    std::optional<std::size_t> pickAt(int x, int y);
    // Screen-space rectangle pick used by the Shift-drag marquee.
    [[nodiscard]] std::vector<std::size_t> pickRect(int x0, int y0, int x1, int y1);

    HWND window_{nullptr};
    HDC device_{nullptr};
    HGLRC glContext_{nullptr};
    int width_{1};
    int height_{1};
    // Set by invalidate(), cleared by a completed frame. Pending work survives
    // the child being hidden, so revealing it again draws a fresh image instead
    // of the undefined contents a hidden GL surface comes back with.
    bool dirty_{true};
    ViewportCamera camera_{};
    ViewportScene scene_{};
    std::optional<std::size_t> selected_;
    std::vector<std::size_t> selection_; // full multi-selection (contains selected_)
    std::optional<std::size_t> hover_;
    SelectCallback onSelect_;
    SelectManyCallback onSelectMany_;
    SelectRectCallback onSelectRect_;
    SelectRailCallback onSelectRail_;
    std::optional<RailPointRef> railHighlight_;
    GizmoCallback onGizmo_;
    bool showLabels_{false};
    bool overlayDark_{true};
    GizmoMode gizmoMode_{GizmoMode::Translate};
    bool gizmoEnabled_{true};
    bool orbitInverted_{false}; // Settings > "Invert camera motion"
    // A gizmo drag in flight. While it runs, left-drag moves the selection
    // instead of panning the camera, exactly like every 3D editor.
    bool draggingGizmo_{false};
    GizmoDrag gizmoDrag_{};
    // Screen point and handle where the current gizmo gesture started, so the
    // click that begins a drag can be told apart from a plain pan.
    int gizmoGrabX_{0};
    int gizmoGrabY_{0};
    bool draggingGizmoStarted_{false};
    // Where the gizmo sits: the first selected object, or the centroid of the
    // whole selection when several are active.
    [[nodiscard]] math::Vec3f gizmoAnchor() const noexcept;
    // Shift-drag marquee state. While armed, left-drag draws a rubber band
    // instead of panning the camera.
    bool marqueeActive_{false};
    int marqueeX0_{0};
    int marqueeY0_{0};
    int marqueeX1_{0};
    int marqueeY1_{0};
    bool marqueeAdditive_{false};
    void drawMarquee();
    bool draggingLeft_{false};
    bool draggingRight_{false};
    bool draggingMiddle_{false}; // MMB drag pans, like every 3D editor
    int lastX_{0};
    int lastY_{0};
    bool leftMoved_{false};
    int wheelAccumulator_{0};   // pending raw wheel deltas, applied in whole notches
    bool trackingMouse_{false}; // TrackMouseEvent armed so hover clears on leave
    bool meshesFilled_{false};  // lazily built once per GL context
    std::vector<math::Vec3f> meshes_[5]; // unit triangles, one entry per CategoryStyle::Shape
    std::vector<ModelListEntry> modelLists_; // display lists, validated via weak_ptr
    ModelTextureCache textureCache_;         // uploaded TEX1/BTI base levels
    bool textured_{true};                    // settings.texturedModels mirror
    bool translucent_{true};                 // settings.translucentModels mirror
    // Set by drawShape while a textured pass runs; drawTexturedModel reads it
    // to select its triangles and depth mask for the current pass.
    ModelPass modelPass_{ModelPass::Opaque};
    // Fixed-size filter mirror (no std::string in the render header): the GUI
    // pushes settings.textureFilter here on every refresh.
    char textureFilter_[8]{"linear"};

    // Fly-movement clock: reset whenever focus is lost or no key is held, so
    // pressing W after a pause never applies the paused time as one jump.
    std::chrono::steady_clock::time_point lastFlyTick_{};
};

} // namespace whitehole::render

#endif // _WIN32
