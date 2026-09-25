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

// GX wrap/filter mapping lives in smg::btiSamplerInfo (Java ImageUtils
// parity, unit-tested in core_tests); the uploader below uses it directly.

// Material's primary TEV texture (Java Bmd parity: first live stage whose
// texmap resolves to a real TEX1 entry), or -1 when the material is
// untextured. Replaces the old "first used map" heuristic that bound the
// wrong layer on multi-texture materials.
int materialTextureSlot(const smg::BmdMaterial& material) noexcept {
    return material.primaryTextureSlot();
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
// GX compare enums line up with the GL ones for the alpha test: an alpha
// comparison is never affected by the reversed-Z projection (alpha is a
// colour value, not a depth), so this table stays the identity mapping.
//
// DEPTH is different, and must go through BmdMaterial::depthFunctionReversedZ
// instead: the viewport projects reversed-Z (near maps to 1, far to 0, buffer
// cleared to 0, frame default GEQUAL) for 24-bit precision at galaxy zoom.
// Under that mapping "nearer" is a LARGER value, so passing a material's raw
// compare straight through (Java parity, valid on its conventional depth
// buffer) made GL_LESS/GL_LEQUAL reject every visible fragment against a
// 0-cleared buffer -- textured models vanished outright, while the flat path
// (which never sets a depth func) kept drawing.
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

using GlActiveTexture = void(APIENTRY*)(unsigned int);

// glActiveTexture is not declared by the legacy <GL/gl.h> header used by the
// MinGW/MSYS2 toolchain. Resolve it through WGL like the other post-1.1
// entry point above; a null result means the context only has the default
// texture unit, which is already the desired unit.
void selectDefaultTextureUnit() noexcept {
    static const GlActiveTexture proc = []() -> GlActiveTexture {
        const auto address = reinterpret_cast<std::intptr_t>(wglGetProcAddress("glActiveTexture"));
        if (address == 0 || address == 1 || address == 2 || address == 3 || address == -1) {
            return nullptr;
        }
        return reinterpret_cast<GlActiveTexture>(address);
    }();
    if (proc != nullptr) {
        proc(0x84C0); // GL_TEXTURE0
    }
}

} // namespace

} // namespace

ModelTextureCache::~ModelTextureCache() = default;

unsigned int ModelTextureCache::textureFor(const std::shared_ptr<const ModelMesh>& mesh,
                                           std::size_t textureIndex, const char* filter) {
    if (!mesh || textureIndex >= mesh->textures.size()) {
        return 0;
    }
    if (!mesh->renderState || !mesh->renderState->isReadyToRender.load(std::memory_order_acquire)) {
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
    // Uploads must target the same unit used by the fixed-function draw path.
    // A caller or another renderer section may leave a multi-texture unit
    // active; without this, the texture is created on the wrong unit and the
    // later draw binds an empty GL_TEXTURE0.
    selectDefaultTextureUnit();
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
    // BTI header wins: `filter` ("nearest") only forces nearest below; the
    // base-level sanity check above stays the upload gate.
    const smg::BtiSamplerInfo sampler = smg::btiSamplerInfo(*source);
    GLint samplerMin = sampler.glMinFilter;
    GLint magFilter = sampler.glMagFilter;
    if (filter != nullptr && std::string_view(filter) == "nearest") {
        samplerMin = GL_NEAREST;
        magFilter = GL_NEAREST;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, samplerMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler.glWrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler.glWrapT);
    glTexParameterf(GL_TEXTURE_2D, 0x813A /* GL_TEXTURE_MIN_LOD */, source->minLod);
    glTexParameterf(GL_TEXTURE_2D, 0x813B /* GL_TEXTURE_MAX_LOD */, source->maxLod);
    glTexParameterf(GL_TEXTURE_2D, 0x8501 /* GL_TEXTURE_LOD_BIAS */, source->lodBias);
    {
        // Java ImageUtils.getAnisotropy: 1 -> 2x, 2 -> 4x, else 1x. Guarded by
        // the driver's max so a bare GL 1.1 context never errors.
        const GLfloat wantAniso =
            source->maxAnisotropy == 1 ? 2.0F : (source->maxAnisotropy == 2 ? 4.0F : 1.0F);
        if (wantAniso > 1.0F) {
            GLfloat driverMax = 1.0F;
            glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &driverMax);
            if (driverMax >= 2.0F) {
                glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */,
                                wantAniso < driverMax ? wantAniso : driverMax);
            }
        }
    }
    // Java BmdRenderer parity: every decoded mip uploads to its own level
    // (mipmapCount levels, 0..N-1) instead of base-only.
    glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */,
                    static_cast<GLint>(source->mipmaps.size() > 0 ? source->mipmaps.size() - 1 : 0));
    for (std::size_t level = 0; level < source->mipmaps.size(); ++level) {
        const auto& mip = source->mipmaps[level];
        if (mip.width == 0 || mip.height == 0 || mip.rgba.empty()) { continue; }
        if (mip.rgba.size() != static_cast<std::size_t>(mip.width) * mip.height * 4U) { continue; }
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8,
                     static_cast<GLsizei>(mip.width), static_cast<GLsizei>(mip.height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, mip.rgba.data());
    }

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

