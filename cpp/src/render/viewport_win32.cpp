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
#include <cstdio>
#include <cstdint>
#include <string_view>
#include <vector>

// MSVC-only; GCC/Clang link OpenGL through the whitehole_win32 CMake target and
// would otherwise warn about the unknown pragma.
#if defined(_MSC_VER)
#pragma comment(lib, "opengl32.lib")
#endif

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

// ---- Label font atlas --------------------------------------------------------
// Glyphs 32..126 rasterized ONCE by GDI into an off-screen bitmap (GDI only
// touches that bitmap, so it cannot flicker) and uploaded as one alpha
// texture. Labels are then textured quads drawn INSIDE the frame before
// SwapBuffers, so every presented frame is one atomic image. The old scheme
// used TextOutW on the front buffer *after* the swap, which races the desktop
// compositor against the shell's Present -- worst with labels enabled. The
// app hosts one viewport child, so the atlas lives at file scope and is
// rebuilt whenever the GL context is (re)created.
class LabelFont {
public:
    bool build(); // GL context must be current
    void destroy() noexcept;
    [[nodiscard]] bool ready() const noexcept { return texture_ != 0; }
    void bind() const; // enable GL_TEXTURE_2D + bind the atlas
    [[nodiscard]] float lineHeight() const noexcept { return cellHeight_; }
    [[nodiscard]] float measure(std::string_view utf8) const;
    // One textured quad per glyph, current GL color; y is the top edge (the
    // ortho pass is y-down like screen space).
    void draw(float x, float y, std::string_view utf8) const;

private:
    struct Glyph {
        float u0{0.0F};
        float v0{0.0F};
        float u1{0.0F};
        float v1{0.0F};
        float advance{0.0F};
    };
    [[nodiscard]] const Glyph& glyphFor(std::uint32_t codepoint) const noexcept;
    static std::uint32_t nextCodepoint(std::string_view text, std::size_t& index) noexcept;

    unsigned int texture_{0};
    float cellHeight_{16.0F};
    std::array<Glyph, 95> glyphs_{}; // ASCII 32..126
};

bool LabelFont::build() {
    if (texture_ != 0) {
        return true;
    }
    constexpr int kFirst = 32;
    constexpr int kLast = 126;
    constexpr int kCount = kLast - kFirst + 1; // 95
    constexpr int kCols = 16;
    constexpr int kRows = (kCount + kCols - 1) / kCols; // 6
    constexpr int kAtlasW = 512; // power of two: plain GL 1.1 texture
    constexpr int kAtlasH = 256;

    HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }

    HDC mem = CreateCompatibleDC(nullptr);
    if (mem == nullptr) {
        return false;
    }
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = kAtlasW;
    bitmapInfo.bmiHeader.biHeight = -kAtlasH; // top-down: row 0 is the top
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        DeleteDC(mem);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(mem, dib);
    PatBlt(mem, 0, 0, kAtlasW, kAtlasH, BLACKNESS);

    // Shrink the point size until the grid provably fits the atlas.
    int pointSize = 9;
    int cellWidth = 0;
    int cellHeightPx = 0;
    HFONT font = nullptr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (font != nullptr) {
            DeleteObject(font);
        }
        font = CreateFontW(-MulDiv(pointSize, dpi > 0 ? dpi : 96, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           // Grayscale AA: ClearType fringes would corrupt the alpha channel.
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (font == nullptr) {
            SelectObject(mem, oldBitmap);
            DeleteObject(dib);
            DeleteDC(mem);
            return false;
        }
        SelectObject(mem, font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(mem, &metrics);
        int maxWidth = 0;
        for (int code = kFirst; code <= kLast; ++code) {
            int width = 0;
            if (GetCharWidth32W(mem, code, code, &width) && width > maxWidth) {
                maxWidth = width;
            }
        }
        cellWidth = maxWidth + 2;
        cellHeightPx = metrics.tmHeight + 2;
        if (cellWidth * kCols <= kAtlasW && cellHeightPx * kRows <= kAtlasH) {
            break;
        }
        --pointSize;
        if (pointSize < 6) {
            break; // absurd DPI: accept slight clipping rather than fail
        }
    }
    cellHeight_ = static_cast<float>(cellHeightPx);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    SetTextAlign(mem, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    for (int index = 0; index < kCount; ++index) {
        const int column = index % kCols;
        const int row = index / kCols;
        const wchar_t character = static_cast<wchar_t>(kFirst + index);
        TextOutW(mem, column * cellWidth, row * cellHeightPx, &character, 1);
        int advance = cellWidth;
        if (!GetCharWidth32W(mem, kFirst + index, kFirst + index, &advance) || advance < 1) {
            advance = cellWidth;
        }
        Glyph glyph;
        glyph.u0 = static_cast<float>(column * cellWidth) / static_cast<float>(kAtlasW);
        glyph.u1 = static_cast<float>(column * cellWidth + advance) / static_cast<float>(kAtlasW);
        glyph.v0 = static_cast<float>(row * cellHeightPx) / static_cast<float>(kAtlasH);
        glyph.v1 = static_cast<float>(row * cellHeightPx + cellHeightPx) / static_cast<float>(kAtlasH);
        glyph.advance = static_cast<float>(advance);
        glyphs_[static_cast<std::size_t>(index)] = glyph;
    }

    // White-on-black ink -> 1 byte per pixel of alpha (red channel).
    std::vector<unsigned char> alpha(static_cast<std::size_t>(kAtlasW) * kAtlasH);
    const auto* base = static_cast<const unsigned char*>(bits);
    for (int y = 0; y < kAtlasH; ++y) {
        const auto* line = base + static_cast<std::size_t>(y) * kAtlasW * 4;
        for (int x = 0; x < kAtlasW; ++x) {
            alpha[static_cast<std::size_t>(y) * kAtlasW + x] = line[x * 4 + 2]; // BGRA -> R
        }
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    GLint previousAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); // crisp at 1:1
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kAtlasW, kAtlasH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
    glBindTexture(GL_TEXTURE_2D, 0);

    SelectObject(mem, oldBitmap);
    DeleteObject(font);
    DeleteObject(dib);
    DeleteDC(mem);

    texture_ = texture;
    return texture_ != 0;
}

void LabelFont::destroy() noexcept {
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
}

void LabelFont::bind() const {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture_);
}

std::uint32_t LabelFont::nextCodepoint(std::string_view text, std::size_t& index) noexcept {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if (lead >= 0xF0U) {
        length = 4;
    } else if (lead >= 0xE0U) {
        length = 3;
    } else if (lead >= 0xC0U) {
        length = 2;
    }
    if (length == 1) {
        ++index;
        return lead;
    }
    if (index + length > text.size()) {
        index = text.size();
        return '?';
    }
    std::uint32_t codepoint = length == 2 ? (lead & 0x1FU) : (length == 3 ? (lead & 0x0FU) : (lead & 0x07U));
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if ((continuation & 0xC0U) != 0x80U) {
            // Malformed: consume what we read (>= 1 byte, so callers always
            // make progress) and substitute '?'.
            index += offset;
            return '?';
        }
        codepoint = (codepoint << 6) | (continuation & 0x3FU);
    }
    index += length;
    return codepoint;
}

