#include "whitehole/smg/bmd.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace whitehole::smg {
namespace {

using io::Endian;
using math::Vec2f;
using math::Vec3f;

// "J3D2" as stored by each byte order. A little-endian file keeps the tag
// bytes in little-endian order, which is what the Java reader checks for.
constexpr std::uint32_t kMagicBigEndian = 0x4A334432U;
constexpr std::uint32_t kMagicLittleEndian = 0x3244334AU;

constexpr std::uint32_t kSectionINF1 = 0x494E4631U;
constexpr std::uint32_t kSectionVTX1 = 0x56545831U;
constexpr std::uint32_t kSectionEVP1 = 0x45565031U;
constexpr std::uint32_t kSectionDRW1 = 0x44525731U;
constexpr std::uint32_t kSectionJNT1 = 0x4A4E5431U;
constexpr std::uint32_t kSectionSHP1 = 0x53485031U;
constexpr std::uint32_t kSectionMAT3 = 0x4D415433U;
constexpr std::uint32_t kSectionMDL3 = 0x4D444C33U;
constexpr std::uint32_t kSectionTEX1 = 0x54455831U;

// Uppercase hex without locale/format-flag noise, for error messages.
std::string hexOffset(std::size_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    if (value == 0) {
        return "0";
    }
    char buffer[20];
    std::size_t length = 0;
    while (value != 0 && length < sizeof(buffer)) {
        buffer[length++] = kDigits[value & 0xFU];
        value >>= 4;
    }
    std::string out;
    out.reserve(length);
    while (length > 0) {
        out.push_back(buffer[--length]);
    }
    return out;
}

// Four printable characters of a section tag, for error messages.
std::string sectionTagName(std::uint32_t tag) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto byte = static_cast<char>((tag >> shift) & 0xFFU);
        out.push_back(byte >= 0x20 && byte < 0x7F ? byte : '?');
    }
    return out;
}

// Bounds-checked random-access reader. Every offset in a J3D file is relative
// to the start of its section, so callers pass absolute positions and the
// reader rejects anything outside the file.
struct Reader {
    std::span<const std::uint8_t> data;
    Endian endian{Endian::big};

    void need(std::size_t position, std::size_t count) const {
        if (position > data.size() || count > data.size() - position) {
            throw std::runtime_error("BMD: read past end of file");
        }
    }

    std::uint8_t u8(std::size_t position) const {
        need(position, 1);
        return data[position];
    }

    std::uint16_t u16(std::size_t position) const {
        need(position, 2);
        const std::uint8_t* bytes = data.data() + position;
        if (endian == Endian::big) {
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
        }
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[1]) << 8) | bytes[0]);
    }

    std::int16_t s16(std::size_t position) const { return static_cast<std::int16_t>(u16(position)); }

    std::uint32_t u32(std::size_t position) const {
        need(position, 4);
        const std::uint8_t* bytes = data.data() + position;
        std::uint32_t value = 0;
        if (endian == Endian::big) {
            for (int index = 0; index < 4; ++index) {
                value = (value << 8) | bytes[index];
            }
        } else {
            for (int index = 3; index >= 0; --index) {
                value = (value << 8) | bytes[index];
            }
        }
        return value;
    }

    float f32(std::size_t position) const {
        const std::uint32_t bits = u32(position);
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // NUL-terminated ASCII name (J3D name tables).
    std::string string(std::size_t position) const {
        std::string out;
        while (position < data.size() && data[position] != 0) {
            out.push_back(static_cast<char>(data[position]));
            ++position;
        }
        return out;
    }
};

// Width of one SHP1 index element. GX index streams really do come in
// u8/u16/s8/s16 flavours (the 0x0300 high byte selects u16, everything else
// reads as a byte), which is what readPrimitiveIndices below switches on.
[[maybe_unused]] int arrayElementBytes(int dataType) {
    switch (dataType) {
        case 0:
        case 1:
            return 1;
        case 2:
        case 3:
            return 2;
        case 4:
            return 4;
        default:
            throw std::runtime_error("BMD: unsupported VTX1 data type " + std::to_string(dataType));
    }
}

float arrayValue(const Reader& reader, std::size_t position, int dataType, int fractionBits) {
    // VTX1 carries only s16 (GX CompType 3) and f32 (4) element data -- Java's
    // readArrayValue returns 0 for every other code, so anything else here is
    // malformed data and fails loudly instead of baking zeros into the mesh.
    if (dataType != 3 && dataType != 4) {
        throw std::runtime_error("BMD: unsupported VTX1 data type " + std::to_string(dataType));
    }
    const auto divisor = static_cast<float>(1 << std::max(fractionBits, 0));
    if (dataType == 4) {
        return reader.f32(position);
    }
    return static_cast<float>(reader.s16(position)) / divisor;
}

