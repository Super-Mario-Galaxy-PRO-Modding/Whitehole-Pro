#include "whitehole/smg/bti.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace whitehole::smg {
namespace {

std::uint8_t expand4(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 4) | value);
}

std::uint8_t expand5(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint8_t expand6(std::uint8_t value) {
    return static_cast<std::uint8_t>((value << 2) | (value >> 4));
}

// GX RGB5A3 word: top bit set -> RGB555 opaque, else ARGB3444.
std::array<std::uint8_t, 4> decodeRgb5a3(std::uint16_t word) {
    if ((word & 0x8000U) != 0U) {
        const std::uint8_t r = expand5(static_cast<std::uint8_t>((word >> 10) & 0x1FU));
        const std::uint8_t g = expand5(static_cast<std::uint8_t>((word >> 5) & 0x1FU));
        const std::uint8_t b = expand5(static_cast<std::uint8_t>(word & 0x1FU));
        return {r, g, b, 255};
    }
    const std::uint8_t a = static_cast<std::uint8_t>((word >> 12) & 0x7U);
    const std::uint8_t r = expand4(static_cast<std::uint8_t>((word >> 8) & 0xFU));
    const std::uint8_t g = expand4(static_cast<std::uint8_t>((word >> 4) & 0xFU));
    const std::uint8_t b = expand4(static_cast<std::uint8_t>(word & 0xFU));
    // 3-bit alpha -> 8-bit via Java/GX style expansion.
    const std::uint8_t a8 = static_cast<std::uint8_t>((a << 5) | (a << 2) | (a >> 1));
    return {r, g, b, a8};
}

std::array<std::uint8_t, 4> decodeRgb565(std::uint16_t word) {
    const std::uint8_t r = expand5(static_cast<std::uint8_t>((word >> 11) & 0x1FU));
    const std::uint8_t g = expand6(static_cast<std::uint8_t>((word >> 5) & 0x3FU));
    const std::uint8_t b = expand5(static_cast<std::uint8_t>(word & 0x1FU));
    return {r, g, b, 255};
}

// Sequential big/little-endian cursor over a TEX1 blob, bounds-checked.
struct Cursor {
    std::span<const std::uint8_t> data;
    std::size_t pos{0};

    std::uint8_t u8() {
        if (pos >= data.size()) {
            throw std::runtime_error("BTI image data overread");
        }
        return data[pos++];
    }

    std::uint16_t u16(io::Endian endian) {
        const std::uint16_t first = u8();
        const std::uint16_t second = u8();
        if (endian == io::Endian::big) {
            return static_cast<std::uint16_t>((first << 8) | second);
        }
        return static_cast<std::uint16_t>((second << 8) | first);
    }

    std::uint32_t u32(io::Endian endian) {
        std::uint32_t value = 0;
        if (endian == io::Endian::big) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                value |= static_cast<std::uint32_t>(u8()) << shift;
            }
        } else {
            for (int shift = 0; shift <= 24; shift += 8) {
                value |= static_cast<std::uint32_t>(u8()) << shift;
            }
        }
        return value;
    }

    void seek(std::size_t position) {
        if (position > data.size()) {
            throw std::runtime_error("BTI image data seek out of range");
        }
        pos = position;
    }
};


void writePixel(BtiImage& image, std::uint32_t x, std::uint32_t y, const std::array<std::uint8_t, 4>& pixel) {
    if (x >= image.width || y >= image.height) {
        return;
    }
    auto* out = image.rgba.data() + ((static_cast<std::size_t>(y) * image.width) + x) * 4U;
    out[0] = pixel[0];
    out[1] = pixel[1];
    out[2] = pixel[2];
    out[3] = pixel[3];
}

std::size_t blockBytes(int format, int width, int height) {
    const auto blocksW = [](int w, int step) { return (w + step - 1) / step; };
    const auto blocksH = [](int h, int step) { return (h + step - 1) / step; };
    switch (format) {
    case 0: return static_cast<std::size_t>(blocksW(width, 8) * blocksH(height, 8)) * 32U;  // I4
    case 1: return static_cast<std::size_t>(blocksW(width, 8) * blocksH(height, 4)) * 32U;  // I8
    case 2: return static_cast<std::size_t>(blocksW(width, 8) * blocksH(height, 4)) * 32U;  // IA4
    case 3:
    case 4:
    case 5:
    case 10: return static_cast<std::size_t>(blocksW(width, 4) * blocksH(height, 4)) * 32U; // IA8, RGB565, RGB5A3, C14X
    case 6: return static_cast<std::size_t>(blocksW(width, 4) * blocksH(height, 4)) * 64U;  // RGBA32
    case 8: return static_cast<std::size_t>(blocksW(width, 8) * blocksH(height, 4)) * 16U;  // C4
    case 9: return static_cast<std::size_t>(blocksW(width, 8) * blocksH(height, 4)) * 32U;  // C8
    case 14: return static_cast<std::size_t>(blocksW(width, 8) * blocksH(height, 8)) * 32U; // CMPR
    default: return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    }
}