const LabelFont::Glyph& LabelFont::glyphFor(std::uint32_t codepoint) const noexcept {
    if (codepoint < 32U || codepoint > 126U) {
        codepoint = '?';
    }
    return glyphs_[codepoint - 32U];
}

float LabelFont::measure(std::string_view utf8) const {
    float width = 0.0F;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::uint32_t codepoint = nextCodepoint(utf8, index);
        if (codepoint < 32U) {
            continue;
        }
        width += glyphFor(codepoint).advance;
    }
    return width;
}

void LabelFont::draw(float x, float y, std::string_view utf8) const {
    if (texture_ == 0) {
        return;
    }
    const float height = cellHeight_;
    glBegin(GL_QUADS);
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::uint32_t codepoint = nextCodepoint(utf8, index);
        if (codepoint < 32U) {
            continue;
        }
        const Glyph& glyph = glyphFor(codepoint);
        glTexCoord2f(glyph.u0, glyph.v0);
        glVertex2f(x, y);
        glTexCoord2f(glyph.u1, glyph.v0);
        glVertex2f(x + glyph.advance, y);
        glTexCoord2f(glyph.u1, glyph.v1);
        glVertex2f(x + glyph.advance, y + height);
        glTexCoord2f(glyph.u0, glyph.v1);
        glVertex2f(x, y + height);
        x += glyph.advance;
    }
    glEnd();
}

LabelFont labelFont; // one viewport child per app; rebuilt with the GL context