std::uint8_t expand4(std::uint8_t value) { return static_cast<std::uint8_t>((value << 4) | value); }
std::uint8_t expand5(std::uint8_t value) { return static_cast<std::uint8_t>((value << 3) | (value >> 2)); }
std::uint8_t expand6(std::uint8_t value) { return static_cast<std::uint8_t>((value << 2) | (value >> 4)); }

// Bytes per vertex color element.
int colorElementBytes(int dataType) {
    switch (dataType) {
        case 0:
        case 3:
            return 2;
        case 1:
        case 2:
        case 4:
        case 5:
            return 4;
        default:
            throw std::runtime_error("BMD: unsupported color data type " + std::to_string(dataType));
    }
}

// Decodes GX color formats to RGBA floats. The Java reader divided raw 5- and
// 6-bit channels by 255, which produced visibly wrong vertex colors; the
// channels are expanded to full range here.
std::array<float, 4> colorValue(const Reader& reader, std::size_t position, int dataType) {
    switch (dataType) {
        case 0: { // RGB565
            const std::uint16_t word = reader.u16(position);
            return {static_cast<float>(expand5(static_cast<std::uint8_t>((word >> 11) & 0x1FU))) / 255.0F,
                    static_cast<float>(expand6(static_cast<std::uint8_t>((word >> 5) & 0x3FU))) / 255.0F,
                    static_cast<float>(expand5(static_cast<std::uint8_t>(word & 0x1FU))) / 255.0F, 1.0F};
        }
        case 1:
        case 2: // RGBX8
            return {static_cast<float>(reader.u8(position)) / 255.0F,
                    static_cast<float>(reader.u8(position + 1)) / 255.0F,
                    static_cast<float>(reader.u8(position + 2)) / 255.0F, 1.0F};
        case 3: { // RGBA4
            const std::uint16_t word = reader.u16(position);
            return {static_cast<float>(expand4(static_cast<std::uint8_t>((word >> 12) & 0xFU))) / 255.0F,
                    static_cast<float>(expand4(static_cast<std::uint8_t>((word >> 8) & 0xFU))) / 255.0F,
                    static_cast<float>(expand4(static_cast<std::uint8_t>((word >> 4) & 0xFU))) / 255.0F,
                    static_cast<float>(expand4(static_cast<std::uint8_t>(word & 0xFU))) / 255.0F};
        }
        case 4: { // RGBA6, stored in the high 24 bits of a word
            const std::uint32_t word = reader.u32(position) >> 8;
            return {static_cast<float>((word >> 18) & 0x3FU) / 63.0F,
                    static_cast<float>((word >> 12) & 0x3FU) / 63.0F,
                    static_cast<float>((word >> 6) & 0x3FU) / 63.0F,
                    static_cast<float>(word & 0x3FU) / 63.0F};
        }
        case 5: // RGBA8
            return {static_cast<float>(reader.u8(position)) / 255.0F,
                    static_cast<float>(reader.u8(position + 1)) / 255.0F,
                    static_cast<float>(reader.u8(position + 2)) / 255.0F,
                    static_cast<float>(reader.u8(position + 3)) / 255.0F};
        default:
            throw std::runtime_error("BMD: unsupported color data type " + std::to_string(dataType));
    }
}

// INF1: scene graph. The node stream is a tiny stack machine (push/pop and
// "draw joint"/"draw shape" opcodes) exactly as the Java reader walks it.
void readINF1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    model.miscFlags = reader.u16(sectionStart + 8);
    model.vertexCount = static_cast<int>(reader.u32(sectionStart + 0x10));
    const auto nodeTableOffset = reader.u32(sectionStart + 0x14);
    if (nodeTableOffset < 0x18 || nodeTableOffset > sectionSize) {
        throw std::runtime_error("BMD: INF1 node table is out of range");
    }

    std::size_t position = sectionStart + nodeTableOffset;
    std::vector<std::int32_t> materialStack{-1};
    std::vector<std::int32_t> nodeStack{-1};

    while (true) {
        const auto opcode = reader.u16(position);
        position += 2;
        if (opcode == 0) {
            break;
        }
        const auto argument = reader.u16(position);
        position += 2;
        switch (opcode) {
            case 0x01: // open child scope
                materialStack.push_back(materialStack.back());
                nodeStack.push_back(static_cast<std::int32_t>(model.sceneGraph.size()) - 1);
                break;
            case 0x02: // close scope
                if (materialStack.size() > 1) materialStack.pop_back();
                if (nodeStack.size() > 1) nodeStack.pop_back();
                break;
            case 0x11: // set current material
                if (!materialStack.empty()) materialStack.pop_back();
                materialStack.push_back(argument);
                break;
            case 0x10:   // draw joint
            case 0x12: { // draw shape
                BmdSceneNode node;
                node.materialIndex = static_cast<std::int16_t>(materialStack.back());
                node.nodeId = static_cast<std::int16_t>(argument);
                node.nodeType = opcode == 0x12 ? 0 : 1;
                node.parentIndex = nodeStack.back();
                model.sceneGraph.push_back(node);
                break;
            }
            default: // unknown opcodes carry no payload beyond their argument
                break;
        }
    }
}

