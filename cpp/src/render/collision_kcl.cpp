#include "whitehole/render/collision_kcl.hpp"

#include "whitehole/io/binary_file.hpp"

#include <cmath>
#include <stdexcept>

namespace whitehole::render {
namespace {

// KCL layout (big endian), reconstructed from the engine's readers:
//   0x00 u32 positionsOffset   -- vec3f array, 12 bytes each
//   0x04 u32 normalsOffset     -- vec3f array (faces + edges share it)
//   0x08 u32 prismsOffset      -- prism array; slot 0 is a dummy, real
//                                 prisms start 0x10 bytes later
//   0x0C u32 octreeOffset      -- closes the prism array (unused here)
//   0x10 ... thickness, octree origin/masks/shifts (irrelevant to geometry)
//   prism: f32 height; u16 position, face, edge0, edge1, edge2, attribute
constexpr std::size_t kHeaderSize = 0x38;
constexpr std::size_t kVec3Size = 12;
constexpr std::size_t kPrismSize = 0x10;
// The engine addresses prisms as `prisms[1 + index]`: one dummy entry sits
// at prismsOffset and real data starts after it (the Java reader's puzzling
// "+0x10" on the stored offset is this same quirk).
constexpr std::size_t kDummyPrisms = kPrismSize;

math::Vec3f readVec(io::BinaryReader& reader) {
    return {reader.readF32(), reader.readF32(), reader.readF32()};
}

} // namespace

bool isKclPath(std::string_view path) noexcept {
    if (path.size() < 4) {
        return false;
    }
    const auto tail = path.substr(path.size() - 4);
    const auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(tail[0]) == '.' && lower(tail[1]) == 'k' && lower(tail[2]) == 'c' &&
        lower(tail[3]) == 'l';
}

std::vector<SnapTriangle> parseKclTriangles(const std::vector<std::uint8_t>& data) {
    if (data.size() < kHeaderSize) {
        throw std::runtime_error("KCL file is too small to hold its header");
    }

    io::BinaryReader reader(data, io::Endian::big);
    const auto positionOffset = static_cast<std::size_t>(reader.readU32());
    const auto normalOffset = static_cast<std::size_t>(reader.readU32());
    const auto prismOffset = static_cast<std::size_t>(reader.readU32());
    const auto octreeOffset = static_cast<std::size_t>(reader.readU32());

    if (positionOffset < kHeaderSize || positionOffset >= normalOffset || normalOffset > prismOffset ||
        prismOffset > data.size()) {
        throw std::runtime_error("KCL header points outside the file");
    }
    const std::size_t prismStart = prismOffset + kDummyPrisms;
    if (prismStart < normalOffset) {
        throw std::runtime_error("KCL prism array overlaps its normal array");
    }

    // The prism array runs from the dummy slot up to the octree; tolerate an
    // implausible octree marker by falling back to the end of the file.
    std::size_t prismEnd = octreeOffset;
    if (prismEnd < prismStart || prismEnd > data.size()) {
        prismEnd = data.size();
    }

    const std::size_t positionCount = (normalOffset - positionOffset) / kVec3Size;
    const std::size_t normalCount = (prismOffset - normalOffset) / kVec3Size;
    const std::size_t prismCount = (prismEnd - prismStart) / kPrismSize;
    if (positionCount == 0 || normalCount == 0) {
        throw std::runtime_error("KCL has no geometry arrays");
    }

    std::vector<math::Vec3f> positions(positionCount);
    reader.seek(positionOffset);
    for (auto& position : positions) {
        position = readVec(reader);
    }

    std::vector<math::Vec3f> normals(normalCount);
    reader.seek(normalOffset);
    for (auto& normal : normals) {
        normal = readVec(reader);
    }

    std::vector<SnapTriangle> triangles;
    triangles.reserve(prismCount);
    reader.seek(prismStart);
    for (std::size_t index = 0; index < prismCount; ++index) {
        const float height = reader.readF32();
        const auto positionIndex = reader.readU16();
        const auto faceIndex = reader.readU16();
        const auto edge0Index = reader.readU16();
        const auto edge1Index = reader.readU16();
        const auto edge2Index = reader.readU16();
        (void)reader.readU16(); // attribute: floor code (.pa index)
        if (positionIndex >= positionCount || faceIndex >= normalCount || edge0Index >= normalCount ||
            edge1Index >= normalCount || edge2Index >= normalCount) {
            continue;
        }

        // Rebuild the three vertices from the wedge definition: the base
        // vertex plus two edge-plane intersections at `height`. The prism is
        // fully determined by the face normal, the three edge normals and
        // the height; vertex order/winding never enters the formula.
        const math::Vec3f& base = positions[positionIndex];
        const math::Vec3f& face = normals[faceIndex];
        const math::Vec3f& edge0 = normals[edge0Index];
        const math::Vec3f& edge1 = normals[edge1Index];
        const math::Vec3f& edge2 = normals[edge2Index];

        const math::Vec3f across = math::Vec3f::cross(face, edge1);
        const math::Vec3f along = math::Vec3f::cross(edge0, face);
        const float acrossScale = math::Vec3f::dot(across, edge2);
        const float alongScale = math::Vec3f::dot(along, edge2);
        if (std::abs(acrossScale) < 1e-12F || std::abs(alongScale) < 1e-12F) {
            continue; // degenerate wedge: parallel edge normals
        }

        SnapTriangle triangle;
        triangle.a = base;
        triangle.b = base + across * (height / acrossScale);
        triangle.c = base + along * (height / alongScale);
        triangle.normal = face;
        if (!std::isfinite(triangle.a.x) || !std::isfinite(triangle.a.y) || !std::isfinite(triangle.a.z) ||
            !std::isfinite(triangle.b.x) || !std::isfinite(triangle.b.y) || !std::isfinite(triangle.b.z) ||
            !std::isfinite(triangle.c.x) || !std::isfinite(triangle.c.y) || !std::isfinite(triangle.c.z)) {
            continue;
        }
        triangles.push_back(triangle);
    }
    return triangles;
}

} // namespace whitehole::render