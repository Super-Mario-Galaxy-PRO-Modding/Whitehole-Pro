#pragma once

// BMD/BDL (J3D) model reading.
//
// Ports the format knowledge of the Java Bmd.java reader: INF1 scene graph,
// VTX1 vertex arrays, SHP1 shapes/draw lists, JNT1 joints, DRW1 draw-matrix
// table, EVP1 envelope weights, MAT3 materials and TEX1 textures (decoded
// through the native BTI decoder). Both byte orders are accepted, and every
// offset is bounds-checked so a malformed file throws std::runtime_error
// instead of reading past the buffer.
//
// The reader decodes the bind pose exactly, which is what the scene editor
// draws. Animation (BCK/BTP/BTK/BRK) and skinning are a separate lane, so
// joints are exposed as raw transforms plus their hierarchy.

#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/bti.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// GX primitive types found in SHP1 draw lists.
enum class BmdPrimitiveType : std::uint8_t {
    Quads = 0x80,
    Triangles = 0x90,
    TriangleStrip = 0x98,
    TriangleFan = 0xA0,
    Lines = 0xA8,
    LineStrip = 0xB0,
    Points = 0xB8,
};

// Vertex attribute array ids: VTX1 array types and the low byte of a SHP1
// attribute word (attrib = arrayType | dataType << 8).
enum class BmdArrayId : std::uint8_t {
    PositionMatrix = 0,
    Position = 9,
    Normal = 10,
    Color0 = 11,
    Color1 = 12,
    TexCoord0 = 13,
    TexCoord1 = 14,
    TexCoord2 = 15,
    TexCoord3 = 16,
    TexCoord4 = 17,
    TexCoord5 = 18,
    TexCoord6 = 19,
    TexCoord7 = 20,
};

// INF1 node. `nodeType` 0 draws a shape, 1 draws a joint.
struct BmdSceneNode {
    std::int16_t materialIndex{-1};
    std::int16_t nodeId{0};
    int parentIndex{-1};
    int nodeType{0};
};

// JNT1 joint (raw bind-pose transform, J3D conventions: rotation is radians).
struct BmdJoint {
    std::string name;
    std::uint16_t matrixFlags{0};
    std::uint8_t doNotInheritParentScale{0};
    math::Vec3f scale{1.0F, 1.0F, 1.0F};
    math::Vec3f rotationRadians{};
    math::Vec3f translation{};
    int parentIndex{-1}; // nearest enclosing joint, -1 for a root
};

// One SHP1 draw list: a stream of vertices sharing one primitive type.
struct BmdPrimitive {
    std::uint8_t type{0};
    int arrayMask{0};
    std::vector<std::uint32_t> positionMatrixIndices;
    std::vector<std::uint32_t> positionIndices;
    std::vector<std::uint32_t> normalIndices;
    std::array<std::vector<std::uint32_t>, 2> colorIndices;
    std::array<std::vector<std::uint32_t>, 8> texcoordIndices;

    [[nodiscard]] std::size_t vertexCount() const noexcept {
        return arrayMask == 0 ? 0 : positionIndices.size();
    }
};

struct BmdPacket {
    std::vector<std::uint16_t> matrixTable; // DRW1 matrix ids for this packet
    std::vector<BmdPrimitive> primitives;
};

struct BmdBatch {
    std::uint8_t matrixType{0};
    std::vector<std::uint16_t> attributeIds; // arrayType | dataType << 8
    std::vector<BmdPacket> packets;
};