// VTX1: vertex attribute arrays. Attribute data offsets are section-relative;
// the array definition table describes each present array.
void readVTX1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    const auto arrayDefinitionOffset = reader.u32(sectionStart + 8);

    std::vector<std::uint32_t> offsets;
    for (std::size_t index = 0; index < 13; ++index) {
        const auto offset = reader.u32(sectionStart + 0xC + index * 4);
        if (offset != 0) {
            offsets.push_back(offset);
        }
    }

    for (std::size_t index = 0; index < offsets.size(); ++index) {
        if (offsets[index] > sectionSize) {
            throw std::runtime_error("BMD: VTX1 array offset is out of range");
        }
        const std::size_t definition = sectionStart + arrayDefinitionOffset + index * 0x10;
        const auto arrayType = static_cast<int>(reader.u32(definition));
        const auto componentCount = static_cast<int>(reader.u32(definition + 4));
        const auto dataType = static_cast<int>(reader.u32(definition + 8));
        const auto fractionBits = static_cast<int>(reader.u8(definition + 0xC));

        std::size_t byteCount = 0;
        if (index + 1 < offsets.size()) {
            if (offsets[index + 1] < offsets[index]) {
                throw std::runtime_error("BMD: VTX1 array offsets are not ascending");
            }
            byteCount = offsets[index + 1] - offsets[index];
        } else {
            byteCount = sectionSize - offsets[index];
        }
        const std::size_t base = sectionStart + offsets[index];

        if (arrayType == 11 || arrayType == 12) {
            const auto elementBytes = static_cast<std::size_t>(colorElementBytes(dataType));
            const std::size_t count = byteCount / elementBytes;
            auto& colors = model.colors[static_cast<std::size_t>(arrayType - 11)];
            colors.clear();
            colors.reserve(count);
            for (std::size_t element = 0; element < count; ++element) {
                colors.push_back(colorValue(reader, base + element * elementBytes, dataType));
            }
            continue;
        }

        // VTX1 data-type codes are GX component types, so only the width-bearing
        // codes 3 (s16) and 4 (f32) carry elements; Java divides the byte span
        // by that width and rejects everything else.
        const auto elementBytes = static_cast<std::size_t>(dataType == 3 ? 2 : 4);
        const std::size_t elements = dataType == 3 || dataType == 4
                                         ? byteCount / elementBytes
                                         : throw std::runtime_error("BMD: unsupported VTX1 data type " +
                                                                   std::to_string(dataType));

        if (arrayType == 9) { // positions
            model.positions.clear();
            // Java: compsize==0 → arraysize/2 vertices, 2 components each (XY);
            // compsize==1 → arraysize/3 vertices, 3 components each (XYZ).
            // componentsPerVert selects the number of position components per vertex.
            const std::size_t componentsPerVert = (componentCount == 0) ? 2U : 3U;
            const std::size_t count = elements / componentsPerVert;
            model.positions.reserve(count);
            for (std::size_t element = 0; element < count; ++element) {
                const std::size_t stride = componentsPerVert * elementBytes;
                const std::size_t at = base + element * stride;
                if (componentsPerVert == 2) {
                    model.positions.push_back({arrayValue(reader, at, dataType, fractionBits),
                                               arrayValue(reader, at + elementBytes, dataType, fractionBits), 0.0F});
                } else {
                    model.positions.push_back({arrayValue(reader, at, dataType, fractionBits),
                                               arrayValue(reader, at + elementBytes, dataType, fractionBits),
                                               arrayValue(reader, at + 2 * elementBytes, dataType, fractionBits)});
                }
            }
            continue;
        }

        if (arrayType == 10) { // normals; component count 0 means three components
            if (componentCount != 0) {
                throw std::runtime_error("BMD: unsupported normal component count " + std::to_string(componentCount));
            }
            const std::size_t count = elements / 3;
            model.normals.clear();
            model.normals.reserve(count);
            for (std::size_t element = 0; element < count; ++element) {
                const std::size_t at = base + element * 3 * elementBytes;
                model.normals.push_back({arrayValue(reader, at, dataType, fractionBits),
                                         arrayValue(reader, at + elementBytes, dataType, fractionBits),
                                         arrayValue(reader, at + 2 * elementBytes, dataType, fractionBits)});
            }
            continue;
        }

        if (arrayType >= 13 && arrayType <= 20) { // texcoords
            auto& texcoords = model.texcoords[static_cast<std::size_t>(arrayType - 13)];
            texcoords.clear();
            if (componentCount == 0) {
                texcoords.reserve(elements);
                for (std::size_t element = 0; element < elements; ++element) {
                    texcoords.push_back(
                        {arrayValue(reader, base + element * elementBytes, dataType, fractionBits), 0.0F});
                }
            } else if (componentCount == 1) {
                const std::size_t count = elements / 2;
                texcoords.reserve(count);
                for (std::size_t element = 0; element < count; ++element) {
                    const std::size_t at = base + element * 2 * elementBytes;
                    texcoords.push_back({arrayValue(reader, at, dataType, fractionBits),
                                         arrayValue(reader, at + elementBytes, dataType, fractionBits)});
                }
            } else {
                throw std::runtime_error("BMD: unsupported texcoord component count " +
                                         std::to_string(componentCount));
            }
            continue;
        }

        throw std::runtime_error("BMD: unsupported VTX1 array type " + std::to_string(arrayType));
    }
}

