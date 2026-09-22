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

#include "whitehole/render/viewport_win32.hpp"

#ifdef _WIN32

#include <windowsx.h>

#include <GL/gl.h>


#include "whitehole/app/theme_palette.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#pragma comment(lib, "opengl32.lib")

namespace whitehole::render {
namespace {

constexpr wchar_t kClassName[] = L"WhiteholeProViewport";
bool classRegistered = false;

// Number of shapes matches CategoryStyle::Shape; meshes are filled lazily on
// first paint while the GL context is current.
constexpr int kShapeCount = 5;

void ensureMeshes(bool& filled, std::vector<math::Vec3f>* meshes) {
    if (filled) {
        return;
    }
    for (int shape = 0; shape < kShapeCount; ++shape) {
        meshes[shape] = shapeTriangles(static_cast<CategoryStyle::Shape>(shape));
    }
    filled = true;
}

// Flat-shaded triangle mesh draw: normals come from each face so the shapes
// read as 3D volumes instead of flat silhouettes on the dark background.
void drawTriangles(const std::vector<math::Vec3f>& triangles) {
    glBegin(GL_TRIANGLES);
    for (std::size_t index = 0; index + 2 < triangles.size(); index += 3) {
        const math::Vec3f& a = triangles[index];
        const math::Vec3f& b = triangles[index + 1];
        const math::Vec3f& c = triangles[index + 2];
        math::Vec3f normal = math::Vec3f::cross(b - a, c - a);
        const float length = normal.length();
        if (length > 0.000001F) {
            normal = normal * (1.0F / length);
        } else {
            normal = {0.0F, 1.0F, 0.0F};
        }
        glNormal3f(normal.x, normal.y, normal.z);
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
        glVertex3f(c.x, c.y, c.z);
    }
    glEnd();
}

// Cap on cached model display lists. Two lists per mesh (shaded + plain) keep
// even a fully modelled zone well inside this budget; the oldest entries are
// evicted FIFO, and expired entries are pruned whenever new ones arrive.
constexpr std::size_t kMaxModelListEntries = 128;

} // namespace

void ViewportWindow::drawModelTriangles(const ModelMesh& mesh, bool bakeColors) {
    glBegin(GL_TRIANGLES);
    for (const auto& triangle : mesh.triangles) {
        if (bakeColors) {
            glColor4f(triangle.color[0], triangle.color[1], triangle.color[2], triangle.color[3]);
        }
        const ModelVertex* vertices[3] = {&triangle.a, &triangle.b, &triangle.c};
        for (const ModelVertex* vertex : vertices) {
            glNormal3f(vertex->normal.x, vertex->normal.y, vertex->normal.z);
            glVertex3f(vertex->position.x, vertex->position.y, vertex->position.z);
        }
    }
    glEnd();
}

unsigned int ViewportWindow::modelDisplayList(const std::shared_ptr<const ModelMesh>& mesh, bool plain) {
    for (auto& entry : modelLists_) {
        if (entry.mesh.expired()) {
            continue;
        }
        if (entry.mesh.lock() != mesh) {
            continue;
        }
        unsigned int& list = plain ? entry.plain : entry.shaded;
        if (list == 0) {
            list = glGenLists(1);
            if (list != 0) {
                glNewList(list, GL_COMPILE);
                drawModelTriangles(*mesh, !plain);
                glEndList();
            }
        }
        return list;
    }

    pruneModelLists();
    ModelListEntry entry;
    entry.mesh = mesh;
    unsigned int& list = plain ? entry.plain : entry.shaded;
    list = glGenLists(1);
    if (list != 0) {
        glNewList(list, GL_COMPILE);
        drawModelTriangles(*mesh, !plain);
        glEndList();
    }
    modelLists_.push_back(std::move(entry));
    return list;
}

void ViewportWindow::pruneModelLists() noexcept {
    // Drop entries whose mesh was evicted from the scene (weak_ptr expired) so
    // stale display lists cannot leak; then apply the FIFO cap.
    modelLists_.erase(std::remove_if(modelLists_.begin(), modelLists_.end(),
                                     [](const ModelListEntry& entry) { return entry.mesh.expired(); }),
                      modelLists_.end());
    while (modelLists_.size() >= kMaxModelListEntries) {
        if (modelLists_.front().shaded != 0) {
            glDeleteLists(modelLists_.front().shaded, 1);
        }
        if (modelLists_.front().plain != 0) {
            glDeleteLists(modelLists_.front().plain, 1);
        }
        modelLists_.erase(modelLists_.begin());
    }
}

ViewportWindow::ViewportWindow() = default;
ViewportWindow::~ViewportWindow() {
    destroy();
}

bool ViewportWindow::create(HWND parent, int controlId, HINSTANCE instance) {
    destroy();
    if (!classRegistered) {
        WNDCLASSW cls{};
        cls.lpfnWndProc = &ViewportWindow::windowProc;
        cls.hInstance = instance;
        cls.lpszClassName = kClassName;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.style = CS_OWNDC;
        if (RegisterClassW(&cls) == 0) {
            return false;
        }
        classRegistered = true;
    }
    // Created hidden: syncViewportChild reveals it once the panel has reported a
    // real rectangle. Creating it visible used to paint a 10x10 GL surface at
    // the top-left corner of the workspace during the boot frames before the
    // dock layout existed.
    window_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 10,
                              10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), instance, this);
    if (window_ == nullptr) {
        return false;
    }
    if (!initGL()) {
        destroy();
        return false;
    }
    return true;
}

