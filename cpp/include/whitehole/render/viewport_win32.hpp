#pragma once

// Win32 + WGL viewport window. Depends on Win32/OpenGL by design; the pure
// camera + scene math above stays portable and unit-tested.
//
// Lifetime: create() makes a child window of the editor, destroy() tears the
// GL context down. setScene() copies oriented boxes in (cheap pointer-free
// structs). Input mirrors Java GalaxyRenderer: left-drag pan, right-drag
// orbit, wheel dolly, click select, Space frame selection.
//
// Objects with a real game model (ViewportBox::model) are drawn from GL
// display lists compiled once per mesh, so a whole zone of BMD models still
// redraws cheaply; objects without one keep the category placeholder shapes.

#ifdef _WIN32

#include "whitehole/render/camera.hpp"
#include "whitehole/render/gizmo.hpp"
#include "whitehole/render/model_mesh.hpp"
#include "whitehole/render/viewport_scene.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

namespace whitehole::render {

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
    // Which transform handles the gizmo shows: move, rotate or scale (W/E/R).
    void setGizmoMode(GizmoMode mode) noexcept;
    // Hides the gizmo when no handle should be offered (no selection, or a
    // text field has focus).
    void setGizmoEnabled(bool enabled) noexcept;
    // Legend/label ink follows the theme instead of the hard-coded dark box the
    // overlay used to paint, which looked wrong inside a light-mode workspace.
    void setOverlayTheme(bool dark) noexcept;
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
    void setOnSelectRail(SelectRailCallback callback) { onSelectRail_ = std::move(callback); }
    // Highlights one rail (pathIndex only) or one of its points (full ref);
    // nullopt clears. The draw pass thickens the matching batches and rings
    // the selected cube, so selection is visible without a gizmo.
    void setRailHighlight(std::optional<RailPointRef> highlight) noexcept;
    void setOnGizmo(GizmoCallback callback) { onGizmo_ = std::move(callback); }
    [[nodiscard]] ViewportCamera& camera() noexcept { return camera_; }

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
    // The GL body of one frame, split out of paint() so it can also run from the
    // editor's own loop rather than only inside a WM_PAINT.
    void drawFrame();
    void paint();
    void updateSize(int width, int height);
    void applyCameraToGL(int width, int height);
    void drawShape(const ViewportBox& box, bool selected, bool hovered);
    void drawGizmo();
    void drawOverlays();
    void drawModelTriangles(const ModelMesh& mesh, bool bakeColors);
    unsigned int modelDisplayList(const std::shared_ptr<const ModelMesh>& mesh, bool plain);
    void pruneModelLists() noexcept;
    void drawGrid();
    void drawOverlay(HDC device);
    std::optional<std::size_t> pickAt(int x, int y);

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
    SelectRailCallback onSelectRail_;
    std::optional<RailPointRef> railHighlight_;
    GizmoCallback onGizmo_;
    bool showLabels_{false};
    bool overlayDark_{true};
    GizmoMode gizmoMode_{GizmoMode::Translate};
    bool gizmoEnabled_{true};
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
    bool draggingLeft_{false};
    bool draggingRight_{false};
    int lastX_{0};
    int lastY_{0};
    bool leftMoved_{false};
    int wheelAccumulator_{0};   // pending raw wheel deltas, applied in whole notches
    bool trackingMouse_{false}; // TrackMouseEvent armed so hover clears on leave
    bool meshesFilled_{false};  // lazily built once per GL context
    std::vector<math::Vec3f> meshes_[5]; // unit triangles, one entry per CategoryStyle::Shape
    std::vector<ModelListEntry> modelLists_; // display lists, validated via weak_ptr
};

} // namespace whitehole::render

#endif // _WIN32