// EVP1: envelope (skin) definitions. Each entry lists the joints that drive a
// draw matrix plus their weights; the inverse bind matrices at the last offset
// are only needed for skinning, which this static reader does not do.
void readEVP1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    const auto count = static_cast<std::size_t>(reader.u16(sectionStart + 8));
    const auto subCountOffset = reader.u32(sectionStart + 0xC);
    const auto jointIndexOffset = reader.u32(sectionStart + 0x10);
    const auto weightOffset = reader.u32(sectionStart + 0x14);
    const auto inverseBindOffset = reader.u32(sectionStart + 0x18);
    (void)sectionSize;
    (void)inverseBindOffset;

    model.envelopeJoints.assign(count, {});
    model.envelopeWeights.assign(count, {});

    std::size_t jointCursor = 0;
    std::size_t weightCursor = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto subCount = static_cast<std::size_t>(reader.u8(sectionStart + subCountOffset + index));
        auto& joints = model.envelopeJoints[index];
        auto& weights = model.envelopeWeights[index];
        joints.reserve(subCount);
        weights.reserve(subCount);
        for (std::size_t sub = 0; sub < subCount; ++sub) {
            joints.push_back(reader.u16(sectionStart + jointIndexOffset + jointCursor));
            jointCursor += 2;
            weights.push_back(reader.f32(sectionStart + weightOffset + weightCursor));
            weightCursor += 4;
        }
    }
}

// DRW1: which draw matrix each SHP1 entry uses, and whether that matrix is a
// single joint or a weighted envelope.
void readDRW1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    const auto count = static_cast<std::size_t>(reader.u16(sectionStart + 8));
    const auto weightedOffset = reader.u32(sectionStart + 0xC);
    const auto indexOffset = reader.u32(sectionStart + 0x10);
    (void)sectionSize;

    model.matrixWeighted.assign(count, false);
    model.matrixIndices.assign(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        model.matrixWeighted[index] = reader.u8(sectionStart + weightedOffset + index) != 0;
        model.matrixIndices[index] = reader.u16(sectionStart + indexOffset + index * 2);
    }
}

// JNT1: joints. Records are indirected through a remap table and names through
// a string table, exactly like the Java reader.
void readJNT1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    const auto count = static_cast<std::size_t>(reader.u16(sectionStart + 8));
    const auto jointOffset = reader.u32(sectionStart + 0xC);
    const auto remapOffset = reader.u32(sectionStart + 0x10);
    const auto nameOffset = reader.u32(sectionStart + 0x14);
    (void)sectionSize;

    constexpr float kRadiansPerUnit = 3.1415926535897932F / 32768.0F;

    model.joints.assign(count, {});
    for (std::size_t index = 0; index < count; ++index) {
        const auto remap = static_cast<std::size_t>(reader.u16(sectionStart + remapOffset + index * 2));
        const std::size_t record = sectionStart + jointOffset + remap * 0x40;
        BmdJoint& joint = model.joints[index];
        joint.matrixFlags = reader.u16(record);
        joint.doNotInheritParentScale = reader.u8(record + 2);
        joint.scale = {reader.f32(record + 4), reader.f32(record + 8), reader.f32(record + 12)};
        joint.rotationRadians = {static_cast<float>(reader.s16(record + 16)) * kRadiansPerUnit,
                                 static_cast<float>(reader.s16(record + 18)) * kRadiansPerUnit,
                                 static_cast<float>(reader.s16(record + 20)) * kRadiansPerUnit};
        joint.translation = {reader.f32(record + 24), reader.f32(record + 28), reader.f32(record + 32)};

        // The name table entry is a 2-byte hash followed by a 2-byte offset.
        const std::size_t nameEntry = sectionStart + nameOffset + 4 + index * 4;
        joint.name = reader.string(sectionStart + nameOffset + reader.u16(nameEntry + 2));
    }

    // Resolve each joint's nearest enclosing joint from the INF1 scene graph,
    // mirroring Java Joint.doCalc()'s parent search.
    for (const auto& node : model.sceneGraph) {
        if (node.nodeType != 1) {
            continue;
        }
        const auto jointIndex = static_cast<std::size_t>(node.nodeId);
        if (jointIndex >= model.joints.size()) {
            continue;
        }
        std::int32_t parent = node.parentIndex;
        while (parent >= 0) {
            const auto& parentNode = model.sceneGraph[static_cast<std::size_t>(parent)];
            if (parentNode.nodeType == 1) {
                break;
            }
            parent = parentNode.parentIndex;
        }
        model.joints[jointIndex].parentIndex = parent;
    }
}

