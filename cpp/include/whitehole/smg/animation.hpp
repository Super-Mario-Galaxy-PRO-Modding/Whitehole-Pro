#pragma once

// J3D animation reading and writing: BCK (joint transforms), BTK (texture
// matrices), BRK (TEV registers), BPK (material colours), BTP (texture
// patterns) and BVA (batch visibility).
//
// PORTED FROM the frozen Java readers in src/whitehole/smg/animation/
// (Hack.io, used with permission): byte offsets, the (time, value, tangent)
// track decode and the Hermite evaluation in valueAt() are transcribed from
// Bck/Btk/Brk/Bpk/Btp/Bva.java + J3DAnim/J3DAnimationTrack.java +
// J3DKeyFrame.java -- that code is what Whitehole's renderer ran on real SMG
// data. Where this file is deliberately STRICTER than Java (magic check,
// bounds checks, NaN refusal), the call site says so.
//
// DERIVED ELSEWHERE: the standard 0x20-byte J3D header (magic "J3D1", file
// size at 0x08, chunk count at 0x0C) and the per-chunk header (4-byte tag,
// 4-byte size, attribute byte) come from WindEditor's J3D Animation wiki and
// CloudModding's BCK page; the name-table layout (u32 count, {u16 hash,
// u16 offset-relative-to-table-start} entries, NUL-terminated strings) and
// the hash `hash = hash * 3 + byte` in wrapping u16 come from Graffito's
// j3d_name_hash_encoded. None of it is checked against a retail SMG file --
// no .bck ships here -- so BLUEPRINT section 16 item 1 carries that check.
//
// THE ROUND-TRIP CONTRACT (section 19's plan/apply discipline, same rules):
//   * parse(write(model)) is SEMANTICALLY equal to model;
//   * write(parse(write(model))) is BYTE-identical to write(model).
// It is NOT "byte-identical to any third-party file": foreign writers pad
// differently (retail stores "This is padding data to alignment") and this
// writer lays tables out in its own canonical order. The writer keeps
// verbatim the two byte regions it cannot interpret, carried in
// AnimFileHeader:
//   prefix  [0x00, firstModelledField): standard header, chunk tag, chunk
//           size (recomputed on write), attribute bytes.
//   fields  [firstModelledField, first table): counts and offsets this
//           reader models are patched over the copy; every reserved byte
//           survives, which is how TTK1's unknown middle region and
//           BTP/BVA's stored max-frame stay honest.
// Everything in the DATA region (key banks, name tables, remap tables,
// centres, ids) is rebuilt from the model.
//
// ROTATIONS: a BCK rotation track holds DEGREES -- the reference renderer
// multiplies by PI/180 at apply time (BmdRenderer.java:1361); BTK's scale
// has no 180 factor (Btk.java:53). sampleBckPoses() is the one place a
// joint's degrees become radians -- keep it that way.

#include "whitehole/math/geometry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::smg {

// ---------------------------------------------------------------------------
// The shared track layer: every format decodes its key banks into these.
// ---------------------------------------------------------------------------

// One keyframe. `time` is a frame index (integer in every real file; float
// evaluation still interpolates between them). Tangents are per-frame
// slopes, exactly as the reference stores them -- not scaled.
struct AnimKey {
    std::int16_t time{0};
    float value{0.0F};
    float inTangent{0.0F};
    float outTangent{0.0F};

    [[nodiscard]] bool operator==(const AnimKey&) const = default;
};

// A decodable animation track. Never empty in a file parsed from disk: the
// reference refuses zero-length tracks (J3DAnimationTrack.java:71) and this
// port refuses them on read AND write. An empty track evaluates to 0, which
// is what Java's getValueAtFrame does for one (parity, not a default a
// caller should rely on).
struct AnimTrack {
    std::vector<AnimKey> keys;

    // Value at `frame`: clamp outside the key range, Hermite between the two
    // bracketing keys using the left's OUT tangent and the right's IN
    // tangent, tangents scaled by the segment length -- the exact arithmetic
    // of J3DKeyFrame.GetHermiteInterpolation.
    [[nodiscard]] float valueAt(float frame) const;

    [[nodiscard]] bool operator==(const AnimTrack&) const = default;
};

// ---------------------------------------------------------------------------
// Per-file header preservation (see the round-trip contract above).
// ---------------------------------------------------------------------------

struct AnimFileHeader {
    // Bytes [0x00, firstModelledField), verbatim from the source file. On
    // write exactly two u32s inside it are recomputed: the file size at
    // 0x08 and the chunk size at chunkStart+4 (chunkStart is 0x20, or 0x30
    // when an SVR3 chunk sits between header and data -- detected by its
    // tag; the reference reader hardcodes 0x20, SMG files never carry it).
    std::vector<std::uint8_t> prefix;
    // Bytes [firstModelledField, first table), verbatim. Modelled counts and
    // offsets are patched over their positions on write; every other byte is
    // untouched.
    std::vector<std::uint8_t> fields;
    bool bigEndian{true};

