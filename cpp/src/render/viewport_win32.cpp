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
    window_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 10,
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
    invalidate();
}

void ViewportWindow::setSelected(std::optional<std::size_t> selected) {
    selected_ = selected;
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

void ViewportWindow::frameAll() {
    if (scene_.empty()) {
        return;
    }
    camera_.frameTarget(scene_.center(), scene_.frameDistance());
    invalidate();
}

void ViewportWindow::frameSelection() {
    if (!selected_.has_value() || *selected_ >= scene_.boxes().size()) {
        frameAll();
        return;
    }
    camera_.frameTarget(scene_.boxes()[*selected_].center, 300.0F);
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
    case WM_LBUTTONDOWN:
        SetFocus(window_);
        SetCapture(window_);
        draggingLeft_ = true;
        leftMoved_ = false;
        lastX_ = GET_X_LPARAM(lParam);
        lastY_ = GET_Y_LPARAM(lParam);
        return 0;
    case WM_LBUTTONUP:
        if (draggingLeft_) {
            ReleaseCapture();
            draggingLeft_ = false;
            if (!leftMoved_ && (wParam & (MK_SHIFT | MK_CONTROL)) == 0) {
                if (onSelect_) {
                    onSelect_(pickAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
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
        if (draggingLeft_ && (wParam & MK_LBUTTON) != 0) {
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
            const bool selected = selected_.has_value() && *selected_ == box.objectIndex;
            const bool hovered = !selected && hover_.has_value() && *hover_ == box.objectIndex;
            drawShape(box, selected, hovered);
        }
        if (selected_.has_value() && *selected_ < scene_.boxes().size()) {
            const auto& box = scene_.boxes()[*selected_];
            glDisable(GL_LIGHTING);
            glDisable(GL_DEPTH_TEST);
            glLineWidth(2.0F);
            glBegin(GL_LINES);
            glColor3f(1, 0.2F, 0.2F);
            glVertex3f(box.center.x - 60, box.center.y, box.center.z);
            glVertex3f(box.center.x + 60, box.center.y, box.center.z);
            glColor3f(0.2F, 1, 0.2F);
            glVertex3f(box.center.x, box.center.y - 60, box.center.z);
            glVertex3f(box.center.x, box.center.y + 60, box.center.z);
            glColor3f(0.3F, 0.5F, 1);
            glVertex3f(box.center.x, box.center.y, box.center.z - 60);
            glVertex3f(box.center.x, box.center.y, box.center.z + 60);
            glEnd();
            glLineWidth(1.0F);
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

bool ViewportWindow::renderIfDirty() {
    if (!dirty_ || window_ == nullptr || glContext_ == nullptr || device_ == nullptr) {
        return false;
    }
    // Nothing to show. Stay dirty so the frame is drawn once it is revealed.
    if (!IsWindowVisible(window_)) {
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

void ViewportWindow::drawOverlay(HDC device) {
    // Legend + labels are GDI drawn on the visible (front) buffer after the
    // swap, so they never flicker with the GL scene.
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
        HBRUSH backBrush = CreateSolidBrush(RGB(16, 20, 28));
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
            SetTextColor(device, RGB(235, 238, 245));
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
            HBRUSH backBrush = CreateSolidBrush(RGB(16, 20, 28));
            FillRect(device, &background, backBrush);
            DeleteObject(backBrush);
            SetTextColor(device, RGB(255, 215, 120));
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
            SetTextColor(device, RGB(10, 12, 18));
            TextOutW(device, left + 1, static_cast<int>(y) + 1, text.c_str(), static_cast<int>(text.size()));
            SetTextColor(device, isSelected ? RGB(255, 220, 130) : RGB(240, 242, 248));
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
} // namespace whitehole::render
#endif // _WIN32