// Resolves the index vector a SHP1 attribute writes into.
std::vector<std::uint32_t>* indexTarget(BmdPrimitive& primitive, int arrayType) {
    if (arrayType == 0) {
        return &primitive.positionMatrixIndices;
    }
    if (arrayType == 9) {
        return &primitive.positionIndices;
    }
    if (arrayType == 10) {
        return &primitive.normalIndices;
    }
    if (arrayType >= 11 && arrayType <= 12) {
        return &primitive.colorIndices[static_cast<std::size_t>(arrayType - 11)];
    }
    if (arrayType >= 13 && arrayType <= 20) {
        return &primitive.texcoordIndices[static_cast<std::size_t>(arrayType - 13)];
    }
    return nullptr; // GX tex/position matrix attribs 1..8 are unused statically
}

// Reads one primitive's per-vertex indices, advancing `cursor`. u8 and u16
// indices are both accepted; the width comes from the attribute's high byte.
void readPrimitiveIndices(const Reader& reader, std::size_t& cursor, const BmdBatch& batch,
                          std::size_t vertexCount, BmdPrimitive& primitive) {
    for (const auto attribute : batch.attributeIds) {
        if (auto* target = indexTarget(primitive, attribute & 0xFF)) {
            target->reserve(vertexCount);
        }
    }
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        for (const auto attribute : batch.attributeIds) {
            const int indexWidth = attribute & 0xFF00;
            std::uint32_t value = 0;
            if (indexWidth == 0x0300) {
                value = reader.u16(cursor);
                cursor += 2;
            } else if (indexWidth <= 0x0200) {
                value = reader.u8(cursor);
                cursor += 1;
            } else {
                throw std::runtime_error("BMD: unsupported SHP1 index attribute " + std::to_string(indexWidth));
            }
            const int arrayType = attribute & 0xFF;
            if (auto* target = indexTarget(primitive, arrayType)) {
                target->push_back(arrayType == 0 ? value / 3U : value);
            }
        }
    }
}