using Pixel = std::array<std::uint8_t, 4>;

Pixel grayPixel(std::uint8_t intensity, std::uint8_t alpha = 255) {
    return {intensity, intensity, intensity, alpha};
}

Pixel magentaPixel() {
    return {255, 0, 255, 255};
}

std::vector<Pixel> loadPalette(std::span<const std::uint8_t> data, int paletteFormat, int count, io::Endian endian) {
    std::vector<Pixel> colors;
    if (count <= 0) {
        return colors;
    }
    colors.reserve(static_cast<std::size_t>(count));
    Cursor cursor{data, 0};
    for (int index = 0; index < count; ++index) {
        const auto word = cursor.u16(endian);
        switch (paletteFormat) {
        case 0: { // IA8: first byte alpha, second intensity
            const auto intensity = static_cast<std::uint8_t>(word & 0xFFU);
            const auto alpha = static_cast<std::uint8_t>((word >> 8) & 0xFFU);
            colors.push_back(grayPixel(intensity, alpha));
            break;
        }
        case 1:
            colors.push_back(decodeRgb565(word));
            break;
        case 2:
            colors.push_back(decodeRgb5a3(word));
            break;
        default:
            colors.push_back(magentaPixel());
            break;
        }
    }
    return colors;
}

Pixel paletteLookup(const std::vector<Pixel>& palette, int index) {
    if (palette.empty()) {
        return magentaPixel();
    }
    if (index < 0 || index >= static_cast<int>(palette.size())) {
        return palette.back();
    }
    return palette[static_cast<std::size_t>(index)];
}



