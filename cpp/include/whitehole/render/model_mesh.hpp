#pragma once

// Model mesh builder: turns a parsed BMD/BDL into a flat triangle list in model
// space, which the viewport draws for objects whose model was found on disk.
//
// The mesh is the bind pose exactly as stored (no skinning), so a scene preview
// shows the same geometry the game loads before any animation plays. Pure data
// and math: no Win32, no OpenGL, unit-testable.

#include "whitehole/math/geometry.hpp"
#include "whitehole/smg/bmd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace whitehole::render {

struct ModelVertex {
    math::Vec3f position{};
    math::Vec3f normal{};
    std::array<float, 2> texCoord{}; // first texture coordinate set
};

struct ModelTriangle {
    ModelVertex a{};
    ModelVertex b{};
    ModelVertex c{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::int32_t materialIndex{-1};
    // True when the owning material is translucent (GX blend mode or
    // pixelEngineMode==4 / DrawFlag==4): these triangles draw in the
    // back-to-front translucent pass instead of the opaque depth-write pass.
    bool translucent{false};
};

struct ModelMesh {
    std::vector<ModelTriangle> triangles;
    math::Vec3f boundsMin{};
    math::Vec3f boundsMax{};
    float radius{0.0F};
    // Lines, line strips and points are counted but not converted: a solid
    // preview has nothing to draw for them.
    std::size_t skippedPrimitives{0};
    // Packets the emission loop saw but could not place: a packet whose SHP1
    // matrix table decoded empty, and one whose draw-matrix id falls outside
    // the DRW1 table. Both used to be silent `continue`s, which hid the SHP1
    // field-offset bug behind healthy-looking probe batches.
    std::size_t droppedEmptyMatrixTable{0};
    std::size_t droppedBadMatrixIndex{0};
    // Material + texture tables copied from the parsed model, so the renderer
    // can bind triangle.materialIndex -> materials[] -> textureIndices[0] ->
    // textures[] without keeping the whole BmdModel alive. Stays empty for
    // hand-built test meshes (which render flat-colored as before).
    std::vector<smg::BmdMaterial> materials;
    std::vector<smg::Bti> textures;

    [[nodiscard]] bool empty() const noexcept { return triangles.empty(); }
};

// Converts every shape node of a model into triangles. Primitives whose indices
// fall outside the vertex arrays are skipped rather than clamped, so a damaged
// file yields the geometry that is valid instead of failing the whole model.
[[nodiscard]] ModelMesh buildModelMesh(const smg::BmdModel& model);

// Appends every triangle of `src` onto `dst`, remapping triangle material
// indices and material texture indices so multi-part merges (PlantA00 +
// PlantA01, ...) keep pointing at the right material/texture after the tables
// are concatenated. Counters are summed; bounds/radius are NOT recomputed
// here (the caller runs recomputeMeshBounds after all parts are in).
void appendModelMesh(ModelMesh& dst, const ModelMesh& src);

// Re-derives boundsMin/boundsMax/radius from the triangle list.
void recomputeMeshBounds(ModelMesh& mesh) noexcept;

} // namespace whitehole::render