// SHP1: shapes. A batch groups packets that share a matrix type and attribute
// layout; each packet carries a draw-matrix table and a primitive stream.
// 16-bit primitive values are read with the file's byte order (the Java reader
// assembles them in file order, which byte-swaps little-endian files).
void readSHP1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    const auto count = static_cast<std::size_t>(reader.u16(sectionStart + 8));
    const auto batchOffset = reader.u32(sectionStart + 0xC);
    const auto remapOffset = reader.u32(sectionStart + 0x10);
    const auto attributeOffset = reader.u32(sectionStart + 0x18);
    const auto matrixTableOffset = reader.u32(sectionStart + 0x1C);
    const auto dataOffset = reader.u32(sectionStart + 0x20);
    const auto matrixDataOffset = reader.u32(sectionStart + 0x24);
    const auto packetOffset = reader.u32(sectionStart + 0x28);

    model.batches.assign(count, {});
    for (std::size_t index = 0; index < count; ++index) {
        const auto remap = static_cast<std::size_t>(reader.u16(sectionStart + remapOffset + index * 2));
        const std::size_t record = sectionStart + batchOffset + remap * 0x28;
        BmdBatch& batch = model.batches[index];
        // Batch entry (0x28 bytes): matrix type, a pad byte, then the packet
        // count, attribute-list offset, first-matrix index and first-packet
        // index as four consecutive u16s (Java: readByte, skip(1), then four
        // readShorts). A previous port read all four one slot too far, which
        // zeroed the packet count on real BDLs and silently dropped every
        // triangle while the probe still showed healthy batches.
        batch.matrixType = reader.u8(record);
        const auto packetCount = static_cast<std::size_t>(reader.u16(record + 2));
        const auto attributesOffset = static_cast<std::size_t>(reader.u16(record + 4));
        const auto firstMatrixIndex = static_cast<std::size_t>(reader.u16(record + 6));
        const auto firstPacketIndex = static_cast<std::size_t>(reader.u16(record + 8));

        // Attribute list: (array type, data type) pairs terminated by 0xFF.
        // Java aborts after 0x20 pairs on a malformed table instead of
        // scanning to EOF, so this port caps the walk the same way.
        std::size_t attributeCursor = sectionStart + attributeOffset + attributesOffset;
        int attributePairs = 0;
        while (true) {
            if (++attributePairs > 0x20) {
                throw std::runtime_error("BMD: SHP1 attribute list is unterminated");
            }
            const auto arrayType = reader.u32(attributeCursor);
            const auto dataType = reader.u32(attributeCursor + 4);
            attributeCursor += 8;
            if (arrayType == 0xFF) {
                break;
            }
            batch.attributeIds.push_back(
                static_cast<std::uint16_t>((arrayType & 0xFFU) | ((dataType & 0xFFU) << 8)));
        }

        int arrayMask = 0;
        for (const auto attribute : batch.attributeIds) {
            arrayMask |= 1 << (attribute & 0xFF);
        }

        batch.packets.assign(packetCount, {});
        for (std::size_t packetIndex = 0; packetIndex < packetCount; ++packetIndex) {
            BmdPacket& packet = batch.packets[packetIndex];
            const std::size_t matrixEntry = sectionStart + matrixDataOffset + (firstMatrixIndex + packetIndex) * 8;
            const auto singleMatrix = reader.u16(matrixEntry);
            const auto multiCount = static_cast<std::size_t>(reader.u16(matrixEntry + 2));
            const auto multiStart = static_cast<std::size_t>(reader.u32(matrixEntry + 4));

            if (batch.matrixType == 3) {
                packet.matrixTable.reserve(multiCount);
                for (std::size_t slot = 0; slot < multiCount; ++slot) {
                    packet.matrixTable.push_back(
                        reader.u16(sectionStart + matrixTableOffset + (multiStart + slot) * 2));
                }
            } else {
                packet.matrixTable.push_back(singleMatrix);
            }

            const std::size_t locationEntry = sectionStart + packetOffset + (firstPacketIndex + packetIndex) * 8;
            const auto packetSize = static_cast<std::size_t>(reader.u32(locationEntry));
            const auto packetDataOffset = static_cast<std::size_t>(reader.u32(locationEntry + 4));
            if (packetSize > sectionSize || packetDataOffset > sectionSize - packetSize) {
                throw std::runtime_error("BMD: SHP1 packet data is out of range");
            }
            std::size_t cursor = sectionStart + dataOffset + packetDataOffset;
            const std::size_t packetEnd = cursor + packetSize;

            while (cursor < packetEnd) {
                const auto primitiveType = reader.u8(cursor);
                cursor += 1;
                if (primitiveType == 0) {
                    break;
                }
                const auto vertexCount = static_cast<std::size_t>(reader.u16(cursor));
                cursor += 2;

                BmdPrimitive primitive;
                primitive.type = primitiveType;
                primitive.arrayMask = arrayMask;
                readPrimitiveIndices(reader, cursor, batch, vertexCount, primitive);
                packet.primitives.push_back(std::move(primitive));
            }
        }
    }
}

std::array<float, 4> mat3Color8(const Reader& reader, std::size_t table, std::uint16_t index) {
    const std::size_t at = table + static_cast<std::size_t>(index) * 4;
    return {static_cast<float>(reader.u8(at)) / 255.0F, static_cast<float>(reader.u8(at + 1)) / 255.0F,
            static_cast<float>(reader.u8(at + 2)) / 255.0F, static_cast<float>(reader.u8(at + 3)) / 255.0F};
}