    // NOTE: each file struct's operator== compares animation content plus
    // byte order, NOT prefix/fields -- a normalised reserved byte must not
    // make two equal animations compare unequal. Preservation of these two
    // regions is checked byte-for-byte by the tests instead.
    [[nodiscard]] bool operator==(const AnimFileHeader&) const = default;
};

// The u16 name-table hash: `hash = hash * 3 + byte`, wrapping in u16
// (Graffito's j3d_name_hash_encoded). Parsed files carry their stored hash
// verbatim through a round trip -- this helper is for CREATORS of new
// entries, so a future rename cannot ship a zeroed hash that silently stops
// the game from matching a material to its animation.
[[nodiscard]] std::uint16_t j3dNameHash(std::string_view name) noexcept;

// ---------------------------------------------------------------------------
// BCK -- joint (skeletal) animation, one entry per model joint.
// ---------------------------------------------------------------------------

struct BckJointTracks {
    std::array<AnimTrack, 3> scale;            // X, Y, Z
    std::array<AnimTrack, 3> rotationDegrees;  // X, Y, Z -- DEGREES
    std::array<AnimTrack, 3> translation;      // X, Y, Z

    [[nodiscard]] bool operator==(const BckJointTracks&) const = default;
};

struct BckAnimationFile {
    AnimFileHeader header;
    // Fixed-point multiplier for rotation values: raw * 2^fraction *
    // 180/32767 (Bck.java:54-56).
    std::uint8_t rotationFraction{0};
    int duration{0};  // last frame, in frames
    std::vector<BckJointTracks> joints;  // size() IS the file's joint count

    // Content + byte order only; see AnimFileHeader's note.
    [[nodiscard]] bool operator==(const BckAnimationFile& other) const {
        return rotationFraction == other.rotationFraction && duration == other.duration &&
               joints == other.joints && header.bigEndian == other.header.bigEndian;
    }
};