void ViewportWindow::destroy() noexcept {
    shutdownGL();
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

void ViewportWindow::setScene(ViewportScene scene) {
    scene_ = std::move(scene);
    if (selected_.has_value() && *selected_ >= scene_.boxes().size()) {
        selected_.reset();
    }
    if (hover_.has_value() && *hover_ >= scene_.boxes().size()) {
        hover_.reset();
    }
    // A rebuild re-indexes every box, so the multi-selection cannot survive it.
    selection_.clear();
    invalidate();
}

void ViewportWindow::setSelected(std::optional<std::size_t> selected) {
    selected_ = selected;
    selection_.clear();
    if (selected.has_value()) {
        selection_.push_back(*selected);
    }
    invalidate();
}

void ViewportWindow::setSelection(std::vector<std::size_t> selected) {
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    selection_ = std::move(selected);
    selected_ = selection_.empty() ? std::optional<std::size_t>{} : selection_.front();
    invalidate();
}

void ViewportWindow::setGizmoMode(GizmoMode mode) noexcept {
    if (gizmoMode_ == mode) {
        return;
    }
    gizmoMode_ = mode;
    invalidate();
}

void ViewportWindow::setGizmoEnabled(bool enabled) noexcept {
    if (gizmoEnabled_ == enabled) {
        return;
    }
    gizmoEnabled_ = enabled;
    invalidate();
}

void ViewportWindow::setHover(std::optional<std::size_t> hover) {
    hover_ = hover;
    invalidate();
}

void ViewportWindow::setShowLabels(bool showLabels) noexcept {
    showLabels_ = showLabels;
    invalidate();
}

void ViewportWindow::setOverlayTheme(bool dark) noexcept {
    if (overlayDark_ == dark) {
        return;
    }
    overlayDark_ = dark;
    invalidate();
}

void ViewportWindow::frameAll() {
    if (scene_.empty()) {
        return;
    }
    camera_.frameTarget(scene_.center(), scene_.frameDistance());
    invalidate();
}

void ViewportWindow::frameSelection() {
    if (selection_.empty()) {
        frameAll();
        return;
    }
    // Frame the whole selection, not just the first object: a two-object
    // selection framed as one object would still leave the other off-screen.
    math::Vec3f low{0.0F, 0.0F, 0.0F};
    math::Vec3f high{0.0F, 0.0F, 0.0F};
    bool any = false;
    for (const auto index : selection_) {
        if (index >= scene_.boxes().size()) {
            continue;
        }
        const auto& box = scene_.boxes()[index];
        if (!any) {
            low = {box.center.x - box.halfExtents.x, box.center.y - box.halfExtents.y,
                   box.center.z - box.halfExtents.z};
            high = {box.center.x + box.halfExtents.x, box.center.y + box.halfExtents.y,
                    box.center.z + box.halfExtents.z};
            any = true;
            continue;
        }
        low = {std::min(low.x, box.center.x - box.halfExtents.x),
               std::min(low.y, box.center.y - box.halfExtents.y),
               std::min(low.z, box.center.z - box.halfExtents.z)};
        high = {std::max(high.x, box.center.x + box.halfExtents.x),
                std::max(high.y, box.center.y + box.halfExtents.y),
                std::max(high.z, box.center.z + box.halfExtents.z)};
    }
    if (!any) {
        frameAll();
        return;
    }
    const math::Vec3f center{(low.x + high.x) * 0.5F, (low.y + high.y) * 0.5F,
                             (low.z + high.z) * 0.5F};
    const math::Vec3f extent{high.x - low.x, high.y - low.y, high.z - low.z};
    const float radius = std::max(std::max(extent.x, extent.y), extent.z) * 0.5F;
    // Distance so the selection fills the view: 45 degrees at the FOV's half
    // angle, with a margin so it never touches the screen edge.
    const float framed = radius > 0.0F ? radius / std::tan(ViewportCamera::kFieldOfView * 0.5F) * 1.6F
                                       : 300.0F;
    camera_.frameTarget(center, std::max(framed, 300.0F));
    invalidate();
}

void ViewportWindow::invalidate() {
    dirty_ = true;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

LRESULT CALLBACK ViewportWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ViewportWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ViewportWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = window;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (self != nullptr) {
        return self->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ViewportWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_SIZE:
        updateSize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        SetCapture(window_);
        draggingLeft_ = true;
        leftMoved_ = false;
        lastX_ = GET_X_LPARAM(lParam);
        lastY_ = GET_Y_LPARAM(lParam);
        // The gizmo claims the gesture when the click lands on one of its
        // handles; a miss falls through to the plain camera pan.
        draggingGizmo_ = false;
        if (gizmoEnabled_ && !selection_.empty()) {
            const auto anchor = gizmoAnchor();
            RECT rect{};
            GetClientRect(window_, &rect);
            const float width = static_cast<float>(rect.right - rect.left);
            const float height = static_cast<float>(rect.bottom - rect.top);
            const auto handle =
                pickGizmoHandle(camera_, anchor, static_cast<float>(lastX_),
                                static_cast<float>(lastY_), width, height);
            if (handle != GizmoHandle::None &&
                beginGizmoDrag(gizmoDrag_, camera_, anchor, gizmoMode_, handle,
                               static_cast<float>(lastX_), static_cast<float>(lastY_), width,
                               height)) {
                draggingGizmo_ = true;
                gizmoGrabX_ = lastX_;
                gizmoGrabY_ = lastY_;
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (draggingGizmo_) {
            draggingGizmo_ = false;
            ReleaseCapture();
            if (onGizmo_) {
                RECT rect{};
                GetClientRect(window_, &rect);
                const float width = static_cast<float>(rect.right - rect.left);
                const float height = static_cast<float>(rect.bottom - rect.top);
                GizmoEdit edit;
                edit.phase = GizmoPhase::End;
                edit.mode = gizmoDrag_.mode;
                edit.handle = gizmoDrag_.handle;
                edit.value = gizmoDragValue(gizmoDrag_, camera_, static_cast<float>(lastX_),
                                            static_cast<float>(lastY_), width, height);
                onGizmo_(edit);
            }
            return 0;
        }
        if (draggingLeft_) {
            ReleaseCapture();
            draggingLeft_ = false;
            // A click (no drag) picks: rail point cubes claim the click first
            // (they are small, deliberate targets), otherwise the plain object
            // flow runs -- plain click replaces the selection, Ctrl/Shift-click
            // toggles it, matching every 3D editor.
            if (!leftMoved_) {
                RECT rect{};
                GetClientRect(window_, &rect);
                const float width = static_cast<float>(rect.right - rect.left);
                const float height = static_cast<float>(rect.bottom - rect.top);
                const auto railHit = scene_.pickRailPoint(
                    camera_, static_cast<float>(GET_X_LPARAM(lParam)),
                    static_cast<float>(GET_Y_LPARAM(lParam)), width, height);
                if (railHit.has_value()) {
                    if (onSelectRail_) {
                        onSelectRail_(*railHit);
                    }
                    invalidate();
                    return 0;
                }
                const auto picked = pickAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                const bool additive = (wParam & (MK_SHIFT | MK_CONTROL)) != 0;
                if (onSelectMany_) {
                    onSelectMany_(picked, additive);
                } else if (onSelect_ && !additive) {
                    onSelect_(picked);
                }
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        SetFocus(window_);
        SetCapture(window_);
        draggingRight_ = true;
        lastX_ = GET_X_LPARAM(lParam);
        lastY_ = GET_Y_LPARAM(lParam);
        return 0;
    case WM_RBUTTONUP:
        if (draggingRight_) {
            ReleaseCapture();
            draggingRight_ = false;
        }
        return 0;
    case WM_MOUSEMOVE: {
        // Arm hover tracking so WM_MOUSELEAVE clears the highlight when the
        // cursor leaves the viewport (without this, a hovered box stays lit).
        if (!trackingMouse_ && window_ != nullptr) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window_;
            if (TrackMouseEvent(&track) != 0) {
                trackingMouse_ = true;
            }
        }
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int dx = x - lastX_;
        const int dy = y - lastY_;
        lastX_ = x;
        lastY_ = y;
        if (draggingGizmo_ && (wParam & MK_LBUTTON) != 0) {
            // Live gizmo drag: the selection follows the cursor frame by frame;
            // the editor turns the Begin/Update/End messages into one undo step.
            if (onGizmo_) {
                RECT rect{};
                GetClientRect(window_, &rect);
                const float width = static_cast<float>(rect.right - rect.left);
                const float height = static_cast<float>(rect.bottom - rect.top);
                GizmoEdit edit;
                edit.phase = draggingGizmoStarted_ ? GizmoPhase::Update : GizmoPhase::Begin;
                draggingGizmoStarted_ = true;
                edit.mode = gizmoDrag_.mode;
                edit.handle = gizmoDrag_.handle;
                edit.value = gizmoDragValue(gizmoDrag_, camera_, static_cast<float>(x),
                                            static_cast<float>(y), width, height);
                onGizmo_(edit);
            }
            invalidate();
        } else if (draggingLeft_ && (wParam & MK_LBUTTON) != 0) {
            if (dx != 0 || dy != 0) {
                leftMoved_ = true;
            }
            camera_.pan(static_cast<float>(dx), static_cast<float>(dy));
            invalidate();
        } else if (draggingRight_ && (wParam & MK_RBUTTON) != 0) {
            camera_.orbit(static_cast<float>(dx) * 0.008F, static_cast<float>(dy) * 0.008F);
            invalidate();
        } else if (!draggingLeft_ && !draggingRight_) {
            const auto hovered = pickAt(x, y);
            if (hovered != hover_) {
                hover_ = hovered;
                invalidate();
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hover_.has_value()) {
            hover_.reset();
            invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // Accumulate raw deltas and apply whole notches, so high-resolution
        // wheels and multi-notch flicks zoom smoothly instead of losing input.
        wheelAccumulator_ += GET_WHEEL_DELTA_WPARAM(wParam);
        int notches = 0;
        while (wheelAccumulator_ >= WHEEL_DELTA) {
            ++notches;
            wheelAccumulator_ -= WHEEL_DELTA;
        }
        while (wheelAccumulator_ <= -WHEEL_DELTA) {
            --notches;
            wheelAccumulator_ += WHEEL_DELTA;
        }
        if (notches != 0) {
            camera_.dolly(static_cast<float>(notches));
            invalidate();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            frameSelection();
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

bool ViewportWindow::initGL() {
    device_ = GetDC(window_);
    if (device_ == nullptr) {
        return false;
    }
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.iLayerType = PFD_MAIN_PLANE;
    const int pixel = ChoosePixelFormat(device_, &format);
    if (pixel == 0 || SetPixelFormat(device_, pixel, &format) == FALSE) {
        ReleaseDC(window_, device_);
        device_ = nullptr;
        return false;
    }
    glContext_ = wglCreateContext(device_);
    if (glContext_ == nullptr) {
        ReleaseDC(window_, device_);
        device_ = nullptr;
        return false;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    width_ = rect.right - rect.left > 0 ? rect.right - rect.left : 1;
    height_ = rect.bottom - rect.top > 0 ? rect.bottom - rect.top : 1;
    return true;
}

void ViewportWindow::shutdownGL() noexcept {
    if (glContext_ != nullptr) {
        // Display lists belong to the context; delete them while it is still
        // current instead of leaking them until the driver reclaims the
        // context itself.
        if (device_ != nullptr) {
            wglMakeCurrent(device_, glContext_);
        }
        for (const auto& entry : modelLists_) {
            if (entry.shaded != 0) {
                glDeleteLists(entry.shaded, 1);
            }
            if (entry.plain != 0) {
                glDeleteLists(entry.plain, 1);
            }
        }
        modelLists_.clear();
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(glContext_);
        glContext_ = nullptr;
    }
    if (device_ != nullptr && window_ != nullptr) {
        ReleaseDC(window_, device_);
        device_ = nullptr;
    }
}

// The GL body of one frame. Kept separate from paint() because the window class
// is CS_OWNDC, so device_ is the window's own persistent DC and this can run
// either inside a WM_PAINT or from the editor's render loop with no DC juggling.
void ViewportWindow::drawFrame() {
    if (glContext_ != nullptr && device_ != nullptr) {
        wglMakeCurrent(device_, glContext_);
        ensureMeshes(meshesFilled_, meshes_);
        applyCameraToGL(width_, height_);
        drawGrid();
        // Simple directional lighting + color material: category colors stay
        // recognizable while per-face normals give the shapes depth.
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        const float ambient[]{0.42F, 0.43F, 0.48F, 1.0F};
        const float diffuse[]{0.72F, 0.71F, 0.68F, 1.0F};
        // Directional light fixed in eye space: always readable no matter
        // how the camera orbits.
        const float position[]{0.4F, 1.0F, 0.6F, 0.0F};
        glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
        glLightfv(GL_LIGHT0, GL_POSITION, position);
        glShadeModel(GL_FLAT);
        glEnable(GL_DEPTH_TEST);
        // Non-uniform object scales need normalisation so the fixed-function
        // pipeline renormalises the inverse-transpose-transformed normals
        // (display lists bake the normals, not the transform).
        glEnable(GL_NORMALIZE);
        pruneModelLists();
        for (const auto& box : scene_.boxes()) {
            const bool selected =
                std::find(selection_.begin(), selection_.end(), box.objectIndex) != selection_.end();
            const bool hovered = !selected && hover_.has_value() && *hover_ == box.objectIndex;
            drawShape(box, selected, hovered);
        }
        // World overlays (rails, cameras, areas, gravity, axis): unlit lines
        // that depth-test against the models so rails hide behind geometry.
        glDisable(GL_LIGHTING);
        drawOverlays();
        glEnable(GL_LIGHTING);
        if (!selection_.empty()) {
            glDisable(GL_LIGHTING);
            glDisable(GL_DEPTH_TEST);
            drawGizmo();
            glEnable(GL_DEPTH_TEST);
        }
        SwapBuffers(device_);
        wglMakeCurrent(nullptr, nullptr);
        drawOverlay(device_);
    }
    // A finished frame -- whichever path drew it -- leaves nothing outstanding.
    dirty_ = false;
}

void ViewportWindow::paint() {
    PAINTSTRUCT paintInfo{};
    BeginPaint(window_, &paintInfo);
    drawFrame();
    EndPaint(window_, &paintInfo);
}

bool ViewportWindow::renderIfVisible() {
    if (window_ == nullptr || glContext_ == nullptr || device_ == nullptr) {
        return false;
    }
    // A hidden surface must not be drawn into: the contents of a hidden
    // double-buffered GL window are undefined the moment it comes back, and the
    // caller re-invalidates when it is revealed. Everything else repaints.
    if (!IsWindowVisible(window_)) {
        dirty_ = true;
        return false;
    }
    drawFrame();
    // Cancel the WM_PAINT invalidate() queued, so this redraw is not repeated
    // the next time the message queue drains.
    ValidateRect(window_, nullptr);
    return true;
}

bool ViewportWindow::renderIfDirty() {
    if (!dirty_ || window_ == nullptr || glContext_ == nullptr || device_ == nullptr) {
        return false;
    }
    drawFrame();
    // Cancel the WM_PAINT invalidate() queued, so this redraw is not repeated
    // the next time the message queue drains.
    ValidateRect(window_, nullptr);
    return true;
}

void ViewportWindow::updateSize(int width, int height) {
    width_ = width > 0 ? width : 1;
    height_ = height > 0 ? height : 1;
    if (window_ != nullptr) {
        invalidate();
    }
}

void ViewportWindow::applyCameraToGL(int width, int height) {
    const float safeHeight = static_cast<float>(height > 0 ? height : 1);
    const float aspect = static_cast<float>(width) / safeHeight;
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    const float half = ViewportCamera::kFieldOfView * 0.5F;
    const float tanHalf = static_cast<float>(std::tan(static_cast<double>(half)));
    const float nearPlane = ViewportCamera::kNearPlane;
    const float farPlane = ViewportCamera::kFarPlane;
    const float top = tanHalf * nearPlane;
    glFrustum(-top * aspect, top * aspect, -top, top, nearPlane, farPlane);
    glMatrixMode(GL_MODELVIEW);
    // Matrix4 keeps element (row, column) at values[4 * row + column], which is
    // exactly the column-major layout glLoadMatrixf expects, so the rendered
    // view is literally the CPU camera: screen picks can never drift from what
    // is drawn (the previous hand-built look matrix was a second copy of this
    // maths that had to be kept in sync by hand).
    const math::Matrix4 view = camera_.viewMatrix();
    glLoadMatrixf(view.values.data());
    glClearColor(0.09F, 0.11F, 0.15F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void ViewportWindow::drawShape(const ViewportBox& box, bool selected, bool hovered) {
    // Real game model path: the BMD triangles are drawn from a cached display
    // list under the object's placement transform. Smooth vertex normals +
    // material diffuse colors make the model read like the game's own render.
    if (box.model != nullptr && !box.model->empty()) {
        glPushMatrix();
        glMultMatrixf(box.world.values.data());
        glShadeModel(GL_SMOOTH);
        if (selected || hovered) {
            // Tinted pass: the plain list (no baked colors) with one highlight
            // color, so a highlighted object is unmistakable at any zoom.
            if (selected) {
                glColor3f(1.0F, 0.85F, 0.20F);
            } else {
                glColor3f(0.55F, 0.80F, 1.0F);
            }
            const unsigned int list = modelDisplayList(box.model, true);
            if (list != 0) {
                glCallList(list);
            } else {
                drawModelTriangles(*box.model, false);
            }
        } else {
            const unsigned int list = modelDisplayList(box.model, false);
            if (list != 0) {
                glCallList(list);
            } else {
                drawModelTriangles(*box.model, true);
            }
        }
        glShadeModel(GL_FLAT);
        glPopMatrix();
        return;
    }

    const CategoryStyle& style = categoryStyle(box.category);
    // Selection/hover tint the category color (instead of replacing it) so
    // the category stays readable while the object is clearly highlighted.
    float r = style.color[0];
    float g = style.color[1];
    float b = style.color[2];
    if (selected) {
        r = 0.35F * r + 0.65F * 1.00F;
        g = 0.35F * g + 0.65F * 0.85F;
        b = 0.35F * b + 0.65F * 0.20F;
    } else if (hovered) {
        r = 0.40F * r + 0.60F * 0.40F;
        g = 0.40F * g + 0.60F * 0.80F;
        b = 0.40F * b + 0.60F * 1.00F;
    }
    glPushMatrix();
    glMultMatrixf(box.world.values.data());
    const auto shapeIndex = static_cast<int>(style.shape);
    if (shapeIndex >= 0 && shapeIndex < kShapeCount) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0F, 1.0F);
        glColor3f(r, g, b);
        drawTriangles(meshes_[static_cast<std::size_t>(shapeIndex)]);
        glDisable(GL_POLYGON_OFFSET_FILL);
        // Edge pass: bright for selection/hover, subtle dark otherwise.
        glDisable(GL_LIGHTING);
        if (selected || hovered) {
            glColor3f(1, 1, 1);
        } else {
            glColor3f(0.12F, 0.15F, 0.22F);
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        drawTriangles(meshes_[static_cast<std::size_t>(shapeIndex)]);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_LIGHTING);
    }
    glPopMatrix();
}

std::wstring toWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void ViewportWindow::setRailHighlight(std::optional<RailPointRef> highlight) noexcept {
    railHighlight_ = highlight;
    invalidate();
}

void ViewportWindow::drawOverlays() {
    // One glBegin/glEnd per batch: batches carry a single colour and width, so
    // this stays one state change + one draw call per overlay group. Geometry
    // was tessellated at scene-rebuild time, never here.
    glDepthMask(GL_TRUE);
    for (const auto& batch : scene_.overlays()) {
        if (batch.segments.empty()) {
            continue;
        }
        float red = static_cast<float>((batch.color >> 24) & 0xFFu) / 255.0F;
        float green = static_cast<float>((batch.color >> 16) & 0xFFu) / 255.0F;
        float blue = static_cast<float>((batch.color >> 8) & 0xFFu) / 255.0F;
        const float alpha = static_cast<float>(batch.color & 0xFFu) / 255.0F;
        float width = batch.width;
        const bool highlighted = batch.railIndex != kNoRail && railHighlight_.has_value()
                                 && railHighlight_->pathIndex == batch.railIndex;
        if (highlighted) {
            // Brighten and thicken the selected rail so it reads instantly
            // against its siblings, whatever the theme.
            red = red + (1.0F - red) * 0.45F;
            green = green + (1.0F - green) * 0.45F;
            blue = blue + (1.0F - blue) * 0.45F;
            width = std::max(width * 2.0F, 3.0F);
        }
        glColor4f(red, green, blue, alpha);
        // Implementations clamp wide aliased lines to 1.0 silently; asking for
        // 1.5 still matches Java's glLineWidth(1.5) wherever it is honoured.
        glLineWidth(width);
        glBegin(GL_LINES);
        for (const auto& segment : batch.segments) {
            glVertex3f(segment.from.x, segment.from.y, segment.from.z);
            glVertex3f(segment.to.x, segment.to.y, segment.to.z);
        }
        glEnd();
    }
    // Ring around the selected point (or control handle): one bright cube so
    // the selection is findable even when its rail is long.
    if (railHighlight_.has_value() && railHighlight_->pointIndex != kNoRail) {
        const auto& paths = scene_.railPaths();
        if (railHighlight_->pathIndex < paths.size()) {
            const auto& points = paths[railHighlight_->pathIndex].points;
            if (railHighlight_->pointIndex < points.size()) {
                const auto& point = points[railHighlight_->pointIndex];
                const math::Vec3f* center = &point.position;
                if (railHighlight_->part == 1) {
                    center = &point.control1;
                } else if (railHighlight_->part == 2) {
                    center = &point.control2;
                }
                glColor4f(1.0F, 0.92F, 0.25F, 1.0F);
                glLineWidth(2.0F);
                const float half = 65.0F;
                glBegin(GL_LINES);
                const math::Vec3f corners[8] = {
                    {center->x - half, center->y - half, center->z - half},
                    {center->x + half, center->y - half, center->z - half},
                    {center->x - half, center->y + half, center->z - half},
                    {center->x + half, center->y + half, center->z - half},
                    {center->x - half, center->y - half, center->z + half},
                    {center->x + half, center->y - half, center->z + half},
                    {center->x - half, center->y + half, center->z + half},
                    {center->x + half, center->y + half, center->z + half}};
                constexpr int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                              {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& edge : edges) {
                    glVertex3f(corners[edge[0]].x, corners[edge[0]].y, corners[edge[0]].z);
                    glVertex3f(corners[edge[1]].x, corners[edge[1]].y, corners[edge[1]].z);
                }
                glEnd();
                glLineWidth(1.0F);
            }
        }
    }
    glLineWidth(1.0F);
}

void ViewportWindow::drawOverlay(HDC device) {
    // Legend + labels are GDI drawn on the visible (front) buffer after the
    // swap, so they never flicker with the GL scene. Colours come from the
    // palette so the overlay matches the theme the rest of the workspace uses.
    const app::Palette& palette = app::themePalette(overlayDark_);
    const auto toColorref = [](const app::Rgba& color) {
        return RGB(static_cast<int>(color.r * 255.0F + 0.5F),
                   static_cast<int>(color.g * 255.0F + 0.5F),
                   static_cast<int>(color.b * 255.0F + 0.5F));
    };
    const COLORREF panelColor = toColorref(palette.panelBg);
    const COLORREF textColor = toColorref(palette.text);
    const COLORREF accentColor = toColorref(palette.unsaved);

    const HFONT oldFont = static_cast<HFONT>(SelectObject(device, GetStockObject(DEFAULT_GUI_FONT)));
    SetBkMode(device, TRANSPARENT);

    // Per-category counts: the map key for the color system.
    constexpr std::size_t kCategoryTotal = 10;
    std::array<int, kCategoryTotal> counts{};
    for (const auto& box : scene_.boxes()) {
        const auto index = static_cast<std::size_t>(box.category);
        if (index < counts.size()) {
            counts[index]++;
        }
    }

    int textWidth = 0;
    int legendLines = 0;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index] == 0) {
            continue;
        }
        legendLines++;
        wchar_t line[128];
        const auto& style = categoryStyle(static_cast<ObjectCategory>(index));
        _snwprintf_s(line, _TRUNCATE, L"%hs \u00D7 %d", style.label, counts[index]);
        SIZE extent{};
        GetTextExtentPoint32W(device, line, static_cast<int>(wcsnlen_s(line, 128)), &extent);
        textWidth = std::max(textWidth, static_cast<int>(extent.cx));
    }

    constexpr int chipSize = 10;
    constexpr int lineStep = 18;
    constexpr int padding = 8;
    constexpr int legendX = 10;
    int legendY = 10;
    if (legendLines > 0) {
        const int legendWidth = textWidth + chipSize + 6 + padding * 2;
        const int legendHeight = legendLines * lineStep + padding * 2;
        RECT background{legendX, legendY, legendX + legendWidth, legendY + legendHeight};
        HBRUSH backBrush = CreateSolidBrush(panelColor);
        FillRect(device, &background, backBrush);
        DeleteObject(backBrush);

        int line = 0;
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] == 0) {
                continue;
            }
            const auto& style = categoryStyle(static_cast<ObjectCategory>(index));
            const int y = legendY + padding + line * lineStep;
            RECT chip{legendX + padding, y + 3, legendX + padding + chipSize, y + 3 + chipSize};
            HBRUSH chipBrush = CreateSolidBrush(
                RGB(static_cast<int>(style.color[0] * 255.0F), static_cast<int>(style.color[1] * 255.0F),
                    static_cast<int>(style.color[2] * 255.0F)));
            FillRect(device, &chip, chipBrush);
            DeleteObject(chipBrush);
            wchar_t text[128];
            _snwprintf_s(text, _TRUNCATE, L"%hs \u00D7 %d", style.label, counts[index]);
            SetTextColor(device, textColor);
            TextOutW(device, legendX + padding + chipSize + 6, y, text, static_cast<int>(wcsnlen_s(text, 128)));
            line++;
        }
        legendY += legendHeight + 6;
    }

    // Selected object line directly under the legend.
    if (selected_.has_value() && *selected_ < scene_.boxes().size()) {
        const auto& box = scene_.boxes()[*selected_];
        const auto& style = categoryStyle(box.category);
        const std::wstring text = toWide(box.name + " \u2014 " + style.label + " (" + box.kind + ")");
        if (!text.empty()) {
            SIZE extent{};
            GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &extent);
            RECT background{legendX, legendY, legendX + extent.cx + padding * 2, legendY + 22};
            HBRUSH backBrush = CreateSolidBrush(panelColor);
            FillRect(device, &background, backBrush);
            DeleteObject(backBrush);
            SetTextColor(device, accentColor);
            TextOutW(device, legendX + padding, legendY + 2, text.c_str(), static_cast<int>(text.size()));
        }
    }

    // Object name labels. Hovered/selected are always labeled; the View menu
    // toggle labels every object for surveying the scene.
    if (showLabels_ || selected_.has_value() || hover_.has_value()) {
        for (const auto& box : scene_.boxes()) {
            const bool isSelected = selected_.has_value() && *selected_ == box.objectIndex;
            const bool isHovered = hover_.has_value() && *hover_ == box.objectIndex;
            if (!showLabels_ && !isSelected && !isHovered) {
                continue;
            }
            const math::Vec3f labelPoint{box.center.x, box.center.y + box.halfExtents.y + 6.0F, box.center.z};
            float x = 0.0F;
            float y = 0.0F;
            if (!camera_.worldToScreen(labelPoint, static_cast<float>(width_), static_cast<float>(height_), x, y)) {
                continue;
            }
            const std::wstring text = toWide(box.name);
            if (text.empty()) {
                continue;
            }
            SIZE extent{};
            GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &extent);
            const int left = static_cast<int>(x) - extent.cx / 2;
            // Drop shadow keeps labels readable over bright geometry.
            SetTextColor(device, overlayDark_ ? RGB(8, 10, 14) : RGB(255, 255, 255));
            TextOutW(device, left + 1, static_cast<int>(y) + 1, text.c_str(), static_cast<int>(text.size()));
            SetTextColor(device, isSelected ? accentColor : textColor);
            TextOutW(device, left, static_cast<int>(y), text.c_str(), static_cast<int>(text.size()));
        }
    }
    SelectObject(device, oldFont);
}

void ViewportWindow::drawGrid() {
    // The grid follows the camera: the step scales with the orbit distance and
    // the patch stays centred (and snapped) on the orbit target, so panning
    // across a galaxy never scrolls the grid out of view.
    const float step = std::clamp(camera_.distance / 20.0F, 25.0F, 2000.0F);
    constexpr int halfLines = 20;
    const float extent = step * static_cast<float>(halfLines);
    const float centerX = std::floor(camera_.target.x / step + 0.5F) * step;
    const float centerZ = std::floor(camera_.target.z / step + 0.5F) * step;

    glDisable(GL_DEPTH_TEST);
    glBegin(GL_LINES);
    glColor3f(0.22F, 0.26F, 0.33F);
    for (int i = -halfLines; i <= halfLines; ++i) {
        const float offset = static_cast<float>(i) * step;
        glVertex3f(centerX + offset, 0.0F, centerZ - extent);
        glVertex3f(centerX + offset, 0.0F, centerZ + extent);
        glVertex3f(centerX - extent, 0.0F, centerZ + offset);
        glVertex3f(centerX + extent, 0.0F, centerZ + offset);
    }
    // World axes, drawn across the visible patch so they stay readable.
    glColor3f(0.9F, 0.25F, 0.25F);
    glVertex3f(centerX - extent, 0.0F, 0.0F);
    glVertex3f(centerX + extent, 0.0F, 0.0F);
    glColor3f(0.3F, 0.9F, 0.3F);
    glVertex3f(0.0F, -extent, 0.0F);
    glVertex3f(0.0F, extent, 0.0F);
    glColor3f(0.3F, 0.5F, 1.0F);
    glVertex3f(0.0F, 0.0F, centerZ - extent);
    glVertex3f(0.0F, 0.0F, centerZ + extent);
    glEnd();
    glEnable(GL_DEPTH_TEST);
}

std::optional<std::size_t> ViewportWindow::pickAt(int x, int y) {
    RECT rect{};
    GetClientRect(window_, &rect);
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    return scene_.pick(camera_, static_cast<float>(x), static_cast<float>(y), width, height);
}

// The gizmo sits on the centroid of the selection, so a multi-selection pivots
// around what the author sees as "the middle" instead of an arbitrary member.
// While a drag is in flight the anchor stays frozen at its grab point, which is
// what keeps the drag mapping (captured at Begin) valid.
math::Vec3f ViewportWindow::gizmoAnchor() const noexcept {
    math::Vec3f sum{};
    std::size_t count = 0;
    for (const auto index : selection_) {
        if (index < scene_.boxes().size()) {
            sum = {sum.x + scene_.boxes()[index].center.x, sum.y + scene_.boxes()[index].center.y,
                   sum.z + scene_.boxes()[index].center.z};
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    return {sum.x / static_cast<float>(count), sum.y / static_cast<float>(count),
            sum.z / static_cast<float>(count)};
}

// A small cone used for the gizmo's arrowheads: 8 segments is plenty at this
// size and legacy GL builds it from raw triangles with no extra state.
void drawGizmoCone(const math::Vec3f& base, const math::Vec3f& direction, float radius,
                   float length) {
    const math::Vec3f tip{base.x + direction.x * length, base.y + direction.y * length,
                          base.z + direction.z * length};
    // Any vector not parallel to the direction serves as the first ring basis.
    math::Vec3f helper{0.0F, 1.0F, 0.0F};
    if (std::abs(math::Vec3f::dot(helper, direction)) > 0.9F) {
        helper = {1.0F, 0.0F, 0.0F};
    }
    const math::Vec3f side = math::Vec3f::cross(direction, helper).normalized();
    const math::Vec3f up = math::Vec3f::cross(side, direction).normalized();

    constexpr int kSegments = 8;
    glBegin(GL_TRIANGLES);
    for (int segment = 0; segment < kSegments; ++segment) {
        const float a0 = static_cast<float>(segment) / kSegments * 6.2831853F;
        const float a1 = static_cast<float>(segment + 1) / kSegments * 6.2831853F;
        const math::Vec3f p0{base.x + (side.x * std::cos(a0) + up.x * std::sin(a0)) * radius,
                             base.y + (side.y * std::cos(a0) + up.y * std::sin(a0)) * radius,
                             base.z + (side.z * std::cos(a0) + up.z * std::sin(a0)) * radius};
        const math::Vec3f p1{base.x + (side.x * std::cos(a1) + up.x * std::sin(a1)) * radius,
                             base.y + (side.y * std::cos(a1) + up.y * std::sin(a1)) * radius,
                             base.z + (side.z * std::cos(a1) + up.z * std::sin(a1)) * radius};
        glVertex3f(p0.x, p0.y, p0.z);
        glVertex3f(p1.x, p1.y, p1.z);
        glVertex3f(tip.x, tip.y, tip.z);
    }
    glEnd();
}

// One unit-cube pass for the gizmo's centre handle (a grab affordance rather
// than a pretty box, so plain GL quads are fine here).
void drawGizmoCube(const math::Vec3f& centre, float half) {
    glBegin(GL_QUADS);
    // +X / -X
    glVertex3f(centre.x + half, centre.y - half, centre.z - half);
    glVertex3f(centre.x + half, centre.y + half, centre.z - half);
    glVertex3f(centre.x + half, centre.y + half, centre.z + half);
    glVertex3f(centre.x + half, centre.y - half, centre.z + half);
    glVertex3f(centre.x - half, centre.y - half, centre.z + half);
    glVertex3f(centre.x - half, centre.y + half, centre.z + half);
    glVertex3f(centre.x - half, centre.y + half, centre.z - half);
    glVertex3f(centre.x - half, centre.y - half, centre.z - half);
    // +Y / -Y
    glVertex3f(centre.x - half, centre.y + half, centre.z - half);
    glVertex3f(centre.x + half, centre.y + half, centre.z - half);
    glVertex3f(centre.x + half, centre.y + half, centre.z + half);
    glVertex3f(centre.x - half, centre.y + half, centre.z + half);
    glVertex3f(centre.x - half, centre.y - half, centre.z + half);
    glVertex3f(centre.x + half, centre.y - half, centre.z + half);
    glVertex3f(centre.x + half, centre.y - half, centre.z - half);
    glVertex3f(centre.x - half, centre.y - half, centre.z - half);
    // +Z / -Z
    glVertex3f(centre.x - half, centre.y - half, centre.z + half);
    glVertex3f(centre.x + half, centre.y - half, centre.z + half);
    glVertex3f(centre.x + half, centre.y + half, centre.z + half);
    glVertex3f(centre.x - half, centre.y + half, centre.z + half);
    glVertex3f(centre.x + half, centre.y - half, centre.z - half);
    glVertex3f(centre.x - half, centre.y - half, centre.z - half);
    glVertex3f(centre.x - half, centre.y + half, centre.z - half);
    glVertex3f(centre.x + half, centre.y + half, centre.z - half);
    glEnd();
}

// The transform gizmo: three axis arrows from the selection centroid plus a
// centre cube. Sized in pixels via gizmoAxisLength so it reads the same at any
// camera distance, and drawn without depth test so it is never swallowed by
// geometry it is standing on. The active/hovered handle brightens to yellow so
// the author always knows which one the next click will grab.
void ViewportWindow::drawGizmo() {
    const math::Vec3f anchor = gizmoAnchor();
    const float axisLength = gizmoAxisLength(camera_, anchor, static_cast<float>(height_));
    if (axisLength <= 0.0F) {
        return;
    }
    // Hover highlight: only worth projecting when the cursor is over us and no
    // drag is running (during a drag the grabbed handle is highlighted anyway).
    GizmoHandle highlighted = GizmoHandle::None;
    if (draggingGizmo_) {
        highlighted = gizmoDrag_.handle;
    } else if (trackingMouse_) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window_, &cursor)) {
            highlighted =
                pickGizmoHandle(camera_, anchor, static_cast<float>(cursor.x),
                                static_cast<float>(cursor.y), static_cast<float>(width_),
                                static_cast<float>(height_));
        }
    }

    glLineWidth(3.0F);
    glBegin(GL_LINES);
    for (const auto handle : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
        const math::Vec3f direction = axisDirection(handle);
        if (handle == highlighted) {
            glColor3f(1.0F, 0.9F, 0.2F);
        } else if (handle == GizmoHandle::AxisX) {
            glColor3f(0.95F, 0.25F, 0.25F);
        } else if (handle == GizmoHandle::AxisY) {
            glColor3f(0.3F, 0.9F, 0.3F);
        } else {
            glColor3f(0.3F, 0.5F, 1.0F);
        }
        glVertex3f(anchor.x, anchor.y, anchor.z);
        glVertex3f(anchor.x + direction.x * axisLength, anchor.y + direction.y * axisLength,
                   anchor.z + direction.z * axisLength);
    }
    glEnd();
    glLineWidth(1.0F);

    // Arrowheads so the direction of each axis reads at a glance.
    constexpr float kConeRadius = 0.045F;
    constexpr float kConeLength = 0.14F;
    for (const auto handle : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
        const math::Vec3f direction = axisDirection(handle);
        if (handle == highlighted) {
            glColor3f(1.0F, 0.9F, 0.2F);
        } else if (handle == GizmoHandle::AxisX) {
            glColor3f(0.95F, 0.25F, 0.25F);
        } else if (handle == GizmoHandle::AxisY) {
            glColor3f(0.3F, 0.9F, 0.3F);
        } else {
            glColor3f(0.3F, 0.5F, 1.0F);
        }
        drawGizmoCone({anchor.x + direction.x * axisLength * 0.86F,
                       anchor.y + direction.y * axisLength * 0.86F,
                       anchor.z + direction.z * axisLength * 0.86F},
                      direction, axisLength * kConeRadius, axisLength * kConeLength);
    }

    // Centre cube: drag it to move the selection freely in the view plane.
    if (highlighted == GizmoHandle::Center) {
        glColor3f(1.0F, 0.9F, 0.2F);
    } else {
        glColor3f(0.85F, 0.85F, 0.9F);
    }
    drawGizmoCube(anchor, axisLength * 0.055F);
}
} // namespace whitehole::render
#endif // _WIN32