BtiImage decodeImage(Cursor cursor, int format, int width, int height, io::Endian endian,
                     const std::vector<Pixel>& palette) {
    BtiImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);
    if (width <= 0 || height <= 0) {
        return image;
    }

    switch (format) {
    case 0: { // I4: 8x8 blocks, two 4-bit intensities per byte (Java: first pixel = high nibble)
        for (int by = 0; by < height; by += 8) {
            for (int bx = 0; bx < width; bx += 8) {
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; x += 2) {
                        const auto byte = cursor.u8();
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   grayPixel(expand4(static_cast<std::uint8_t>(byte >> 4))));
                        writePixel(image, static_cast<std::uint32_t>(bx + x + 1), static_cast<std::uint32_t>(by + y),
                                   grayPixel(expand4(static_cast<std::uint8_t>(byte & 0x0F))));
                    }
                }
            }
        }
        break;
    }

    case 1: { // I8: 8x4 blocks
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 8) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   grayPixel(cursor.u8()));
                    }
                }
            }
        }
        break;
    }

    case 2: { // IA4: 8x4 blocks, low nibble intensity, high nibble alpha
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 8) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        const auto byte = cursor.u8();
                        const auto intensity = expand4(static_cast<std::uint8_t>(byte & 0x0F));
                        const auto alpha = expand4(static_cast<std::uint8_t>(byte >> 4));
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   grayPixel(intensity, alpha));
                    }
                }
            }
        }
        break;
    }

    case 3: { // IA8: 4x4 blocks, first byte alpha, second intensity
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 4) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const auto alpha = cursor.u8();
                        const auto intensity = cursor.u8();
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   grayPixel(intensity, alpha));
                    }
                }
            }
        }
        break;
    }

    case 4:   // RGB565: 4x4 blocks
    case 5: { // RGB5A3: 4x4 blocks
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 4) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const auto word = cursor.u16(endian);
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   format == 4 ? decodeRgb565(word) : decodeRgb5a3(word));
                    }
                }
            }
        }
        break;
    }



    case 6: { // RGBA32: 4x4 blocks, plane 1 = (alpha, red) pairs, plane 2 = (green, blue) pairs
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 4) {
                std::array<std::uint8_t, 64> plane{};

                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        plane[(static_cast<std::size_t>(y) * 4U + static_cast<std::size_t>(x)) * 4U] = cursor.u8();
                        plane[(static_cast<std::size_t>(y) * 4U + static_cast<std::size_t>(x)) * 4U + 1U] = cursor.u8();
                    }
                }
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        plane[(static_cast<std::size_t>(y) * 4U + static_cast<std::size_t>(x)) * 4U + 2U] = cursor.u8();
                        plane[(static_cast<std::size_t>(y) * 4U + static_cast<std::size_t>(x)) * 4U + 3U] = cursor.u8();
                    }
                }
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const auto base = (static_cast<std::size_t>(y) * 4U + static_cast<std::size_t>(x)) * 4U;
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   {plane[base + 1], plane[base + 2], plane[base + 3], plane[base]});
                    }
                }
            }
        }
        break;
    }

    case 8: { // C4: 8x4 blocks, two 4-bit indices per byte (high nibble first)
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 8) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 8; x += 2) {
                        const auto byte = cursor.u8();
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   paletteLookup(palette, static_cast<int>(byte >> 4)));
                        writePixel(image, static_cast<std::uint32_t>(bx + x + 1), static_cast<std::uint32_t>(by + y),
                                   paletteLookup(palette, static_cast<int>(byte & 0x0F)));
                    }
                }
            }
        }
        break;
    }

    case 9: { // C8: 8x4 blocks, one 8-bit index per pixel
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 8) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   paletteLookup(palette, cursor.u8()));
                    }
                }
            }
        }
        break;
    }



    case 10: { // C14X: 4x4 blocks, 16-bit indices
        for (int by = 0; by < height; by += 4) {
            for (int bx = 0; bx < width; bx += 4) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const auto word = cursor.u16(endian);
                        writePixel(image, static_cast<std::uint32_t>(bx + x), static_cast<std::uint32_t>(by + y),
                                   paletteLookup(palette, static_cast<int>(word)));
                    }
                }
            }
        }
        break;
    }

    case 14: { // CMPR: 8x8 macro = four 4x4 DXT1-style sub-blocks
        for (int by = 0; by < height; by += 8) {
            for (int bx = 0; bx < width; bx += 8) {
                for (int sby = 0; sby < 8; sby += 4) {
                    for (int sbx = 0; sbx < 8; sbx += 4) {
                        const auto colorA = cursor.u16(endian);
                        const auto colorB = cursor.u16(endian);
                        std::uint32_t indices = 0;
                        for (int shift = 24; shift >= 0; shift -= 8) {
                            indices |= static_cast<std::uint32_t>(cursor.u8()) << shift;
                        }

                        const auto r1 = expand5(static_cast<std::uint8_t>((colorA >> 11) & 0x1FU));
                        const auto g1 = expand6(static_cast<std::uint8_t>((colorA >> 5) & 0x3FU));
                        const auto b1 = expand5(static_cast<std::uint8_t>(colorA & 0x1FU));
                        const auto r2 = expand5(static_cast<std::uint8_t>((colorB >> 11) & 0x1FU));
                        const auto g2 = expand6(static_cast<std::uint8_t>((colorB >> 5) & 0x3FU));
                        const auto b2 = expand5(static_cast<std::uint8_t>(colorB & 0x1FU));
                        const Pixel subPalette[4] = {
                            {r1, g1, b1, 255},
                            {r2, g2, b2, 255},
                            colorA > colorB
                                ? Pixel{static_cast<std::uint8_t>((2U * r1 + r2) / 3U),
                                        static_cast<std::uint8_t>((2U * g1 + g2) / 3U),
                                        static_cast<std::uint8_t>((2U * b1 + b2) / 3U), 255}
                                : Pixel{static_cast<std::uint8_t>((r1 + r2) / 2U),
                                        static_cast<std::uint8_t>((g1 + g2) / 2U),
                                        static_cast<std::uint8_t>((b1 + b2) / 2U), 255},
                            colorA > colorB
                                ? Pixel{static_cast<std::uint8_t>((r1 + 2U * r2) / 3U),
                                        static_cast<std::uint8_t>((g1 + 2U * g2) / 3U),
                                        static_cast<std::uint8_t>((b1 + 2U * b2) / 3U), 255}
                                : Pixel{r2, g2, b2, 0},
                        };

                        for (int y = 0; y < 4; ++y) {
                            for (int x = 0; x < 4; ++x) {
                                const auto color = subPalette[(indices >> 30) & 0x3U];
                                indices <<= 2;
                                writePixel(image, static_cast<std::uint32_t>(bx + sbx + x),
                                           static_cast<std::uint32_t>(by + sby + y), color);
                            }
                        }
                    }
                }
            }
        }
        break;
    }

    default: { // Unknown format: solid magenta placeholder, matching the Java renderer.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                writePixel(image, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), magentaPixel());
            }
        }
        break;
    }
    } // switch

    return image;
}

} // namespace