// MAT3: materials. Each material's fields live in one 0x14C-byte record whose
// values are byte/short indices into per-field tables. Only the fields a static
// preview needs are kept, but the record is still walked field by field so the
// cursor lands exactly where the Java reader's does.
void readMAT3(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    // One material record: the byte/short field indices the reader walks in
    // order, from pixelEngineMode through the texture index list.
    constexpr std::size_t kRecordSize = 0x14C;
    const auto count = static_cast<std::size_t>(reader.u16(sectionStart + 8));
    const auto dataTableOffset = reader.u32(sectionStart + 0xC);
    const auto remapOffset = reader.u32(sectionStart + 0x10);
    const auto nameOffset = reader.u32(sectionStart + 0x14);
    const auto cullModeOffset = reader.u32(sectionStart + 0x1C);
    const auto materialColorOffset = reader.u32(sectionStart + 0x20);
    const auto colorChannelCountOffset = reader.u32(sectionStart + 0x24);
    // Colour-channel and texture-index tables are skipped positionally below
    // (light channels and texture indices are walked as raw record bytes), so
    // their offsets are only read to keep the field walk explicit.
    (void)reader.u32(sectionStart + 0x28); // ColorChannelTableOffset
    (void)reader.u32(sectionStart + 0x48); // TextureIndexTableOffset — read positionally later
    const auto ambientColorOffset = reader.u32(sectionStart + 0x2C);
    const auto texGenCountOffset = reader.u32(sectionStart + 0x34);
    const auto tevStageCountOffset = reader.u32(sectionStart + 0x58);
    const auto zModeOffset = reader.u32(sectionStart + 0x74);
    const auto zCompLocOffset = reader.u32(sectionStart + 0x78);
    const auto ditherOffset = reader.u32(sectionStart + 0x7C);
    (void)sectionSize;

    const auto byteTable = [&reader](std::size_t table, std::size_t& cursor) {
        const auto index = reader.u8(cursor);
        cursor += 1;
        return reader.u8(table + static_cast<std::size_t>(index));
    };
    // Kept for the day a short-indexed table is needed again; the colour and
    // ambient lookups use color8Table and the texture list is positional.
    [[maybe_unused]] const auto shortTable = [&reader](std::size_t table, std::size_t& cursor) {
        const auto index = reader.u16(cursor);
        cursor += 2;
        return reader.u16(table + static_cast<std::size_t>(index) * 2);
    };
    const auto color8Table = [&reader](std::size_t table, std::size_t& cursor) {
        const auto index = reader.u16(cursor);
        cursor += 2;
        return mat3Color8(reader, table, index);
    };

    model.materials.assign(count, {});
    for (std::size_t index = 0; index < count; ++index) {
        BmdMaterial& material = model.materials[index];

        const std::size_t nameEntry = sectionStart + nameOffset + 4 + index * 4;
        material.name = reader.string(sectionStart + nameOffset + reader.u16(nameEntry + 2));

        const auto remap = static_cast<std::size_t>(reader.u16(sectionStart + remapOffset + index * 2));
        std::size_t cursor = sectionStart + dataTableOffset + remap * kRecordSize;

        material.pixelEngineMode = reader.u8(cursor);
        cursor += 1;
        material.cullingMode = byteTable(sectionStart + cullModeOffset, cursor);
        const auto colorChannelCount = static_cast<int>(byteTable(sectionStart + colorChannelCountOffset, cursor));
        const auto texGenCount = static_cast<int>(byteTable(sectionStart + texGenCountOffset, cursor));
        const auto tevStageCount = static_cast<int>(byteTable(sectionStart + tevStageCountOffset, cursor));
        (void)texGenCount;
        (void)tevStageCount;
        // zcomp loc is stored as a byte index; the value decides whether depth
        // testing happens before texturing, which the preview does not need.
        (void)byteTable(sectionStart + zCompLocOffset, cursor);
        { // ZMode: one byte index into a four-byte-per-entry table. Java names
            // these BlendEnableDepthTest / BlendDepthFunction /
            // BlendWriteToZBuffer; the entry doubles as the blend switch the
            // game itself consults, so both land on the material together.
            const auto zModeIndex = static_cast<std::size_t>(reader.u8(cursor));
            cursor += 1;
            const std::size_t zBase = sectionStart + zModeOffset + zModeIndex * 4;
            const bool zTest = reader.u8(zBase) != 0;
            material.depthFunction = reader.u8(zBase + 1);
            material.depthWrite = zTest && reader.u8(zBase + 2) != 0;
            material.blendMode = reader.u8(zBase + 3) != 0 ? 1 : 0;
        } // ZMode
        // Dither enable flag (byte table lookup).
        (void)byteTable(sectionStart + ditherOffset, cursor);

        material.diffuseColor = color8Table(sectionStart + materialColorOffset, cursor);
        (void)color8Table(sectionStart + materialColorOffset, cursor); // second colour channel
        // Light channels: two entries of one short index each when present,
        // otherwise four skipped bytes per channel (matches the Java reader).
        for (int channel = 0; channel < 2; ++channel) {
            if (channel < colorChannelCount) {
                cursor += 2;
                cursor += 2;
            } else {
                cursor += 4;
            }
        }
        material.ambientColor = color8Table(sectionStart + ambientColorOffset, cursor);
        (void)color8Table(sectionStart + ambientColorOffset, cursor);

        cursor += 16; // light table (eight shorts), unused by a static preview
        // Texture generators, post texture generators, texture matrices and
        // their post block are positional: eight, eight, ten and twenty shorts
        // respectively, whatever the counts above say.
        cursor += 16;
        cursor += 16;
        cursor += 20;
        cursor += 40;

        for (std::size_t slot = 0; slot < material.textureIndices.size(); ++slot) {
            // Texture indices are a raw short array inside the record; the
            // short-table path would index the table, which this layout does not
            // have. 0xFFFF means "unused texture map".
            const auto textureIndex = reader.u16(cursor);
            cursor += 2;
            material.textureIndices[slot] = textureIndex == 0xFFFFU ? -1 : static_cast<std::int32_t>(textureIndex);
        }
    }
}