namespace {

// The Win32 GL 1.1 header stops at GL_CLAMP; mirrored repeat arrived with
// GL 1.4 / ARB_texture_mirrored_repeat. 0x8370 is the spec value, so this
// fallback is safe whenever the SDK header does not provide it.
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif

// GX wrap mode (from Bti::wrapS/wrapT) to GL. 0 = clamp, 1 = repeat,
// 2 = mirror; anything else clamps, matching Java's conservative mapping.
int glWrapFor(std::uint8_t wrap) noexcept {
    switch (wrap) {
        case 1: return GL_REPEAT;
        case 2: return GL_MIRRORED_REPEAT;
        default: return GL_CLAMP;
    }
}

// Material's first used texture map, or -1 when the material is untextured.
int materialTextureSlot(const smg::BmdMaterial& material) noexcept {
    for (const auto slot : material.textureIndices) {
        if (slot >= 0) {
            return slot;
        }
    }
    return -1;
}

// GX -> GL state tables, transcribed from BmdRenderer.render() so the C++
// draw matches the Java editor. The index guards turn a corrupt MAT3 read
// into the neutral factor instead of an out-of-bounds fetch.
constexpr std::array<int, 10> kBlendSrcFactors{GL_ZERO,      GL_ONE,          GL_ONE,   GL_ZERO,
                                               GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                                               GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                               GL_ONE_MINUS_DST_COLOR};
constexpr std::array<int, 10> kBlendDstFactors{GL_ZERO,      GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                               GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                                               GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                               GL_ONE_MINUS_DST_COLOR};
constexpr std::array<int, 16> kLogicOps{GL_CLEAR,      GL_AND,         GL_AND_REVERSE, GL_COPY,
                                        GL_AND_INVERTED, GL_NOOP,       GL_XOR,         GL_OR,
                                        GL_NOR,        GL_EQUIV,       GL_INVERT,      GL_OR_REVERSE,
                                        GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND,      GL_SET};
constexpr std::array<int, 8> kCompareFuncs{GL_NEVER,  GL_LESS,    GL_EQUAL,   GL_LEQUAL,
                                           GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
// GX compare enums line up with the GL ones for both alpha test and depth.
constexpr std::array<int, 8> kAlphaFuncs = kCompareFuncs;
constexpr std::array<int, 8> kDepthFuncs = kCompareFuncs;
constexpr std::array<int, 3> kCullModes{GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};

template <std::size_t N>
constexpr int tableAt(const std::array<int, N>& table, int index, int fallback) noexcept {
    return index >= 0 && static_cast<std::size_t>(index) < N ? table[static_cast<std::size_t>(index)]
                                                             : fallback;
}

// glBlendEquation arrived in GL 1.4, but opengl32.dll only exports GL 1.1,
// so the entry point has to come from the driver via wglGetProcAddress. The
// WGL spec allows sentinel return values (1/2/3/-1) instead of null for "not
// available"; Java's isFunctionAvailable guard has the same effect -- without
// the function the default add equation stays in force.
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif
using GlBlendEquation = void(APIENTRY*)(unsigned int);
GlBlendEquation blendEquationProc() noexcept {
    static const GlBlendEquation proc = []() -> GlBlendEquation {
        const auto address = reinterpret_cast<std::intptr_t>(wglGetProcAddress("glBlendEquation"));
        if (address == 0 || address == 1 || address == 2 || address == 3 || address == -1) {
            return nullptr;
        }
        return reinterpret_cast<GlBlendEquation>(address);
    }();
    return proc;
}

} // namespace

} // namespace

ModelTextureCache::~ModelTextureCache() = default;

unsigned int ModelTextureCache::textureFor(const std::shared_ptr<const ModelMesh>& mesh,
                                           std::size_t textureIndex, const char* filter) {
    if (!mesh || textureIndex >= mesh->textures.size()) {
        return 0;
    }
    const smg::Bti* source = &mesh->textures[textureIndex];

    // Reuse an existing upload for this mesh, dropping dead entries first.
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto live = it->mesh.lock();
        if (!live) {
            for (const auto name : it->names) {
                if (name != 0) {
                    glDeleteTextures(1, &name);
                }
            }
            it = entries_.erase(it);
            continue;
        }
        if (live == mesh) {
            if (textureIndex < it->names.size() && it->names[textureIndex] != 0) {
                return it->names[textureIndex];
            }
            break;
        }
        ++it;
    }

    if (source->mipmaps.empty()) {
        return 0;
    }
    const auto& image = source->mipmaps.front();
    if (image.width == 0 || image.height == 0 || image.rgba.empty()) {
        return 0;
    }
    const std::size_t want = image.rgba.size();
    if (want != static_cast<std::size_t>(image.width) * image.height * 4U) {
        return 0;
    }

    GLuint name = 0;
    glGenTextures(1, &name);
    if (name == 0) {
        return 0;
    }
    // Save/restore everything the upload touches: the old code only saved the
    // unpack alignment, and a leaked GL_TEXTURE_2D enable would tint every
    // later flat-colored pass.
    GLint previousAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    const GLboolean wasEnabled = glIsEnabled(GL_TEXTURE_2D);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, name);
    const GLint minFilter = (filter != nullptr && std::string_view(filter) == "nearest") ? GL_NEAREST
                                                                                          : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapFor(source->wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapFor(source->wrapT));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.width),
                 static_cast<GLsizei>(image.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());

    glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));
    if (!wasEnabled) {
        glDisable(GL_TEXTURE_2D);
    }

    // Record the upload against this mesh (grow the slot table as needed).
    for (auto& entry : entries_) {
        if (entry.mesh.lock() == mesh) {
            if (entry.names.size() <= textureIndex) {
                entry.names.resize(textureIndex + 1, 0);
            }
            entry.names[textureIndex] = name;
            return name;
        }
    }
    Entry fresh;
    fresh.mesh = mesh;
    fresh.names.assign(mesh->textures.size(), 0);
    if (fresh.names.size() <= textureIndex) {
        fresh.names.resize(textureIndex + 1, 0);
    }
    fresh.names[textureIndex] = name;
    entries_.push_back(std::move(fresh));
    return name;
}

void ModelTextureCache::clear() noexcept {
    for (auto& entry : entries_) {
        for (const auto name : entry.names) {
            if (name != 0) {
                glDeleteTextures(1, &name);
            }
        }
    }
    entries_.clear();
}