BtiImage decodeBtiImage(std::span<const std::uint8_t> data, std::size_t offset, int format, int width, int height,
                        int mipmapLevel, io::Endian endian, std::span<const std::uint8_t> paletteData,
                        int paletteFormat) {
    (void)mipmapLevel; // width/height already describe the requested mip level
    Cursor cursor{data, offset};
    std::vector<Pixel> palette;
    if (!paletteData.empty()) {
        palette = loadPalette(paletteData, paletteFormat, static_cast<int>(paletteData.size() / 2), endian);
    }
    return decodeImage(cursor, format, width, height, endian, palette);
}

Bti parseBti(std::span<const std::uint8_t> tex1Data, std::size_t entryOffset, io::Endian endian) {
    constexpr std::size_t kHeaderSize = 32;
    if (entryOffset > tex1Data.size() || tex1Data.size() - entryOffset < kHeaderSize) {
        throw std::runtime_error("BTI header is truncated");
    }

    Cursor cursor{tex1Data, entryOffset};
    Bti texture;
    texture.format = cursor.u8();
    texture.alphaMode = cursor.u8();
    texture.width = cursor.u16(endian);
    texture.height = cursor.u16(endian);
    texture.wrapS = cursor.u8();
    texture.wrapT = cursor.u8();
    texture.usePalette = cursor.u8() != 0;
    texture.paletteFormat = cursor.u8();
    texture.paletteCount = cursor.u16(endian);
    const auto paletteOffset = cursor.u32(endian);
    texture.useMipmap = cursor.u8() != 0;
    cursor.u8(); // enableEdgeLod
    cursor.u8(); // clampLodBias
    texture.maxAnisotropy = cursor.u8();
    texture.minFilter = cursor.u8();
    texture.magFilter = cursor.u8();
    texture.minLod = static_cast<float>(cursor.u8()) * 0.125F;
    texture.maxLod = static_cast<float>(cursor.u8()) * 0.125F;
    texture.mipmapCount = cursor.u8();
    cursor.u8(); // padding
    texture.lodBias = static_cast<float>(cursor.u16(endian)) * 0.01F;
    const auto imageOffset = cursor.u32(endian);

    const std::size_t paletteBase = entryOffset + paletteOffset;
    const std::size_t imageBase = entryOffset + imageOffset;

    std::vector<Pixel> palette;
    const bool palettized = texture.format == 8 || texture.format == 9 || texture.format == 10;
    if (palettized && texture.paletteCount > 0 &&
        paletteBase + static_cast<std::size_t>(texture.paletteCount) * 2U <= tex1Data.size()) {
        palette = loadPalette(tex1Data.subspan(paletteBase), texture.paletteFormat, texture.paletteCount, endian);
    }

    const int levelCount = texture.mipmapCount == 0 ? 1 : texture.mipmapCount;
    std::size_t cursorPosition = imageBase;
    int mipWidth = static_cast<int>(texture.width);
    int mipHeight = static_cast<int>(texture.height);
    for (int level = 0; level < levelCount && mipWidth > 0 && mipHeight > 0; ++level) {
        const std::size_t consumed = blockBytes(texture.format, mipWidth, mipHeight);
        if (cursorPosition > tex1Data.size() || tex1Data.size() - cursorPosition < consumed) {
            break; // truncated image data: keep the levels decoded so far
        }
        texture.mipmaps.push_back(
            decodeImage(Cursor{tex1Data, cursorPosition}, texture.format, mipWidth, mipHeight, endian, palette));
        cursorPosition += consumed;
        mipWidth /= 2;
        mipHeight /= 2;
    }
    if (texture.mipmaps.empty()) {
        BtiImage empty;
        empty.width = texture.width;
        empty.height = texture.height;
        texture.mipmaps.push_back(std::move(empty));
    }

    return texture;
}

} // namespace whitehole::smg

