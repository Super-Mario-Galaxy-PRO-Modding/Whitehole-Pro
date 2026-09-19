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

    bool create(HWND parent, int controlId, HINSTANCE instance);
    void destroy() noexcept;
    [[nodiscard]] bool valid() const noexcept { return window_ != nullptr && glContext_ != nullptr; }
    [[nodiscard]] HWND handle() const noexcept { return window_; }

    void setScene(ViewportScene scene);
    void setSelected(std::optional<std::size_t> selected);
    void setHover(std::optional<std::size_t> hover);
    void setShowLabels(bool showLabels) noexcept;
    void frameAll();
    void frameSelection();
    void invalidate();

    void setOnSelect(SelectCallback callback) { onSelect_ = std::move(callback); }
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
    void paint();
    void updateSize(int width, int height);
    void applyCameraToGL(int width, int height);
    void drawShape(const ViewportBox& box, bool selected, bool hovered);
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
    ViewportCamera camera_{};
    ViewportScene scene_{};
    std::optional<std::size_t> selected_;
    std::optional<std::size_t> hover_;
    SelectCallback onSelect_;
    bool showLabels_{false};
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