unsigned int ModelTextureCache::missingTexture() {
    if (missingTexture_ != 0) {
        return missingTexture_;
    }
    constexpr unsigned char pixels[] = {
        255, 64, 64, 255,  64, 64, 255, 255,  64, 64, 255, 255, 255, 64, 64, 255,
        64, 255, 64, 255,  64, 64, 255, 255,  64, 64, 255, 255, 255, 64, 64, 255,
    };
    GLint oldAlignment = 4;
    GLint oldBinding = 0;
    selectDefaultTextureUnit();
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldBinding);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &missingTexture_);
    glBindTexture(GL_TEXTURE_2D, missingTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldBinding));
    return missingTexture_;
}

void ModelTextureCache::clear() noexcept {
    // Deletion applies to the active texture unit too; normalize it so every
    // cached GL name is removed from the same unit on which it was uploaded.
    selectDefaultTextureUnit();
    for (auto& entry : entries_) {
        for (const auto name : entry.names) {
            if (name != 0) {
                glDeleteTextures(1, &name);
            }
        }
    }
    entries_.clear();
    if (missingTexture_ != 0) {
        glDeleteTextures(1, &missingTexture_);
        missingTexture_ = 0;
    }
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
        if (name == 0 && material != nullptr) {
            name = textureCache_.missingTexture();
        }
        if (name != boundName) {
            selectDefaultTextureUnit();
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
            // Reversed-Z, so the compare has to be restated: see the note on
            // kDepthFuncs and BmdMaterial::depthFunctionReversedZ.
            const int gxCompare = material != nullptr
                                      ? static_cast<int>(material->depthFunctionReversedZ())
                                      : 6; // GEQUAL == the frame default
            glDepthFunc(tableAt(kDepthFuncs, gxCompare, GL_GEQUAL));
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
    glDepthFunc(GL_GEQUAL);
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
    // Restore the pointer before tearing the window down: a hidden cursor
    // outliving the viewport would follow the author around the whole desktop.
    endFlyLook();
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
    // A rebuild re-indexes boxes by objectIndex (not position), so prune stale
    // members instead of dropping the whole multi-selection.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [&](std::size_t index) { return index >= scene_.boxes().size(); }),
                     selection_.end());
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

void ViewportWindow::setOrbitInverted(bool inverted) noexcept {
    orbitInverted_ = inverted;
    controller_.settings.invertOrbit = inverted;
}

void ViewportWindow::setNavSettings(const NavSettings& settings) {
    const NavPreset preset = settings.preset;
    controller_.settings = settings;
    if (preset != NavPreset::Custom) {
        // The named presets always own their button mapping; only Custom keeps
        // whatever the user configured.
        controller_.settings.mapping = mappingForPreset(preset);
    }
    orbitInverted_ = controller_.settings.invertOrbit;
    invalidate();
}

void ViewportWindow::setCollisionTriangles(std::vector<SnapTriangle> triangles) {
    collisionTriangles_ = std::move(triangles);
    invalidate();
}

SnapScene ViewportWindow::buildSnapScene() const {
    SnapScene snap;
    snap.kcl = collisionTriangles_;
    snap.boxes.reserve(scene_.boxes().size());
    for (const auto& box : scene_.boxes()) {
        SnapBox entry;
        entry.objectIndex = box.objectIndex;
        entry.center = box.center;
        entry.halfExtents = box.halfExtents;
        entry.pickWorld = box.pickWorld;
        snap.boxes.push_back(entry);
    }
    return snap;
}

std::optional<SnapHit> ViewportWindow::snapDownwards(const math::Vec3f& position, float halfHeight,
                                                     std::optional<std::size_t> ignoreIndex,
                                                     float standOff, float maxDistance) const noexcept {
    // Exact KCL triangles win over the coarse oriented-box approximation, so a
    // drop onto a sculpted planet lands flush instead of on its bounding box.
    const SnapScene snap = buildSnapScene();
    return dropToSurface(snap, position, halfHeight, standOff, maxDistance,
                         ignoreIndex.value_or(static_cast<std::size_t>(-1)));
}

float ViewportWindow::halfHeightFor(std::size_t objectIndex) const noexcept {
    if (objectIndex >= scene_.boxes().size()) {
        return 0.0F;
    }
    return scene_.boxes()[objectIndex].halfExtents.y;
}

void ViewportWindow::frameAll() {
    if (scene_.empty()) {
        return;
    }
    startFocusTween(scene_.center(), scene_.frameDistance());
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
    // Ease onto the bounding sphere instead of teleporting: 'F' is pressed
    // constantly while working, and a smooth move keeps the author oriented.
    startFocusTween(center, frameDistanceForRadius(radius));
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
        // handles. Shift remains a precision modifier for that gesture; only a
        // click that misses the gizmo becomes the Shift marquee.
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
        if (!draggingGizmo_ && (wParam & MK_SHIFT) != 0) {
            marqueeActive_ = true;
            marqueeX0_ = marqueeX1_ = lastX_;
            marqueeY0_ = marqueeY1_ = lastY_;
            marqueeAdditive_ = (wParam & (MK_SHIFT | MK_CONTROL)) != 0;
            invalidate();
            return 0;
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
                                            static_cast<float>(lastY_), width, height,
                                            (wParam & MK_SHIFT) != 0);
                onGizmo_(edit);
            }
            return 0;
        }
        if (draggingLeft_) {
            ReleaseCapture();
            draggingLeft_ = false;
            // A marquee drag selects everything in the rubber band.
            if (marqueeActive_) {
                marqueeActive_ = false;
                const int dx = std::abs(marqueeX1_ - marqueeX0_);
                const int dy = std::abs(marqueeY1_ - marqueeY0_);
                if (dx < 4 && dy < 4) {
                    // Treated as a plain click: fall through to the click-pick
                    // path below with the release point.
                    leftMoved_ = false;
                    lastX_ = GET_X_LPARAM(lParam);
                    lastY_ = GET_Y_LPARAM(lParam);
                } else {
                    auto hits = pickRect(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_);
                    if (onSelectRect_) {
                        onSelectRect_(std::move(hits), marqueeAdditive_);
                    }
                    invalidate();
                    return 0;
                }
            }
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
                if (additive) {
                    if (onSelectMany_) {
                        onSelectMany_(picked, true);
                    }
                } else if (onSelect_) {
                    onSelect_(picked);
                } else if (onSelectMany_) {
                    // Compatibility fallback for hosts that only registered the
                    // multi-select callback.
                    onSelectMany_(picked, false);
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
        // RMB is the flycam: full 3D flight + mouselook until it is released.
        beginFlyLook();
        return 0;
    case WM_RBUTTONUP:
        if (draggingRight_) {
            ReleaseCapture();
            draggingRight_ = false;
            endFlyLook();
        }
        return 0;
    case WM_KILLFOCUS:
        // Never leave the cursor hidden or the flycam pinned when focus moves on.
        endFlyLook();
        return 0;
    case WM_CAPTURECHANGED:
        endFlyLook();
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
        if (flyLook_) {
            // FPS mouselook: the cursor is parked on the viewport centre, so each
            // event carries a fresh offset no matter how far the author sweeps,
            // and the gesture never runs out of screen.
            if (x != flyCenter_.x || y != flyCenter_.y) {
                pendingDX_ += static_cast<float>(x - flyCenter_.x);
                pendingDY_ += static_cast<float>(y - flyCenter_.y);
                POINT center = flyCenter_;
                ClientToScreen(window_, &center);
                SetCursorPos(center.x, center.y);
            }
            lastX_ = flyCenter_.x;
            lastY_ = flyCenter_.y;
            return 0;
        }
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
                                            static_cast<float>(y), width, height,
                                            (wParam & MK_SHIFT) != 0);
                onGizmo_(edit);
            }
            invalidate();
        } else if (draggingLeft_ && (wParam & MK_LBUTTON) != 0) {
            if (marqueeActive_) {
                marqueeX1_ = x;
                marqueeY1_ = y;
                leftMoved_ = true;
                invalidate();
            } else {
                if (dx != 0 || dy != 0) {
                    leftMoved_ = true;
                }
                // Queued, not applied: pollInput() is the single place that
                // turns mouse motion into camera motion, so the control presets
                // and their sensitivities govern every gesture.
                pendingDX_ += static_cast<float>(dx);
                pendingDY_ += static_cast<float>(dy);
            }
        } else if (draggingRight_ && (wParam & MK_RBUTTON) != 0) {
            pendingDX_ += static_cast<float>(dx);
            pendingDY_ += static_cast<float>(dy);
        } else if (draggingMiddle_ && (wParam & MK_MBUTTON) != 0) {
            pendingDX_ += static_cast<float>(dx);
            pendingDY_ += static_cast<float>(dy);
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
        // wheels and multi-notch flicks stay smooth instead of losing input.
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
            // While flying, the wheel is the speed control (with an on-screen
            // readout); otherwise it dollies toward the cursor (Shift = fast).
            pendingWheel_ += static_cast<float>(notches);
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
    // PFD_SUPPORT_COMPOSITION tells the DWM this pixel format is safe to
    // composite directly: without it, some drivers/OS versions fall back to
    // sampling a cached bitmap of the window instead of the live GL surface,
    // which is the other well-documented cause of "flickers with stale
    // content" bug reports for GL child windows (distinct from -- and on top
    // of -- the WM_PAINT issue fixed in paint() above).
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SUPPORT_COMPOSITION;
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
    // Vsync stays at the driver default (interval 1, i.e. ON). An earlier
    // version forced interval 0 here to "race" the shell's Present, on the
    // theory that the shell rewrites this child's pixels every frame and the
    // child had to re-patch itself immediately to land in the same
    // compositor sample. That theory doesn't hold: a sibling window's
    // Present() only ever touches its own swapchain, never another HWND's
    // pixels, so there was nothing to race. What was actually happening is
    // covered now by PFD_SUPPORT_COMPOSITION above and by paint() drawing a
    // real frame on every WM_PAINT below -- so forcing vsync off no longer
    // buys anything and only reintroduces plain screen tearing as a new
    // artifact, which is why it has been removed.
    if (wglMakeCurrent(device_, glContext_) == TRUE) {
        // Double buffering only defines WHICH buffer we draw into, not that
        // drawing to the back buffer is the driver's choice. Some drivers pick
        // the front buffer, which lets the compositor sample half-drawn
        // geometry; naming it explicitly removes that class of flicker.
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
        glClearDepth(0.0F);
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

// The GL body of one frame, shared by both paint() (a floor: guarantees any
// OS-requested repaint is satisfied on the spot) and the editor loop's
// renderIfVisible()/renderIfDirty() (continuous redraw while the app is
// idle-pumping messages). The window class is CS_OWNDC, so device_ is the
// window's own persistent DC and this runs straight through with no DC
// juggling regardless of which caller invoked it.
void ViewportWindow::drawFrame(bool pollInputFrame) {
    if (glContext_ != nullptr && device_ != nullptr) {
        wglMakeCurrent(device_, glContext_);
        if (pollInputFrame) {
            pollInput();
        }
        ensureMeshes(meshesFilled_, meshes_);
        applyCameraToGL(width_, height_);
        drawGrid();
        // Simple directional lighting + color material: category colors stay
        // recognizable while per-face normals give the shapes depth.
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glEnable(GL_COLOR_MATERIAL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        selectDefaultTextureUnit();
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
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
        if (marqueeActive_) {
            drawMarquee();
        }
        SwapBuffers(device_);
        wglMakeCurrent(nullptr, nullptr);
    }
    // A finished frame -- whichever path drew it -- leaves nothing outstanding.
    dirty_ = false;
}

void ViewportWindow::paint() {
    PAINTSTRUCT paintInfo{};
    BeginPaint(window_, &paintInfo);
    // WM_PAINT must actually present a frame here, not just validate the
    // update region. The previous version skipped drawing on the assumption
    // that the shell's Present() always rewrites this child's pixels a
    // moment later, so anything drawn here would supposedly be thrown away.
    // That assumption does not hold for a real WS_CHILD window: a sibling's
    // Present() only ever touches its own swapchain/redirection surface, it
    // never repaints another HWND's pixels. Meanwhile Windows delivers
    // WM_PAINT synchronously in situations the external "editor loop" never
    // gets a chance to run in: the nested modal loop that owns the message
    // pump while a floating/dockable panel is being moved or resized
    // (WM_ENTERSIZEMOVE), restoring from minimized, or DWM needing to
    // refill its redirection surface after this window was occluded. In all
    // of those cases the old code left EndPaint validating a region that was
    // never actually redrawn, so the compositor kept showing whatever stale
    // or blank backing-store content it had -- the exact flicker/garbage
    // symptom this window was reported to have. Drawing for real here, with
    // the same GL path the render loop uses, makes every OS-requested
    // repaint self-sufficient; the external loop's continuous redraw on top
    // of this is just extra frames; it is a strict "at least this" floor.
    if (glContext_ != nullptr && device_ != nullptr) {
        drawFrame(false);
    }
    EndPaint(window_, &paintInfo);
}

bool ViewportWindow::renderIfVisible() {
    if (window_ == nullptr || glContext_ == nullptr || device_ == nullptr) {
        return false;
    }
    // A hidden surface must not be drawn into: the contents of a hidden
    // double-buffered GL window are undefined the moment it comes back, and the
    // caller re-invalidates when it is revealed.
    if (!IsWindowVisible(window_)) {
        dirty_ = true;
        return false;
    }

    // A visible GL child is rendered every host tick. `dirty_` is retained
    // only as an invalidation hint for callers; it must not gate the viewport
    // pass or static scenes can appear frozen between compositor updates.
    pollInput();
    drawFrame(false);
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
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glClearDepth(0.0F);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    const float half = ViewportCamera::kFieldOfView * 0.5F;
    // Dynamic clip planes: near tracks the orbit distance, far tracks distance
    // + scene radius, so 24-bit depth stays precise at every zoom instead of
    // z-fighting rails/overlays from a fixed 1..60000 frustum at galaxy range.
    const float nearPlane = camera_.nearPlane();
    const float farPlane = camera_.farPlane(scene_.frameDistance());
    const float depthRange = farPlane - nearPlane;
    // Reversed-Z column-major matrix: near maps to 1, far maps to 0.
    const float f = 1.0F / static_cast<float>(std::tan(static_cast<double>(half)));
    const GLfloat projection[16]{
        f / static_cast<float>(aspect), 0.0F, 0.0F, 0.0F,
        0.0F, f, 0.0F, 0.0F,
        0.0F, 0.0F, -nearPlane / depthRange, -1.0F,
        0.0F, 0.0F, -(nearPlane * farPlane) / depthRange, 0.0F};
    glLoadMatrixf(projection);
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
    if (box.model != nullptr && !box.model->empty() &&
        box.model->renderState &&
        box.model->renderState->isReadyToRender.load(std::memory_order_acquire)) {
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
        legendY += 26.0F;
    }

    // Fly-speed readout: the wheel changes the flycam speed while RMB is held,
    // so the current multiplier is shown until the gesture has been idle a
    // moment. Without this the speed change is invisible and feels like a bug.
    if (hudTimer_ > 0.0F && hudSpeed_ > 0.0F) {
        char line[64];
        std::snprintf(line, sizeof(line), "Fly speed  %.2fx", static_cast<double>(hudSpeed_));
        const float lineWidth = labelFont.measure(line);
        const float fade = std::min(1.0F, hudTimer_ / 0.6F);
        rect(legendX, legendY, lineWidth + padding * 2.0F, 22.0F,
             translucent(palette.panelBg, 0.92F * fade));
        text(legendX + padding, legendY + 2.0F, line,
             app::Rgba{palette.text.r, palette.text.g, palette.text.b, fade});
        legendY += 26.0F;
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

void ViewportWindow::beginFlyLook() {
    if (flyLook_) {
        return;
    }
    flyLook_ = true;
    controller_.resetFlySpeed();
    orbitPivotSet_ = false;
    if (window_ == nullptr) {
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    flyCenter_.x = (rect.right - rect.left) / 2;
    flyCenter_.y = (rect.bottom - rect.top) / 2;
    // Park the cursor on the centre and hide it: mouselook then never runs out
    // of screen, which is the whole difference between a flycam and a drag.
    POINT center = flyCenter_;
    ClientToScreen(window_, &center);
    SetCursorPos(center.x, center.y);
    if (!cursorHidden_) {
        ShowCursor(FALSE);
        cursorHidden_ = true;
    }
    lastX_ = flyCenter_.x;
    lastY_ = flyCenter_.y;
    invalidate();
}

void ViewportWindow::endFlyLook() noexcept {
    if (!flyLook_) {
        return;
    }
    flyLook_ = false;
    if (cursorHidden_) {
        ShowCursor(TRUE); // balanced against the hide in beginFlyLook
        cursorHidden_ = false;
    }
}

void ViewportWindow::applyCameraDelta(const CameraDelta& delta) {
    const bool orbiting = delta.orbitYaw != 0.0F || delta.orbitPitch != 0.0F;
    if (orbiting && !orbitPivotSet_) {
        // Re-anchor once per gesture: an orbit should spin around what the
        // author is working on, not around wherever the view happened to point.
        orbitPivotSet_ = true;
        if (!selection_.empty()) {
            camera_.target = gizmoAnchor();
        }
    }
    bool changed = false;
    if (orbiting) {
        camera_.orbit(delta.orbitYaw, delta.orbitPitch);
        changed = true;
    }
    if (delta.panX != 0.0F || delta.panY != 0.0F) {
        camera_.pan(delta.panX, delta.panY);
        changed = true;
    }
    if (delta.dollyNotches != 0.0F) {
        if (delta.dollyToCursor) {
            camera_.dollyTowardCursor(delta.dollyNotches, static_cast<float>(lastX_),
                                      static_cast<float>(lastY_), static_cast<float>(width_),
                                      static_cast<float>(height_));
        } else {
            camera_.dolly(delta.dollyNotches);
        }
        changed = true;
    }
    if (delta.flyRight != 0.0F || delta.flyUp != 0.0F || delta.flyForward != 0.0F) {
        // flyStep quotes its speed at distance 800, so the same keys move at a
        // sensible rate in a 50-unit room and across a 50000-unit galaxy.
        const float scale = camera_.distance / 800.0F;
        camera_.fly(delta.flyRight * scale, delta.flyUp * scale, delta.flyForward * scale);
        changed = true;
    }
    if (changed) {
        // Any manual camera input takes over from an in-flight focus tween: the
        // author always wins over the animation.
        tween_.cancel();
        invalidate();
    }
}

void ViewportWindow::updateTween(float dt) {
    if (!tween_.active()) {
        return;
    }
    camera_.setPose(tween_.update(dt));
    invalidate();
}

void ViewportWindow::startFocusTween(const math::Vec3f& center, float distance) {
    const CameraPose from = camera_.pose();
    CameraPose to = from;
    to.target = center;
    to.distance = std::clamp(distance, ViewportCamera::kMinDistance, ViewportCamera::kMaxDistance);
    tween_.start(from, to, 0.45F);
    invalidate();
}

void ViewportWindow::pollInput() {
    const auto now = std::chrono::steady_clock::now();
    // Delta time is real elapsed time, clamped so a stall (galaxy load, a
    // debugger break) can never teleport the camera. The tick also resets
    // whenever nothing is happening, so the first key after a pause starts from
    // zero instead of applying the idle time as one jump.
    float dt = std::chrono::duration<float>(now - lastFlyTick_).count();
    lastFlyTick_ = now;
    dt = std::clamp(dt, 0.0F, 0.1F);

    const auto held = [](int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; };
    const bool focused = window_ != nullptr && GetFocus() == window_;

    InputIntent intent;
    intent.dt = dt;
    intent.shift = held(VK_SHIFT);
    intent.ctrl = held(VK_CONTROL);
    intent.alt = held(VK_MENU);
    intent.rmbHeld = draggingRight_;
    // A gizmo drag or a marquee owns the left button: those gestures must never
    // also pan the camera.
    intent.lmbHeld = draggingLeft_ && !draggingGizmo_ && !marqueeActive_;
    intent.mmbHeld = draggingMiddle_;
    intent.mouseDX = pendingDX_;
    intent.mouseDY = pendingDY_;
    intent.wheelNotches = pendingWheel_;
    pendingDX_ = 0.0F;
    pendingDY_ = 0.0F;
    pendingWheel_ = 0.0F;
    intent.width = static_cast<float>(width_);
    intent.height = static_cast<float>(height_);
    intent.cursorX = static_cast<float>(lastX_);
    intent.cursorY = static_cast<float>(lastY_);

    // Keyboard movement needs this child's focus, so typing in a panel can never
    // fly the camera. WASD drives the flycam only while RMB is held (or when
    // free-fly is opted into), which is what leaves W/E/R free to be the gizmo
    // mode keys. Q/E verticals and the arrows belong to the flycam alone: with
    // RMB released the arrows nudge the selection instead.
    const bool moveKeys = flyLook_ || controller_.settings.freeFlyWithoutRmb;
    if (focused) {
        intent.forward = (moveKeys && held('W')) || (flyLook_ && held(VK_UP));
        intent.back = (moveKeys && held('S')) || (flyLook_ && held(VK_DOWN));
        intent.left = (moveKeys && held('A')) || (flyLook_ && held(VK_LEFT));
        intent.right = (moveKeys && held('D')) || (flyLook_ && held(VK_RIGHT));
        intent.up = flyLook_ && (held('E') || held(VK_PRIOR));
        intent.down = flyLook_ && (held('Q') || held(VK_NEXT));
    }

    // The controller is the single authority on what a gesture means, which is
    // what makes the control presets (and the unit tests) possible.
    const CameraDelta delta = resolveCameraDelta(controller_, intent);
    applyCameraDelta(delta);
    if (delta.orbitYaw == 0.0F && delta.orbitPitch == 0.0F) {
        orbitPivotSet_ = false; // the orbit gesture ended; the next one re-anchors
    }
    updateTween(dt);
    if (focused) {
        pollNudge();
    } else if (flyLook_) {
        // Losing the keyboard mid-flight would otherwise leave the cursor hidden
        // and the view pinned to a dead flycam.
        endFlyLook();
    }

    // Fly-speed HUD: shown while flying and for a moment after the last change.
    if (delta.flying) {
        hudSpeed_ = controller_.flyMultiplier;
        hudTimer_ = 1.4F;
    } else if (hudTimer_ > 0.0F) {
        hudTimer_ = std::max(0.0F, hudTimer_ - dt);
    }
}

void ViewportWindow::pollNudge() {
    if (!onNudge_ || selection_.empty() || flyLook_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    constexpr int kKeyCount = 6;
    const int virtualKeys[kKeyCount] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT};
    const NudgeKey nudgeKeys[kKeyCount] = {NudgeKey::Left,  NudgeKey::Right,   NudgeKey::Up,
                                           NudgeKey::Down,  NudgeKey::PageUp, NudgeKey::PageDown};
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    // Shift = fine placement, Ctrl = coarse; the plain step matches the gizmo's
    // translate snap so both feel like the same grid.
    const float step = shift ? std::max(nudgeStep_ * 0.1F, 0.01F)
                             : (ctrl ? nudgeStep_ * 10.0F : nudgeStep_);

    for (int index = 0; index < kKeyCount; ++index) {
        const bool down = (GetAsyncKeyState(virtualKeys[index]) & 0x8000) != 0;
        const bool edge = down && !nudgeWasDown_[index];
        bool fire = edge;
        if (down && !edge && now >= nextNudgeRepeat_) {
            fire = true; // auto-repeat while held, but slow enough to be exact
        }
        nudgeWasDown_[index] = down;
        if (!fire) {
            continue;
        }
        const int axis = nudgeAxis(nudgeKeys[index]);
        const math::Vec3f direction = nudgeDelta(nudgeKeys[index], step);
        const float amount = axis == 0 ? direction.x : (axis == 1 ? direction.y : direction.z);
        // Alt duplicates: the editor clones the selection once, on the press
        // edge, so holding Alt+Left does not leave a trail of copies.
        onNudge_(axis, amount, alt && edge);
        nextNudgeRepeat_ = now + std::chrono::milliseconds(edge ? 380 : 60);
    }
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
    // Forgiving click: exact ray hit first, then the nearest projected centre
    // within ~10 px so tiny/distant objects stay clickable.
    return scene_.pickForgiving(camera_, static_cast<float>(x), static_cast<float>(y), width, height,
                                pickDistance(), 10.0F);
}

std::vector<std::size_t> ViewportWindow::pickRect(int x0, int y0, int x1, int y1) {
    RECT rect{};
    GetClientRect(window_, &rect);
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    return scene_.pickRect(camera_, static_cast<float>(x0), static_cast<float>(y0),
                           static_cast<float>(x1), static_cast<float>(y1), width, height);
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

// One ring around an axis: the rotate affordance. 48 segments read smooth at
// any zoom; a plain line loop needs no lighting or depth state.
void drawGizmoRing(const math::Vec3f& centre, const math::Vec3f& axis, float radius) {
    math::Vec3f helper{0.0F, 1.0F, 0.0F};
    if (std::abs(math::Vec3f::dot(helper, axis)) > 0.9F) {
        helper = {1.0F, 0.0F, 0.0F};
    }
    const math::Vec3f side = math::Vec3f::cross(axis, helper).normalized();
    const math::Vec3f up = math::Vec3f::cross(side, axis).normalized();
    glBegin(GL_LINE_LOOP);
    constexpr int kSegments = 48;
    for (int segment = 0; segment < kSegments; ++segment) {
        const float angle = static_cast<float>(segment) / kSegments * 6.2831853F;
        glVertex3f(centre.x + (side.x * std::cos(angle) + up.x * std::sin(angle)) * radius,
                   centre.y + (side.y * std::cos(angle) + up.y * std::sin(angle)) * radius,
                   centre.z + (side.z * std::cos(angle) + up.z * std::sin(angle)) * radius);
    }
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
        // Translate reads as arrows; rotate dims the shafts so the rings lead;
        // scale draws shorter, thicker-feeling shafts (same lines, brighter ink).
        if (handle == highlighted) {
            glColor3f(1.0F, 0.9F, 0.2F);
        } else if (handle == GizmoHandle::AxisX) {
            glColor3f(0.95F, 0.25F, 0.25F);
        } else if (handle == GizmoHandle::AxisY) {
            glColor3f(0.3F, 0.9F, 0.3F);
        } else {
            glColor3f(0.3F, 0.5F, 1.0F);
        }
        const float shaftScale = gizmoMode_ == GizmoMode::Scale ? 0.72F : 1.0F;
        glVertex3f(anchor.x, anchor.y, anchor.z);
        glVertex3f(anchor.x + direction.x * axisLength * shaftScale,
                   anchor.y + direction.y * axisLength * shaftScale,
                   anchor.z + direction.z * axisLength * shaftScale);
    }
    glEnd();
    glLineWidth(1.0F);

    // Rotate mode: a ring around each axis so "grab to turn" reads instantly.
    if (gizmoMode_ == GizmoMode::Rotate) {
        for (const auto handle : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
            if (handle == highlighted) {
                glColor3f(1.0F, 0.9F, 0.2F);
            } else if (handle == GizmoHandle::AxisX) {
                glColor3f(0.95F, 0.25F, 0.25F);
            } else if (handle == GizmoHandle::AxisY) {
                glColor3f(0.3F, 0.9F, 0.3F);
            } else {
                glColor3f(0.3F, 0.5F, 1.0F);
            }
            drawGizmoRing(anchor, axisDirection(handle), axisLength * 0.85F);
        }
    }

    // Arrowheads so the direction of each axis reads at a glance.
    // Scale mode caps them with cubes ("grab to grow") instead of cones.
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
        const float shaftScale = gizmoMode_ == GizmoMode::Scale ? 0.72F : 1.0F;
        if (gizmoMode_ == GizmoMode::Scale) {
            drawGizmoCube({anchor.x + direction.x * axisLength * shaftScale,
                           anchor.y + direction.y * axisLength * shaftScale,
                           anchor.z + direction.z * axisLength * shaftScale},
                          axisLength * 0.055F);
            continue;
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

// Rubber-band marquee: an XOR-style outline drawn over the finished frame,
// in window pixels with the Y axis flipped to GL coordinates.
void ViewportWindow::drawMarquee() {
    const int x0 = std::min(marqueeX0_, marqueeX1_);
    const int x1 = std::max(marqueeX0_, marqueeX1_);
    const int y0 = std::min(marqueeY0_, marqueeY1_);
    const int y1 = std::max(marqueeY0_, marqueeY1_);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(std::max(width_, 1)), 0.0,
            static_cast<double>(std::max(height_, 1)), -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    const double top = static_cast<double>(std::max(height_, 1) - y0);
    const double bottom = static_cast<double>(std::max(height_, 1) - y1);
    glColor4f(0.4F, 0.7F, 1.0F, 0.15F);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glVertex2d(x0, bottom);
    glVertex2d(x1, bottom);
    glVertex2d(x1, top);
    glVertex2d(x0, top);
    glEnd();
    glDisable(GL_BLEND);
    glColor3f(0.5F, 0.8F, 1.0F);
    glBegin(GL_LINE_LOOP);
    glVertex2d(x0, bottom);
    glVertex2d(x1, bottom);
    glVertex2d(x1, top);
    glVertex2d(x0, top);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}
} // namespace whitehole::render
#endif // _WIN32