void ViewportWindow::drawModelTriangles(const ModelMesh& mesh, bool bakeColors, const char* filter,
                                       ModelTextureCache* textures) {
    // Legacy flat path (display lists + untextured immediate): no texture
    // state touched at all, so the frame is identical with the cache hot/cold.
    // Textured models NEVER go through display lists: a list bakes geometry
    // but cannot bind per-triangle textures (the upload cache keys on the
    // shared_ptr the list does not hold), so they draw immediate below.
    (void)filter;
    (void)textures;
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

void ViewportWindow::drawTexturedModel(const std::shared_ptr<const ModelMesh>& mesh, const char* filter,
                                       bool selected, bool hovered) {
    // Per-material immediate draw (Java BmdRenderer parity): each material run
    // applies its alpha test, blend/logic-op, cull and depth state, binds its
    // texture and modulates it with the triangle colour. With the two-pass
    // split on, only the current modelPass_'s triangles draw and the
    // translucent pass never writes depth; split off = one pass, no blending.
    // selected/hovered swap in the same highlight colours the flat path uses.
    if (!mesh || mesh->triangles.empty()) {
        return;
    }
    const bool split = translucent_;
    const bool wantTranslucent = modelPass_ == ModelPass::Translucent;
    const GLboolean wasTextured = glIsEnabled(GL_TEXTURE_2D);

    // GL state calls are illegal between glBegin/glEnd, so a material change
    // closes the current run, applies the new state, and a new run opens.
    unsigned int boundName = ~0U; // force the first material through an explicit bind
    bool texturing = wasTextured != 0;
    int currentMaterial = -2;
    bool inBatch = false;
    const auto endBatch = [&] {
        if (inBatch) {
            glEnd();
            inBatch = false;
        }
    };
    const auto applyMaterial = [&](int materialIndex) {
        currentMaterial = materialIndex;
        const smg::BmdMaterial* material =
            materialIndex >= 0 && static_cast<std::size_t>(materialIndex) < mesh->materials.size()
                ? &mesh->materials[static_cast<std::size_t>(materialIndex)]
                : nullptr;
        endBatch();

        // Texture: first used map, or texturing off (BmdRenderer parity).
        unsigned int name = 0;
        if (material != nullptr) {
            const int slot = materialTextureSlot(*material);
            if (slot >= 0) {
                name = textureCache_.textureFor(mesh, static_cast<std::size_t>(slot), filter);
            }
        }
        if (name != boundName) {
            if (boundName != 0) {
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            if (name != 0) {
                glBindTexture(GL_TEXTURE_2D, name);
            }
            boundName = name;
        }
        const bool wantTexturing = name != 0;
        if (wantTexturing != texturing) {
            if (wantTexturing) {
                glEnable(GL_TEXTURE_2D);
            } else {
                glDisable(GL_TEXTURE_2D);
            }
            texturing = wantTexturing;
        }

        // Alpha test: BmdRenderer's three-branch decision, verbatim.
        if (material == nullptr || !material->alphaTestEnabled()) {
            glDisable(GL_ALPHA_TEST);
        } else {
            glEnable(GL_ALPHA_TEST);
            const int op = material->alphaOp;
            const int func0 = material->alphaFunc0;
            const int func1 = material->alphaFunc1;
            if (op == 0 && (func0 == 0 || func1 == 0)) {
                glAlphaFunc(GL_NEVER, 0.0F); // AND with NEVER rejects everything
            } else if ((op == 1 && func0 == 0) || (op == 0 && func0 == 7)) {
                glAlphaFunc(tableAt(kAlphaFuncs, func1, GL_ALWAYS),
                            static_cast<float>(material->alphaRef1) / 255.0F);
            } else {
                glAlphaFunc(tableAt(kAlphaFuncs, func0, GL_ALWAYS),
                            static_cast<float>(material->alphaRef0) / 255.0F);
            }
        }

        // Blend / logic op: only while the split is active (single-pass mode
        // draws everything opaque); otherwise Java's case 0/1/3/2 + GX tables.
        if (material == nullptr || !split) {
            glDisable(GL_BLEND);
            glDisable(GL_COLOR_LOGIC_OP);
        } else {
            switch (material->blendMode) {
                case 1:
                case 3:
                    glDisable(GL_COLOR_LOGIC_OP);
                    glEnable(GL_BLEND);
                    if (const GlBlendEquation equation = blendEquationProc()) {
                        equation(material->blendMode == 3 ? GL_FUNC_SUBTRACT : GL_FUNC_ADD);
                    }
                    glBlendFunc(tableAt(kBlendSrcFactors, material->blendSrcFactor, GL_ONE),
                                tableAt(kBlendDstFactors, material->blendDstFactor, GL_ZERO));
                    break;
                case 2:
                    glDisable(GL_BLEND);
                    glEnable(GL_COLOR_LOGIC_OP);
                    glLogicOp(tableAt(kLogicOps, material->blendOp, GL_COPY));
                    break;
                default:
                    glDisable(GL_BLEND);
                    glDisable(GL_COLOR_LOGIC_OP);
                    break;
            }
        }

        // Face culling: 0 off, 1..3 cull front/back/both.
        if (material == nullptr || material->cullingMode == 0) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(tableAt(kCullModes, static_cast<int>(material->cullingMode) - 1, GL_BACK));
        }

        // Depth test/function from ZMode. The write mask follows the material
        // except in the translucent pass, which never writes depth so nearer
        // blended fragments cannot occlude the ones behind them.
        if (material != nullptr && !material->depthTest) {
            glDisable(GL_DEPTH_TEST);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(tableAt(kDepthFuncs, material != nullptr ? material->depthFunction : 1, GL_LESS));
        }
        GLboolean depthMask = material != nullptr && !material->depthWrite ? GL_FALSE : GL_TRUE;
        if (split && wantTranslucent) {
            depthMask = GL_FALSE;
        }
        glDepthMask(depthMask);
    };
    for (const auto& triangle : mesh->triangles) {
        if (split && triangle.translucent != wantTranslucent) {
            continue; // belongs to the other scene-wide pass
        }
        if (triangle.materialIndex != currentMaterial) {
            applyMaterial(triangle.materialIndex);
        }
        if (!inBatch) {
            glBegin(GL_TRIANGLES);
            inBatch = true;
        }
        if (selected) {
            glColor4f(1.0F, 0.85F, 0.20F, 1.0F);
        } else if (hovered) {
            glColor4f(0.55F, 0.80F, 1.0F, 1.0F);
        } else {
            // Modulate: Java multiplies material colour by the texel.
            glColor4f(triangle.color[0], triangle.color[1], triangle.color[2], triangle.color[3]);
        }
        const ModelVertex* vertices[3] = {&triangle.a, &triangle.b, &triangle.c};
        for (const ModelVertex* vertex : vertices) {
            glTexCoord2f(vertex->texCoord[0], vertex->texCoord[1]);
            glNormal3f(vertex->normal.x, vertex->normal.y, vertex->normal.z);
            glVertex3f(vertex->position.x, vertex->position.y, vertex->position.z);
        }
    }
    endBatch();

    // Hand every flag this walk may have raised back to the frame: overlays,
    // the gizmo and the labels draw next, and they assume the plain state the
    // old single-pass renderer left behind.
    if (boundName != 0 && boundName != ~0U) {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (wasTextured) {
        glEnable(GL_TEXTURE_2D);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
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

void ViewportWindow::setTextureFilter(const char* filter) noexcept {
    const std::string_view want = filter != nullptr ? filter : "linear";
    const std::string_view have(textureFilter_);
    if (want == have) {
        return;
    }
    const std::size_t count = std::min(want.size(), sizeof(textureFilter_) - 1);
    std::copy_n(want.data(), count, textureFilter_);
    textureFilter_[count] = '\0';
    // Filter is baked at upload time, so existing uploads must be redone.
    textureCache_.clear();
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
                    static_cast<float>(GET_Y_LPARAM(lParam)), width, height, pickDistance());
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
    case WM_MBUTTONDOWN:
        SetFocus(window_);
        SetCapture(window_);
        draggingMiddle_ = true;
        lastX_ = GET_X_LPARAM(lParam);
        lastY_ = GET_Y_LPARAM(lParam);
        return 0;
    case WM_MBUTTONUP:
        if (draggingMiddle_) {
            ReleaseCapture();
            draggingMiddle_ = false;
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
            const float invert = orbitInverted_ ? -1.0F : 1.0F;
            camera_.orbit(static_cast<float>(dx) * 0.008F * invert,
                          static_cast<float>(dy) * 0.008F * invert);
            invalidate();
        } else if (draggingMiddle_ && (wParam & MK_MBUTTON) != 0) {
            camera_.pan(static_cast<float>(dx), static_cast<float>(dy));
            invalidate();
        } else if (!draggingLeft_ && !draggingRight_ && !draggingMiddle_) {
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
            // Shift = fast zoom, Java's fast-scroll modifier (x3).
            const int scaled = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ? notches * 3 : notches;
            camera_.dolly(static_cast<float>(scaled));
            invalidate();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            frameSelection();
            return 0;
        }
        if (wParam == VK_HOME) {
            frameAll();
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
    // Swap interval 0 (WGL_EXT_swap_control): the shell's D3D Present(1, 0)
    // already paces the app at the monitor refresh, and it rewrites the whole
    // window surface -- including this child's region -- every frame, so the
    // child MUST re-patch its pixels immediately afterwards to make it into
    // the same compositor sample. The driver default (interval 1) made
    // SwapBuffers wait for the NEXT vblank, leaving the present's blank state
    // on screen for a whole refresh and drawing the scene again for the next:
    // the alternating blink, worst with slow (large-galaxy) frames. With the
    // interval at 0 the swap returns as soon as the patch is queued, so the
    // compositor always samples shell + viewport as one complete image. An
    // extension-less driver just keeps the old blocking behaviour.
    if (wglMakeCurrent(device_, glContext_) == TRUE) {
        using SwapIntervalFn = BOOL(WINAPI*)(int);
        const auto swapInterval =
            reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"));
        if (swapInterval != nullptr) {
            swapInterval(0);
        }
        wglMakeCurrent(nullptr, nullptr);
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
        labelFont.destroy(); // atlas texture belongs to this context too
        textureCache_.clear(); // model textures belong to it as well
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(glContext_);
        glContext_ = nullptr;
    }
    if (device_ != nullptr && window_ != nullptr) {
        ReleaseDC(window_, device_);
        device_ = nullptr;
    }
}

// The GL body of one frame. Independent of paint() (which only validates the
// WM_PAINT update region): the editor's loop calls this via renderIfVisible()
// AFTER the shell's Present -- the only moment a child draw survives, since
// the present erases anything drawn before it. The window class is CS_OWNDC,
// so device_ is the window's own persistent DC and this runs straight from
// the render loop with no DC juggling.
void ViewportWindow::drawFrame() {
    if (glContext_ != nullptr && device_ != nullptr) {
        wglMakeCurrent(device_, glContext_);
        pollFlyMovement(); // WASD/arrows while this child has keyboard focus
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
        // GX front faces wind clockwise; Java's editors set this once per frame
        // too (GalaxyEditorForm's glFrontFace(GL_CW)), so per-material culling
        // agrees with the game about which side is which.
        glFrontFace(GL_CW);
        // Non-uniform object scales need normalisation so the fixed-function
        // pipeline renormalises the inverse-transpose-transformed normals
        // (display lists bake the normals, not the transform).
        glEnable(GL_NORMALIZE);
        pruneModelLists();
        // Scene-wide model passes, like GalaxyEditorForm's OPAQUE then
        // TRANSLUCENT renderAllObjects loops: every box lays down its opaque
        // geometry (and all the flat editor shapes) first, then a second loop
        // blends the translucent triangles over the finished depth buffer in
        // box order. Translucency off keeps the legacy single pass.
        const auto drawBoxes = [&](ModelPass pass) {
            for (const auto& box : scene_.boxes()) {
                const bool selected =
                    std::find(selection_.begin(), selection_.end(), box.objectIndex) != selection_.end();
                const bool hovered = !selected && hover_.has_value() && *hover_ == box.objectIndex;
                drawShape(box, selected, hovered, pass);
            }
        };
        drawBoxes(ModelPass::Opaque);
        if (translucent_) {
            drawBoxes(ModelPass::Translucent);
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
        // Legend + name labels INSIDE the frame: one swap = one atomic image.
        drawLabels();
        SwapBuffers(device_);
        wglMakeCurrent(nullptr, nullptr);
    }
    // A finished frame -- whichever path drew it -- leaves nothing outstanding.
    dirty_ = false;
}

void ViewportWindow::paint() {
    PAINTSTRUCT paintInfo{};
    BeginPaint(window_, &paintInfo);
    // WM_PAINT validates the update region and NOTHING else. Every frame is
    // drawn by the editor loop's renderIfVisible() AFTER the shell's D3D
    // Present, because the present rewrites the whole window surface
    // (including this child's region) and would erase anything drawn before
    // it -- so a paint-time draw was pure waste: a second full scene draw and
    // swap per invalidated frame, racing the compositor on top of the real
    // one. dirty_ deliberately stays set until the loop draws, and EndPaint
    // validates so no WM_PAINT storm follows.
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
    // Dynamic clip planes: near tracks the orbit distance, far tracks distance
    // + scene radius, so 24-bit depth stays precise at every zoom instead of
    // z-fighting rails/overlays from a fixed 1..60000 frustum at galaxy range.
    const float nearPlane = camera_.nearPlane();
    const float farPlane = camera_.farPlane(scene_.frameDistance());
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

void ViewportWindow::drawShape(const ViewportBox& box, bool selected, bool hovered, ModelPass pass) {
    // Real game model path. Textured models draw immediate (per-triangle binds
    // from the upload cache) and honour the scene-wide pass split; untextured
    // ones use the cached display lists and draw once, in the opaque loop.
    // Smooth vertex normals + material diffuse colors make the model read like
    // the game's own render.
    if (box.model != nullptr && !box.model->empty()) {
        const bool textured = textured_ && !box.model->materials.empty() && !box.model->textures.empty();
        if (!textured && pass != ModelPass::Opaque) {
            return;
        }
        glPushMatrix();
        glMultMatrixf(box.world.values.data());
        glShadeModel(GL_SMOOTH);
        if (textured) {
            modelPass_ = pass;
            drawTexturedModel(box.model, textureFilter_, selected, hovered);
            glShadeModel(GL_FLAT);
            glPopMatrix();
            return;
        }
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

    if (pass != ModelPass::Opaque) {
        return; // placeholder cubes belong to the opaque loop only
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

void ViewportWindow::drawLabels() {
    // Legend + labels drawn INSIDE the frame as GL quads from the baked font
    // atlas (see LabelFont): one swap = one atomic image, so nothing here can
    // race the desktop compositor the way front-buffer TextOutW did. Colours
    // come from the palette so the overlay matches the workspace theme.
    if (!labelFont.build()) {
        return;
    }
    const app::Palette& palette = app::themePalette(overlayDark_);
    const auto translucent = [](const app::Rgba& color, float alpha) {
        return app::Rgba{color.r, color.g, color.b, alpha};
    };

    // Ortho pass in screen space (y down), over the frame we are about to swap.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(width_), static_cast<double>(height_), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Per-category counts: the map key for the color system.
    constexpr std::size_t kCategoryTotal = 10;
    std::array<int, kCategoryTotal> counts{};
    std::array<std::string, kCategoryTotal> lines{};
    float textWidth = 0.0F;
    int legendLines = 0;
    for (const auto& box : scene_.boxes()) {
        const auto index = static_cast<std::size_t>(box.category);
        if (index < counts.size()) {
            counts[index]++;
        }
    }
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index] == 0) {
            continue;
        }
        legendLines++;
        const auto& style = categoryStyle(static_cast<ObjectCategory>(index));
        char line[96];
        std::snprintf(line, sizeof(line), "%s × %d", style.label, counts[index]);
        lines[index] = line;
        textWidth = std::max(textWidth, labelFont.measure(lines[index]));
    }

    // Rect + text helpers: solid panels/chips, textured glyph runs.
    const auto rect = [](float x, float y, float w, float h, const app::Rgba& color) {
        glDisable(GL_TEXTURE_2D);
        glColor4f(color.r, color.g, color.b, color.a);
        glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x, y + h);
        glEnd();
    };
    const auto text = [](float x, float y, std::string_view content, const app::Rgba& color) {
        labelFont.bind();
        glColor4f(color.r, color.g, color.b, color.a);
        labelFont.draw(x, y, content);
        glDisable(GL_TEXTURE_2D);
    };

    constexpr float chipSize = 10.0F;
    constexpr float lineStep = 18.0F;
    constexpr float padding = 8.0F;
    constexpr float legendX = 10.0F;
    float legendY = 10.0F;
    if (legendLines > 0) {
        const float legendWidth = textWidth + chipSize + 6.0F + padding * 2.0F;
        const float legendHeight = static_cast<float>(legendLines) * lineStep + padding * 2.0F;
        rect(legendX, legendY, legendWidth, legendHeight, translucent(palette.panelBg, 0.92F));

        int line = 0;
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] == 0) {
                continue;
            }
            const auto& style = categoryStyle(static_cast<ObjectCategory>(index));
            const float y = legendY + padding + static_cast<float>(line) * lineStep;
            rect(legendX + padding, y + 3.0F, chipSize, chipSize,
                 app::Rgba{style.color[0], style.color[1], style.color[2], 1.0F});
            text(legendX + padding + chipSize + 6.0F, y, lines[index], palette.text);
            line++;
        }
        legendY += legendHeight + 6.0F;
    }

    // Selected object line directly under the legend.
    if (selected_.has_value() && *selected_ < scene_.boxes().size()) {
        const auto& box = scene_.boxes()[*selected_];
        const auto& style = categoryStyle(box.category);
        const std::string line = box.name + " — " + style.label + " (" + box.kind + ")";
        const float lineWidth = labelFont.measure(line);
        rect(legendX, legendY, lineWidth + padding * 2.0F, 22.0F, translucent(palette.panelBg, 0.92F));
        text(legendX + padding, legendY + 2.0F, line, palette.unsaved);
    }

    // Object name labels. Hovered/selected are always labeled; the View menu
    // toggle labels every object for surveying the scene.
    if (showLabels_ || selected_.has_value() || hover_.has_value()) {
        const app::Rgba shadow{0.03F, 0.04F, 0.055F, 0.85F};
        for (const auto& box : scene_.boxes()) {
            const bool isSelected = selected_.has_value() && *selected_ == box.objectIndex;
            const bool isHovered = hover_.has_value() && *hover_ == box.objectIndex;
            if (!showLabels_ && !isSelected && !isHovered) {
                continue;
            }
            if (box.name.empty()) {
                continue;
            }
            const math::Vec3f labelPoint{box.center.x, box.center.y + box.halfExtents.y + 6.0F, box.center.z};
            float x = 0.0F;
            float y = 0.0F;
            if (!camera_.worldToScreen(labelPoint, static_cast<float>(width_), static_cast<float>(height_), x, y)) {
                continue;
            }
            const float left = x - labelFont.measure(box.name) * 0.5F;
            // Drop shadow keeps labels readable over bright geometry.
            text(left + 1.0F, y + 1.0F, box.name, shadow);
            const app::Rgba ink = isSelected    ? palette.unsaved
                                  : isHovered   ? app::Rgba{0.55F, 0.80F, 1.00F, 1.0F}
                                                : palette.text;
            text(left, y, box.name, ink);
        }
    }

    // One atomic frame: undo what the pass touched, then hand over to the swap.
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
}

void ViewportWindow::pollFlyMovement() {
    const auto now = std::chrono::steady_clock::now();
    // Focus gate: only the viewport's own keyboard focus flies the camera, so
    // typing in a panel can never move it. GetAsyncKeyState reads held state
    // directly, so auto-repeat and focus handoff both behave. The tick resets
    // while unfocused/idle, so the first key after a pause never jumps.
    if (window_ == nullptr || GetFocus() != window_) {
        lastFlyTick_ = now;
        return;
    }
    const auto held = [](int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; };
    const bool forwardKey = held('W') || held(VK_UP);
    const bool backKey = held('S') || held(VK_DOWN);
    const bool leftKey = held('A') || held(VK_LEFT);
    const bool rightKey = held('D') || held(VK_RIGHT);
    const bool upKey = held('E') || held(VK_PRIOR);    // PgUp, Java keyMask parity
    const bool downKey = held('Q') || held(VK_NEXT);   // PgDn, Java keyMask parity
    if (!forwardKey && !backKey && !leftKey && !rightKey && !upKey && !downKey) {
        lastFlyTick_ = now;
        return;
    }
    float dt = std::chrono::duration<float>(now - lastFlyTick_).count();
    lastFlyTick_ = now;
    dt = std::clamp(dt, 0.0F, 0.1F); // a stall must not teleport the camera
    // Speed scales with orbit distance (like pan) so the same keys feel right
    // in a 50-unit room and a 50000-unit galaxy; Shift x3 / Ctrl x0.25 are
    // Java's fast/slow modifiers.
    float speed = camera_.distance * 1.5F * dt;
    if (held(VK_SHIFT)) {
        speed *= 3.0F;
    }
    if (held(VK_CONTROL)) {
        speed *= 0.25F;
    }
    const float rightAmount = (rightKey ? speed : 0.0F) - (leftKey ? speed : 0.0F);
    const float upAmount = (upKey ? speed : 0.0F) - (downKey ? speed : 0.0F);
    const float forwardAmount = (forwardKey ? speed : 0.0F) - (backKey ? speed : 0.0F);
    camera_.fly(rightAmount, upAmount, forwardAmount);
    invalidate();
}

void ViewportWindow::drawGrid() {
    // The patch covers the whole visible ground (see gridSpec: extent >= 2x
    // the orbit distance, step snapped to 1/2/5x10^n) and the outer rings fade
    // to nothing, so the grid dissolves instead of ending in the hard cut
    // ("the grid clips") the old fixed patch drew mid-screen.
    const GridSpec spec = gridSpec(camera_.distance);
    constexpr int halfLines = ViewportCamera::kGridHalfLines;
    const float step = spec.step;
    const float extent = spec.extent;
    const float centerX = std::floor(camera_.target.x / step + 0.5F) * step;
    const float centerZ = std::floor(camera_.target.z / step + 0.5F) * step;
    const float fadeStart = static_cast<float>(halfLines) * 0.55F;
    const float fadeSpan = static_cast<float>(halfLines) - fadeStart;
    const auto ringAlpha = [fadeStart, fadeSpan](int index) {
        const float magnitude = static_cast<float>(index < 0 ? -index : index);
        if (magnitude <= fadeStart) {
            return 1.0F;
        }
        return std::max(0.0F, 1.0F - (magnitude - fadeStart) / fadeSpan);
    };

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_LINES);
    for (int i = -halfLines; i <= halfLines; ++i) {
        const float alpha = ringAlpha(i);
        if (alpha <= 0.02F) {
            continue;
        }
        const float offset = static_cast<float>(i) * step;
        glColor4f(0.22F, 0.26F, 0.33F, alpha);
        glVertex3f(centerX + offset, 0.0F, centerZ - extent);
        glVertex3f(centerX + offset, 0.0F, centerZ + extent);
        glVertex3f(centerX - extent, 0.0F, centerZ + offset);
        glVertex3f(centerX + extent, 0.0F, centerZ + offset);
    }
    // World axes across the patch. The red/green/blue rays run through the
    // world origin; they stay fully opaque (their finite end is a normal axis
    // end, not an artifact).
    glColor4f(0.9F, 0.25F, 0.25F, 1.0F);
    glVertex3f(centerX - extent, 0.0F, 0.0F);
    glVertex3f(centerX + extent, 0.0F, 0.0F);
    glColor4f(0.3F, 0.9F, 0.3F, 1.0F);
    glVertex3f(0.0F, -extent, 0.0F);
    glVertex3f(0.0F, extent, 0.0F);
    glColor4f(0.3F, 0.5F, 1.0F, 1.0F);
    glVertex3f(0.0F, 0.0F, centerZ - extent);
    glVertex3f(0.0F, 0.0F, centerZ + extent);
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

std::optional<std::size_t> ViewportWindow::pickAt(int x, int y) {
    RECT rect{};
    GetClientRect(window_, &rect);
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    return scene_.pick(camera_, static_cast<float>(x), static_cast<float>(y), width, height, pickDistance());
}

float ViewportWindow::pickDistance() const noexcept {
    // Reach scales with the scene, so a galaxy framed past the old hard 20000
    // unit limit stays clickable; the floor keeps open-space misclicks from
    // selecting across an empty map.
    return std::max(scene_.frameDistance() * 2.0F, 20000.0F);
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