// TEX1: texture entries, each a BTI blob whose internal offsets are relative to
// its own entry start (handled by the native BTI decoder).
void readTEX1(const Reader& reader, std::size_t sectionStart, std::size_t sectionSize, BmdModel& model) {
    const auto count = static_cast<std::size_t>(reader.u16(sectionStart + 8));
    const auto entriesOffset = reader.u32(sectionStart + 0xC);

    const auto section = reader.data.subspan(sectionStart, sectionSize);
    model.textures.clear();
    model.textures.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        model.textures.push_back(parseBti(section, entriesOffset + index * 32, reader.endian));
    }
}

} // namespace

const BmdJoint* BmdModel::findJoint(std::string_view name) const noexcept {
    for (const auto& joint : joints) {
        if (joint.name == name) {
            return &joint;
        }
    }
    return nullptr;
}

BmdModel parseBmd(std::span<const std::uint8_t> data) {
    if (data.size() < 0x20) {
        throw std::runtime_error("BMD: file is truncated");
    }

    Reader reader{data, Endian::big};
    const auto magic = reader.u32(0);
    if (magic == kMagicLittleEndian) {
        reader.endian = Endian::little;
    } else if (magic != kMagicBigEndian) {
        throw std::runtime_error("BMD: file does not start with a J3D model tag");
    }

    BmdModel model;
    model.bigEndian = reader.endian == Endian::big;
    model.version.assign(reinterpret_cast<const char*>(data.data() + 4), 4);

    const auto sectionCount = reader.u32(0xC);
    std::size_t position = 0x20;
    for (std::uint32_t index = 0; index < sectionCount; ++index) {
        if (position > data.size() || data.size() - position < 8) {
            throw std::runtime_error("BMD: section table is truncated");
        }
        const auto tag = reader.u32(position);
        const auto size = static_cast<std::size_t>(reader.u32(position + 4));
        if (size < 8 || size > data.size() - position) {
            throw std::runtime_error("BMD: section size is out of range");
        }
        const std::size_t sectionStart = position;
        // Attribute any read failure inside a section to that section, so a
        // probe message alone identifies the failing stage and offset.
        try {
            switch (tag) {
                case kSectionINF1:
                    readINF1(reader, sectionStart, size, model);
                    break;
                case kSectionVTX1:
                    readVTX1(reader, sectionStart, size, model);
                    break;
                case kSectionEVP1:
                    readEVP1(reader, sectionStart, size, model);
                    break;
                case kSectionDRW1:
                    readDRW1(reader, sectionStart, size, model);
                    break;
                case kSectionJNT1:
                    readJNT1(reader, sectionStart, size, model);
                    break;
                case kSectionSHP1:
                    readSHP1(reader, sectionStart, size, model);
                    break;
                case kSectionMAT3:
                    readMAT3(reader, sectionStart, size, model);
                    break;
                case kSectionMDL3: // BDL pre-header; nothing to read, like Java
                    break;
                case kSectionTEX1:
                    readTEX1(reader, sectionStart, size, model);
                    break;
                default:
                    // Real BMD/BDL files carry sections this reader does not need
                    // yet (EVW1, PTH1, SRT1, IOR1, BOM1, SMP1, CLR1, PAT1, ANK1,
                    // etc.). Skip them silently — the geometry sections above are
                    // all that the viewport needs to draw the bind-pose mesh.
                    break;
            }
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(error.what()) + " (section " + sectionTagName(tag) +
                                     " at 0x" + hexOffset(sectionStart) + ")");
        }
        position = sectionStart + size;
    }

    if (!model.positions.empty()) {
        model.boundsMin = model.positions.front();
        model.boundsMax = model.positions.front();
        for (const auto& point : model.positions) {
            model.boundsMin = {std::min(model.boundsMin.x, point.x), std::min(model.boundsMin.y, point.y),
                               std::min(model.boundsMin.z, point.z)};
            model.boundsMax = {std::max(model.boundsMax.x, point.x), std::max(model.boundsMax.y, point.y),
                               std::max(model.boundsMax.z, point.z)};
        }
    }
    return model;
}

} // namespace whitehole::smg
