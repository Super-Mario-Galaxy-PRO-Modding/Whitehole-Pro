#pragma once

// BTI (binary texture image) parsing and GX texture decoding.
// Ports the format knowledge of the Java Bti/ImageUtils pair, but decodes
// every supported format to a uniform RGBA8 pixel layout (the Java layer
// kept format-specific intermediates and left palettized formats magenta;
// here indexed formats expand properly through their palette).

#include "whitehole/io/binary_file.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace whitehole::smg {

// GX image formats found inside BTI/BMD TEX1 sections.
enum class BtiFormat : std::uint8_t {
    i4 = 0,
    i8 = 1,
    ia4 = 2,
    ia8 = 3,
    rgb565 = 4,
    rgb5a3 = 5,
    rgba32 = 6,
    c4 = 8,
    c8 = 9,
    c14x = 10,
    cmpr = 14,
};

enum class BtiPaletteFormat : std::uint8_t { ia8 = 0, rgb565 = 1, rgb5a3 = 2 };

struct BtiImage {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> rgba; // width * height * 4, bytes in R, G, B, A order
};

struct Bti {
    std::uint8_t format{0};
    std::uint8_t alphaMode{0};
    std::uint16_t width{0};
    std::uint16_t height{0};
    std::uint8_t wrapS{0};
    std::uint8_t wrapT{0};
    bool usePalette{false};
    std::uint8_t paletteFormat{0};
    std::uint16_t paletteCount{0};
    bool useMipmap{false};
    std::uint8_t maxAnisotropy{0};
    std::uint8_t minFilter{0};
    std::uint8_t magFilter{0};
    float minLod{0.0F};
    float maxLod{0.0F};
    std::uint8_t mipmapCount{0};
    float lodBias{0.0F};

    std::vector<BtiImage> mipmaps; // decoded RGBA8, level 0 first

    [[nodiscard]] const BtiImage& base() const noexcept { return mipmaps.front(); }
};

// Portable GX sampler mapping, re-derived from Java ImageUtils/BmdRenderer
// behaviour (kept header-only-adjacent here so the core stays unit-testable
// without GL headers): wrap 0 = clamp, 2 = mirror, everything else
// (incl. 1) = repeat; filter 0 = nearest, 2/3/4/5 = mipmapped variants,
// else linear. Values are the GL enum integers so the viewport can pass them
// straight to glTexParameteri without a second translation table.
struct BtiSamplerInfo {
    int glWrapS{0x2901};
    int glWrapT{0x2901};
    int glMinFilter{0x2601};
    int glMagFilter{0x2601};
};
[[nodiscard]] inline BtiSamplerInfo btiSamplerInfo(const Bti& texture) noexcept {
    BtiSamplerInfo info;
    // Java ImageUtils.getWrapMode: 0 -> CLAMP_TO_EDGE, 2 -> MIRRORED_REPEAT,
    // default (incl. 1) -> REPEAT.
    constexpr int kGlRepeat = 0x2901;
    constexpr int kGlMirroredRepeat = 0x8370;
    constexpr int kGlClampToEdge = 0x812F;
    const auto wrap = [](std::uint8_t mode) {
        switch (mode) {
            case 0: return kGlClampToEdge;
            case 2: return kGlMirroredRepeat;
            default: return kGlRepeat;
        }
    };
    // Java ImageUtils.getFilterMode: 0 -> NEAREST, 2 -> NEAREST_MIPMAP_NEAREST,
    // 3 -> LINEAR_MIPMAP_NEAREST, 4 -> NEAREST_MIPMAP_LINEAR,
    // 5 -> LINEAR_MIPMAP_LINEAR, default (incl. 1) -> LINEAR.
    const auto filter = [](std::uint8_t mode) {
        switch (mode) {
            case 0: return 0x2600; // NEAREST
            case 2: return 0x2700; // NEAREST_MIPMAP_NEAREST
            case 3: return 0x2701; // LINEAR_MIPMAP_NEAREST
            case 4: return 0x2702; // NEAREST_MIPMAP_LINEAR
            case 5: return 0x2703; // LINEAR_MIPMAP_LINEAR
            default: return 0x2601; // LINEAR
        }
    };
    info.glWrapS = wrap(texture.wrapS);
    info.glWrapT = wrap(texture.wrapT);
    info.glMinFilter = filter(texture.minFilter);
    info.glMagFilter = filter(texture.magFilter);
    return info;
}

// Parses a BTI entry that lives inside `tex1Data` at `entryOffset` (TEX1
// section bytes, or a whole standalone .bti file with entryOffset 0).
// Throws std::runtime_error on malformed input.
[[nodiscard]] Bti parseBti(std::span<const std::uint8_t> tex1Data, std::size_t entryOffset, io::Endian endian);

// Decodes raw GX image data (single mip level, no header) to RGBA8.
[[nodiscard]] BtiImage decodeBtiImage(std::span<const std::uint8_t> data, std::size_t offset, int format, int width,
                                      int height, int mipmapLevel, io::Endian endian,
                                      std::span<const std::uint8_t> paletteData = {}, int paletteFormat = 0);

} // namespace whitehole::smg
