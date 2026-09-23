#include "whitehole/render/model_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace whitehole::render {
namespace {

bool fetchVertex(const smg::BmdModel& model, const smg::BmdPrimitive& primitive, std::size_t vertexIndex,
                 ModelVertex& out) {
    if (vertexIndex >= primitive.positionIndices.size()) { return false; }
    const auto positionIndex = static_cast<std::size_t>(primitive.positionIndices[vertexIndex]);
    if (positionIndex >= model.positions.size()) { return false; }
    out.position = model.positions[positionIndex];
    if (vertexIndex < primitive.normalIndices.size()) {
        const auto normalIndex = static_cast<std::size_t>(primitive.normalIndices[vertexIndex]);
        if (normalIndex < model.normals.size()) { out.normal = model.normals[normalIndex]; }
    }
    const auto& texcoordIndices = primitive.texcoordIndices[0];
    if (vertexIndex < texcoordIndices.size()) {
        const auto texcoordIndex = static_cast<std::size_t>(texcoordIndices[vertexIndex]);
        if (texcoordIndex < model.texcoords[0].size()) {
            const auto& uv = model.texcoords[0][texcoordIndex];
            out.texCoord = std::array<float, 2>{uv.x, uv.y};
        }
    }
    return true;
}

math::Vec3f faceNormal(const ModelVertex& a, const ModelVertex& b, const ModelVertex& c) {
    const math::Vec3f normal = math::Vec3f::cross(b.position - a.position, c.position - a.position);
    return normal.length() < 0.000001F ? math::Vec3f{0.0F, 1.0F, 0.0F} : normal.normalized();
}

std::array<float, 4> materialColor(const smg::BmdModel& model, std::int16_t materialIndex) {
    if (materialIndex < 0 || static_cast<std::size_t>(materialIndex) >= model.materials.size()) {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    return model.materials[static_cast<std::size_t>(materialIndex)].diffuseColor;
}

} // namespace

math::Matrix4 jointLocalMatrix(const smg::BmdJoint& joint) noexcept {
    return math::Matrix4::translation(joint.translation) *
           math::Matrix4::rotationZ(joint.rotationRadians.x) *
           math::Matrix4::rotationY(joint.rotationRadians.y) *
           math::Matrix4::rotationX(joint.rotationRadians.z) *
           math::Matrix4::scale(joint.scale);
}

std::vector<math::Matrix4> jointWorldMatrices(const smg::BmdModel& model) noexcept {
    std::vector<math::Matrix4> world(model.joints.size());
    for (std::size_t index = 0; index < model.joints.size(); ++index) {
        const auto& joint = model.joints[index];
        math::Matrix4 local = jointLocalMatrix(joint);
        if (joint.parentIndex >= 0 && static_cast<std::size_t>(joint.parentIndex) < world.size()) {
            world[index] = world[joint.parentIndex] * local;
        } else {
            world[index] = local;
        }
    }
    return world;
}

math::Matrix4 drawMatrix(const smg::BmdModel& model, std::size_t matrixIndex,
                         const std::vector<math::Matrix4>& jointWorld) noexcept {
    // Three parallel DRW1/EVP1 tables feed a weighted draw matrix, and they are
    // sized independently: matrixIndices and matrixWeighted come from DRW1 (one
    // entry per draw matrix), while envelopeJoints / envelopeWeights come from
    // EVP1 (one entry per *envelope*). J3D semantics for a weighted entry are
    // that DRW1's stored index selects an EVP1 envelope -- not a matrix -- so
    // the envelope list is indexed with that value, and both lists are range
    // checked. Indexing an EVP1 list with a DRW1 matrix index is what used to
    // read out of bounds and fault (0xC0000005) on real files.
    if (matrixIndex >= model.matrixIndices.size() || matrixIndex >= model.matrixWeighted.size()) {
        return math::Matrix4{};
    }
    // Java: mt[currentMtxID] is already a baked model-space matrix for the
    // first table entry (m[3] identity row), which the renderer multiplies
    // every draw matrix by. drawMatrix(0) is therefore guaranteed identity --
    // enforce that here so a JNT1/DRW1 decode drift can never squash a model
    // to a point again.
    if (matrixIndex == 0) { return math::Matrix4{}; }
    const auto bindIndex = static_cast<std::size_t>(model.matrixIndices[matrixIndex]);
    if (!model.matrixWeighted[matrixIndex]) {
        // Single-bind entry: the index is a JNT1 joint index.
        if (bindIndex < jointWorld.size()) { return jointWorld[bindIndex]; }
        return math::Matrix4{};
    }
    // Weighted entry: the index is an EVP1 envelope index.
    if (bindIndex >= model.envelopeJoints.size() || bindIndex >= model.envelopeWeights.size()) {
        return math::Matrix4{};
    }
    const auto& envelopeJoints = model.envelopeJoints[bindIndex];
    const auto& envelopeWeights = model.envelopeWeights[bindIndex];
    if (envelopeJoints.empty()) { return math::Matrix4{}; }
    const std::size_t blendCount = std::min(envelopeJoints.size(), envelopeWeights.size());
    float weightSum = 0.0F;
    for (std::size_t i = 0; i < blendCount; ++i) { weightSum += envelopeWeights[i]; }
    if (weightSum < 0.000001F) { weightSum = 1.0F; }
    math::Matrix4 result;
    result.values.fill(0.0F);
    result.values[15] = 1.0F;
    for (std::size_t i = 0; i < blendCount; ++i) {
        const auto jointIndex = static_cast<std::size_t>(envelopeJoints[i]);
        if (jointIndex >= jointWorld.size()) { continue; }
        const float w = envelopeWeights[i] / weightSum;
        const auto& jm = jointWorld[jointIndex];
        for (int j = 0; j < 16; ++j) { result.values[j] += jm.values[j] * w; }
    }
    return result;
}


void transformPrimitive(ModelMesh& mesh, const smg::BmdModel& model, const smg::BmdPrimitive& primitive,
                        const math::Matrix4& drawMatrix, const std::array<float, 4>& color,
                        std::int32_t materialIndex, bool translucent) {
    using smg::BmdPrimitiveType;
    const auto primitiveType = static_cast<BmdPrimitiveType>(primitive.type);
    const std::size_t count = primitive.positionIndices.size();
    if (primitiveType == BmdPrimitiveType::Triangles ||
        primitiveType == BmdPrimitiveType::TriangleStrip ||
        primitiveType == BmdPrimitiveType::TriangleFan ||
        primitiveType == BmdPrimitiveType::Quads) {
        struct TVertex { math::Vec3f position; math::Vec3f normal; std::array<float, 2> texCoord; bool valid{ false }; };
        std::vector<TVertex> tv(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto posIdx = static_cast<std::size_t>(primitive.positionIndices[i]);
            if (posIdx < model.positions.size()) {
                tv[i].position = drawMatrix.transformPoint(model.positions[posIdx]);
                tv[i].valid = true;
            }
            tv[i].texCoord = {0.0F, 0.0F};
            if (i < primitive.normalIndices.size()) {
                const auto nIdx = static_cast<std::size_t>(primitive.normalIndices[i]);
                if (nIdx >= model.normals.size()) { tv[i].normal = {0.0F, 1.0F, 0.0F}; }
                else {
                    const float m00 = drawMatrix.values[0], m01 = drawMatrix.values[1], m02 = drawMatrix.values[2];
                    const float m10 = drawMatrix.values[4], m11 = drawMatrix.values[5], m12 = drawMatrix.values[6];
                    const float m20 = drawMatrix.values[8], m21 = drawMatrix.values[9], m22 = drawMatrix.values[10];
                    const float det = m00 * (m11 * m22 - m12 * m21)
                                    - m01 * (m10 * m22 - m12 * m20)
                                    + m02 * (m10 * m21 - m11 * m20);
                    if (std::abs(det) > 0.000001F) {
                        const float idet = 1.0F / det;
                        const float a00 = (m11 * m22 - m12 * m21) * idet;
                        const float a01 = -(m10 * m22 - m12 * m20) * idet;
                        const float a02 = (m10 * m21 - m11 * m20) * idet;
                        const float a10 = -(m01 * m22 - m02 * m21) * idet;
                        const float a11 = (m00 * m22 - m02 * m20) * idet;
                        const float a12 = -(m00 * m21 - m01 * m20) * idet;
                        const float a20 = (m01 * m12 - m02 * m11) * idet;
                        const float a21 = -(m00 * m12 - m02 * m10) * idet;
                        const float a22 = (m00 * m11 - m01 * m10) * idet;
                        tv[i].normal = math::Vec3f{
                            model.normals[nIdx].x * a00 + model.normals[nIdx].y * a10 + model.normals[nIdx].z * a20,
                            model.normals[nIdx].x * a01 + model.normals[nIdx].y * a11 + model.normals[nIdx].z * a21,
                            model.normals[nIdx].x * a02 + model.normals[nIdx].y * a12 + model.normals[nIdx].z * a22
                        };
                    } else {
                        tv[i].normal = model.normals[nIdx];
                    }
                }
            }
            if (i < primitive.texcoordIndices[0].size()) {
                const auto tIdx = static_cast<std::size_t>(primitive.texcoordIndices[0][i]);
                if (tIdx < model.texcoords[0].size()) {
                    const auto& uv = model.texcoords[0][tIdx];
                    tv[i]    .texCoord = std::array<float, 2>{uv.x, uv.y};
                }
            }
        }
        auto pushTv = [&](std::size_t a, std::size_t b, std::size_t c) {
            if (a >= tv.size() || b >= tv.size() || c >= tv.size()) return;
            if (!tv[a].valid || !tv[b].valid || !tv[c].valid) return;
            ModelTriangle tri;
            tri.a = {tv[a].position, tv[a].normal, tv[a].texCoord};
            tri.b = {tv[b].position, tv[b].normal, tv[b].texCoord};
            tri.c = {tv[c].position, tv[c].normal, tv[c].texCoord};
            const math::Vec3f n = faceNormal(tri.a, tri.b, tri.c);
            ModelVertex* verts[3] = {&tri.a, &tri.b, &tri.c};
            for (ModelVertex* v : verts) {
                if (v->normal.length() < 0.000001F) { v->normal = n; }
            }
            tri.color = color;
            tri.materialIndex = materialIndex;
            tri.translucent = translucent;
            mesh.triangles.push_back(tri);
        };
        switch (primitiveType) {
            case BmdPrimitiveType::Triangles:
                for (std::size_t i = 0; i + 2 < count; i += 3) pushTv(i, i + 1, i + 2);
                break;
            case BmdPrimitiveType::TriangleStrip:
                for (std::size_t i = 0; i + 2 < count; ++i) pushTv(i, i + 1, i + 2);
                break;
            case BmdPrimitiveType::TriangleFan:
                for (std::size_t i = 1; i + 1 < count; ++i) pushTv(0, i, i + 1);
                break;
            case BmdPrimitiveType::Quads:
                for (std::size_t i = 0; i + 3 < count; i += 4) {
                    pushTv(i, i + 1, i + 2);
                    pushTv(i, i + 2, i + 3);
                }
                break;
            default:
                mesh.skippedPrimitives++;
                break;
        }
    } else {
        mesh.skippedPrimitives++;
    }
}



ModelMesh buildModelMesh(const smg::BmdModel& model) {
    ModelMesh mesh;
    if (model.positions.empty()) { return mesh; }
    const auto jointWorld = jointWorldMatrices(model);
    for (const auto& node : model.sceneGraph) {
        if (node.nodeType != 0) { continue; }
        const auto batchIndex = static_cast<std::size_t>(node.nodeId);
        if (batchIndex >= model.batches.size()) { continue; }
        const auto& batch = model.batches[batchIndex];
        const std::array<float, 4> color = materialColor(model, node.materialIndex);
        const bool translucent = node.materialIndex >= 0 &&
                                 static_cast<std::size_t>(node.materialIndex) < model.materials.size() &&
                                 model.materials[static_cast<std::size_t>(node.materialIndex)].translucent();
        for (const auto& packet : batch.packets) {
            for (const auto& primitive : packet.primitives) {
                if (primitive.positionIndices.empty()) { mesh.skippedPrimitives++; continue; }
                if (packet.matrixTable.empty()) { mesh.droppedEmptyMatrixTable++; continue; }
                const auto matrixIndex = static_cast<std::size_t>(packet.matrixTable[0]);
                if (matrixIndex >= model.matrixIndices.size()) { mesh.droppedBadMatrixIndex++; continue; }
                const auto draw = drawMatrix(model, matrixIndex, jointWorld);
                transformPrimitive(mesh, model, primitive, draw, color, node.materialIndex, translucent);
            }
        }
    }
    if (!mesh.triangles.empty()) {
        mesh.boundsMin = mesh.triangles.front().a.position;
        mesh.boundsMax = mesh.triangles.front().a.position;
        for (const auto& triangle : mesh.triangles) {
            const ModelVertex* vertices[3] = {&triangle.a, &triangle.b, &triangle.c};
            for (const ModelVertex* vertex : vertices) {
                mesh.boundsMin = {std::min(mesh.boundsMin.x, vertex->position.x),
                                  std::min(mesh.boundsMin.y, vertex->position.y),
                                  std::min(mesh.boundsMin.z, vertex->position.z)};
                mesh.boundsMax = {std::max(mesh.boundsMax.x, vertex->position.x),
                                  std::max(mesh.boundsMax.y, vertex->position.y),
                                  std::max(mesh.boundsMax.z, vertex->position.z)};
            }
        }
        mesh.radius = (mesh.boundsMax - mesh.boundsMin).length() * 0.5F;
    }
    return mesh;
}

} // namespace whitehole::render
