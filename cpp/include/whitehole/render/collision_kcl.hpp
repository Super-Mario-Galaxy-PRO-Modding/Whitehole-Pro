#pragma once

// Clean-room KCL parser: reads the big-endian prism collision format the SMG
// engine ships (ObjectData model archives, and stage archives when a mod
// includes one) and turns it into SnapTriangle soup for surface snapping.
// The octree is deliberately skipped -- the editor raycasts the full prism
// list directly, which is simpler and fast enough at editor scale.
//
// Format references were the engine's own KCollisionServer plus the Java
// Kcl reader; this header only consumes bytes, never game code.

#include "whitehole/render/surface_snap.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace whitehole::render {

// Parses every prism of a big-endian KCL blob into triangles in the file's
// own coordinate space. Each triangle keeps the KCL face normal (the
// outward-pointing surface normal the engine's own hit tests use) and
// kCollisionNoOwner as its source index. Throws std::runtime_error on
// malformed input; prisms with degenerate geometry or out-of-range indices
// are skipped instead of failing the whole file.
[[nodiscard]] std::vector<SnapTriangle> parseKclTriangles(const std::vector<std::uint8_t>& data);

// True when `path` ends in ".kcl", case-insensitively.
[[nodiscard]] bool isKclPath(std::string_view path) noexcept;

} // namespace whitehole::render