[[nodiscard]] BckAnimationFile parseBck(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> writeBck(const BckAnimationFile& file);

// One joint's transform at `frame`, rotation in radians. Scaled from the
// tracks exactly as BmdRenderer.java:1355-1367 applies them (rotation *
// PI/180 at that point and nowhere else).
struct BckJointPose {
    math::Vec3f scale{1.0F, 1.0F, 1.0F};
    math::Vec3f rotationRadians{};
    math::Vec3f translation{};
};

[[nodiscard]] std::vector<BckJointPose> sampleBckPoses(const BckAnimationFile& file,
                                                       float frame);

// ---------------------------------------------------------------------------
// BTK -- texture-matrix (UV) animation, one entry per material.
// ---------------------------------------------------------------------------

struct BtkMaterialAnimation {
    std::string materialName;
    std::uint16_t nameHash{0};   // stored verbatim; see j3dNameHash()
    int textureGeneratorId{0};   // texture generator in the model to target
    math::Vec3f center{};        // texture matrix centre
    std::array<AnimTrack, 3> scale;        // U, V, W
    std::array<AnimTrack, 3> rotation;     // U, V, W (BTK's own scale, no 180)
    std::array<AnimTrack, 3> translation;  // U, V, W

    [[nodiscard]] bool operator==(const BtkMaterialAnimation& other) const {
        return materialName == other.materialName && nameHash == other.nameHash &&
               textureGeneratorId == other.textureGeneratorId &&
               center.x == other.center.x && center.y == other.center.y &&
               center.z == other.center.z && scale == other.scale &&
               rotation == other.rotation && translation == other.translation;
    }
};

struct BtkAnimationFile {
    AnimFileHeader header;
    std::uint8_t rotationFraction{0};
    int duration{0};
    bool useMaya{false};  // Java reads it (Btk.java:74) and never acts on it
    std::vector<BtkMaterialAnimation> animations;
    // Material remap table: Java reads the offset and ignores the data
    // (Btk.java:63), but dropping it would hand the game a stale pointer, so
    // it is carried through. Empty = the offset is written 0 (absent).
    std::vector<std::uint16_t> materialRemap;

    // Content + byte order only; see AnimFileHeader's note.
    [[nodiscard]] bool operator==(const BtkAnimationFile& other) const {
        return rotationFraction == other.rotationFraction && duration == other.duration &&
               useMaya == other.useMaya && animations == other.animations &&
               materialRemap == other.materialRemap &&
               header.bigEndian == other.header.bigEndian;
    }
};

[[nodiscard]] BtkAnimationFile parseBtk(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> writeBtk(const BtkAnimationFile& file);

// First entry animating `materialName`, or null (the renderer's lookup path).
[[nodiscard]] const BtkMaterialAnimation* findBtkAnimation(const BtkAnimationFile& file,
                                                          std::string_view materialName);

// ---------------------------------------------------------------------------
// BRK -- TEV register colour animation, split into register and constant
// targets (Brk.java's Target enum).
// ---------------------------------------------------------------------------

enum class BrkTarget : std::uint8_t {
    reg,      // a TEV register (Java: REGISTER)
    constant, // a TEV konst colour (Java: CONSTANT)
};

struct BrkAnimation {
    std::string materialName;
    std::uint16_t nameHash{0};
    BrkTarget target{BrkTarget::reg};
    int targetValueId{0};  // which register the game writes this colour to
    std::array<AnimTrack, 4> rgba;  // Red, Green, Blue, Alpha

    [[nodiscard]] bool operator==(const BrkAnimation&) const = default;
};

struct BrkAnimationFile {
    AnimFileHeader header;
    int duration{0};
    // File order: every register entry (as the register table stores them),
    // then every constant entry. Which entries exist is decided by the
    // section's raw offset being 0 -- see parseBrk: Java checks
    // (raw + 0x20) != 0, which can never be false, so this port checks the
    // raw offset and treats 0 as "section absent" (the counts are zero in
    // every file where Java's check would have misfired anyway).
    std::vector<BrkAnimation> animations;
    // Remap tables (Java reads their offsets and ignores the data,
    // Brk.java:69-70). Empty = the section's offset is written 0 (absent).
    std::vector<std::uint16_t> registerRemap;
    std::vector<std::uint16_t> constantRemap;

    // Content + byte order only; see AnimFileHeader's note.
    [[nodiscard]] bool operator==(const BrkAnimationFile& other) const {
        return duration == other.duration && animations == other.animations &&
               registerRemap == other.registerRemap && constantRemap == other.constantRemap &&
               header.bigEndian == other.header.bigEndian;
    }
};

[[nodiscard]] BrkAnimationFile parseBrk(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> writeBrk(const BrkAnimationFile& file);

[[nodiscard]] const BrkAnimation* findBrkAnimation(const BrkAnimationFile& file,
                                                  std::string_view materialName,
                                                  BrkTarget target);

// ---------------------------------------------------------------------------
// BPK -- material colour animation per material.
// ---------------------------------------------------------------------------

struct BpkAnimation {
    std::string materialName;
    std::uint16_t nameHash{0};
    std::array<AnimTrack, 4> rgba;  // Red, Green, Blue, Alpha

    [[nodiscard]] bool operator==(const BpkAnimation&) const = default;
};

struct BpkAnimationFile {
    AnimFileHeader header;
    int duration{0};
    std::vector<BpkAnimation> animations;
    // See BtkAnimationFile::materialRemap.
    std::vector<std::uint16_t> materialRemap;

    // Content + byte order only; see AnimFileHeader's note.
    [[nodiscard]] bool operator==(const BpkAnimationFile& other) const {
        return duration == other.duration && animations == other.animations &&
               materialRemap == other.materialRemap &&
               header.bigEndian == other.header.bigEndian;
    }
};

[[nodiscard]] BpkAnimationFile parseBpk(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> writeBpk(const BpkAnimationFile& file);

[[nodiscard]] const BpkAnimation* findBpkAnimation(const BpkAnimationFile& file,
                                                  std::string_view materialName);

// ---------------------------------------------------------------------------
// BTP -- texture pattern (frame swap) animation: a flat list of frames per
// material, not a keyframed track (Btp.java).
// ---------------------------------------------------------------------------

struct BtpAnimation {
    std::string materialName;
    std::uint16_t nameHash{0};
    int textureId{0};                  // which texture slot it swaps
    std::vector<std::int16_t> frames;  // pattern index per frame

    [[nodiscard]] bool operator==(const BtpAnimation&) const = default;
};

struct BtpAnimationFile {
    AnimFileHeader header;
    std::vector<BtpAnimation> animations;
    // The table at offset 3 -- Java's unused fourth offset (Btp.java:51).
    // Carried for the same reason as BTK's remap: dropping data a loader may
    // read is not this writer's call. Empty = offset written 0.
    std::vector<std::uint16_t> materialRemap;

    // Content + byte order only; see AnimFileHeader's note.
    [[nodiscard]] bool operator==(const BtpAnimationFile& other) const {
        return animations == other.animations && materialRemap == other.materialRemap &&
               header.bigEndian == other.header.bigEndian;
    }
};

[[nodiscard]] BtpAnimationFile parseBtp(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> writeBtp(const BtpAnimationFile& file);

// The pattern index at `frameIndex` for (material, textureId): clamps the
// frame like Btp.java:86-97 and returns null when nothing matches.
[[nodiscard]] std::optional<std::int16_t> btpFrameAt(const BtpAnimationFile& file,
                                                    std::string_view materialName,
                                                    int textureId, int frameIndex);

// ---------------------------------------------------------------------------
// BVA -- batch visibility animation: one byte per frame per batch. Bva.java
// stores them as booleans; this keeps the raw byte so a round trip cannot
// flatten a value the game might distinguish from 0/1.
// ---------------------------------------------------------------------------

struct BvaAnimationFile {
    AnimFileHeader header;
    std::vector<std::vector<std::uint8_t>> batches;  // [batch][frame] -> 0/1

    // Content + byte order only; see AnimFileHeader's note.
    [[nodiscard]] bool operator==(const BvaAnimationFile& other) const {
        return batches == other.batches && header.bigEndian == other.header.bigEndian;
    }
};

[[nodiscard]] BvaAnimationFile parseBva(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> writeBva(const BvaAnimationFile& file);




} // namespace whitehole::smg