// MAT3 material, reduced to what a static preview needs. Blend, cull, depth
// and alpha-compare fields drive the per-material GL state in the viewport
// (opaque/translucent split, alpha testing, face culling). They default to
// the "draw everything, depth on" behaviour the old preview assumed, so any
// model that never sets them renders exactly as before.
struct BmdMaterial {
    std::string name;
    std::uint8_t pixelEngineMode{0};
    std::uint8_t cullingMode{0};
    std::array<float, 4> diffuseColor{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> ambientColor{1.0F, 1.0F, 1.0F, 1.0F};
    // TEV texture map -> TEX1 index; -1 means the map is unused.
    std::array<std::int32_t, 8> textureIndices{-1, -1, -1, -1, -1, -1, -1, -1};
    // Per TEV stage plumbing (MAT3 TEV-order table, Java Bmd parity):
    // tevTexMap[stage] is the GX texture map id (0..7, -1 = none/0xFF) and
    // tevTexCoord[stage] the GX texcoord id (0..7, -1 = none/0xFF).
    std::array<std::int32_t, 16> tevTexCoord{-1, -1, -1, -1, -1, -1, -1, -1,
                                             -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<std::int32_t, 16> tevTexMap{-1, -1, -1, -1, -1, -1, -1, -1,
                                           -1, -1, -1, -1, -1, -1, -1, -1};
    int tevStageCount{0};
    // First TEV stage (in order) whose texture map resolves to a real TEX1
    // entry, or the first used map when no stage resolves (untextured -> -1).
    // Java binds every stage's texture; the fixed-function preview draws one,
    // so it must be the colour stage's texture, not blindly slot 0.
    [[nodiscard]] std::int32_t primaryTextureSlot() const noexcept {
        for (int stage = 0; stage < tevStageCount && stage < 16; ++stage) {
            const auto map = tevTexMap[static_cast<std::size_t>(stage)];
            if (map >= 0 && map < 8) {
                const auto slot = textureIndices[static_cast<std::size_t>(map)];
                if (slot >= 0) { return slot; }
            }
        }
        for (const auto slot : textureIndices) {
            if (slot >= 0) { return slot; }
        }
        return -1;
    }
    // Texcoord set the primary stage samples (0..7, defaults to 0 when the
    // stage has none or the mesh does not carry it).
    [[nodiscard]] int primaryTexCoordSet() const noexcept {
        for (int stage = 0; stage < tevStageCount && stage < 16; ++stage) {
            const auto map = tevTexMap[static_cast<std::size_t>(stage)];
            if (map >= 0 && map < 8 &&
                textureIndices[static_cast<std::size_t>(map)] >= 0) {
                const auto coord = tevTexCoord[static_cast<std::size_t>(stage)];
                return (coord >= 0 && coord < 8) ? coord : 0;
            }
        }
        return 0;
    }
    // GX blend mode (0 = none/logic, 1 = blend, 3 = subtract) plus its source
    // and destination factors, as stored in the MAT3 blend-info table.
    std::uint8_t blendMode{0};
    std::uint8_t blendSrcFactor{0};
    std::uint8_t blendDstFactor{0};
    std::uint8_t blendOp{0};
    // ZMode's three independent bytes: whether the depth test runs, which
    // compare function it uses, and whether fragments update the depth buffer.
    // Kept separate (like Bmd.java's BlendEnableDepthTest / BlendDepthFunction
    // / BlendWriteToZBuffer) so the viewport can apply each one exactly as
    // BmdRenderer does.
    bool depthTest{true};
    std::uint8_t depthFunction{1};
    bool depthWrite{true};
    // Alpha-compare pair: func 0..7 (never/less/equal/lequal/greater/notequal/
    // gequal/always), reference 0..255, and the AND(0)/OR(1) merge operation.
    std::uint8_t alphaFunc0{7};
    std::uint8_t alphaRef0{0};
    std::uint8_t alphaFunc1{7};
    std::uint8_t alphaRef1{0};
    std::uint8_t alphaOp{1};

    // True when the material draws through the blended pass: Takochu's
    // DrawFlag == 4 test, re-derived here as pixelEngineMode == 4 (the value
    // the Java reader documents as "translucent"). Blend-type materials count
    // too, since the game blends them regardless of the draw flag.
    [[nodiscard]] bool translucent() const noexcept {
        return pixelEngineMode == 4 || blendMode == 1 || blendMode == 3;
    }
    // Whether BmdRenderer would enable GL_ALPHA_TEST: it only skips the test
    // when an OR merge has an ALWAYS side (the comparison can never fail
    // anyway). Every other combination runs a real test -- including the
    // AND-with-NEVER case, which discards every fragment.
    [[nodiscard]] bool alphaTestEnabled() const noexcept {
        return !(alphaOp == 1 && (alphaFunc0 == 7 || alphaFunc1 == 7));
    }
};

struct BmdModel {
    bool bigEndian{true};
    std::string version; // "bmd3" / "bdl4" / ...
    std::uint16_t miscFlags{0};
    int vertexCount{0};

    std::vector<math::Vec3f> positions;
    std::vector<math::Vec3f> normals;
    std::array<std::vector<std::array<float, 4>>, 2> colors;
    std::array<std::vector<math::Vec2f>, 8> texcoords;

    std::vector<BmdSceneNode> sceneGraph;
    std::vector<BmdJoint> joints;
    std::vector<BmdBatch> batches;
    std::vector<BmdMaterial> materials;
    std::vector<Bti> textures;

    // DRW1: weighted flag per draw matrix and its joint/envelope index.
    std::vector<bool> matrixWeighted;
    std::vector<std::uint16_t> matrixIndices;

    // EVP1 envelopes: one entry per draw matrix that uses skinning weights.
    std::vector<std::vector<std::uint16_t>> envelopeJoints;
    std::vector<std::vector<float>> envelopeWeights;

    math::Vec3f boundsMin{};
    math::Vec3f boundsMax{};

    [[nodiscard]] const BmdJoint* findJoint(std::string_view name) const noexcept;
    [[nodiscard]] bool valid() const noexcept { return !positions.empty(); }
};

// Parses a whole BMD/BDL file. Throws std::runtime_error on malformed input.
[[nodiscard]] BmdModel parseBmd(std::span<const std::uint8_t> data);

} // namespace whitehole::smg
