#include "whitehole/smg/animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace whitehole::smg {
namespace {

using io::Endian;

// ---------------------------------------------------------------------------
// Format facts. Everything is chunk-relative unless stated otherwise; the
// reference reader uses chunkStart = 0x20 (Bck.java:64), and Java's absolute
// offsets line up with these once you add it. firstModelledField is where
// AnimFileHeader::prefix ends; fieldEnd is the first byte past every field
// this port models (reserved bytes between them stay in `fields`):
//   BCK  frac +9, duration +10, joint count +12, 3 bank counts,
//        4 offsets +0x14..+0x20, end +0x24.
//   BTK  frac +9, duration +10, table count (= 3 x materials) +12,
//        3 bank counts, 8 offsets +0x14..+0x30, useMaya +0x5C, end +0x60.
//   BRK  duration +0x0A, 10 counts +0x0C..+0x1E, 14 offsets +0x20..+0x54,
//        end +0x58.
//   BPK  duration +0x0C, anim count +0x0E, 4 bank counts +0x10..+0x16,
//        7 offsets +0x18..+0x30, end +0x34.
//   BTP  batch count +0x0C (reserved u16 at +0x0E stays in `fields`),
//        4 offsets +0x10..+0x1C, end +0x20.
//   BVA  batch count +0x0C, 2 offsets +0x10,+0x14, end +0x18.
constexpr std::size_t kStandardHeaderSize = 0x20;

// 'J3D1' big-endian, and the same magic as a little-endian file's u32 reads
// when fetched big-endian (the check Bck.java:50 performs, restated so both
// spellings are explicit).
constexpr std::uint32_t kMagicBigEndian = 0x4A334431U;
constexpr std::uint32_t kMagicLittleEndian = 0x3144334A;

constexpr float kDegreesToRadians = 3.14159265358979323846F / 180.0F;

// Bounds-checked random-access reader over the raw file, same shape as the
// one in bmd.cpp: every access names its own failure instead of reading past
// the buffer.
struct Reader {
    std::span<const std::uint8_t> data;
    Endian endian{Endian::big};

    void need(std::size_t position, std::size_t count) const {
        if (position > data.size() || count > data.size() - position) {
            throw std::runtime_error("animation: read past end of file at offset " +
                                     std::to_string(position));
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

    // NUL-terminated ASCII, bounded by the file (J3D name tables).
    std::string string(std::size_t position) const {
        std::string out;
        while (position < data.size() && data[position] != 0) {
            out.push_back(static_cast<char>(data[position]));
            ++position;
        }
        if (position >= data.size()) {
            throw std::runtime_error("animation: unterminated name in a name table");
        }
        return out;
    }
};

// The first four bytes decide the byte order: 'J3D1' written big-endian is
// a big-endian file; 'J3D1' written little-endian reads as 0x3144334A when
// fetched big-endian. Anything else is not a J3D animation -- Java accepts
// every input (it simply stays big-endian); this port refuses it loudly,
// because a wrong-endian parse of an arbitrary file only fails later, on a
// count, with a message that blames the wrong thing.
Endian sniffEndian(std::span<const std::uint8_t> data, const char* format) {
    if (data.size() < 4) {
        throw std::runtime_error(std::string(format) + ": file is too small to be a J3D animation");
    }
    const std::uint32_t big =
        (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
    if (big == kMagicBigEndian) {
        return Endian::big;
    }
    if (big == kMagicLittleEndian) {
        return Endian::little;
    }
    throw std::runtime_error(std::string(format) + ": not a J3D1 animation file (bad magic)");
}

// Where the data chunk starts: right after the standard 0x20 header, unless
// an SVR3 chunk sits in between (WindEditor's J3D Animation wiki). The
// reference reader hardcodes 0x20; SMG files never carry SVR3, so this is
// strictly more permissive and the tag check keeps the old behaviour.
std::size_t chunkStartOf(std::span<const std::uint8_t> data) {
    if (data.size() >= kStandardHeaderSize + 4 && data[0x20] == 'S' && data[0x21] == 'V' &&
        data[0x22] == 'R' && data[0x23] == '3') {
        return kStandardHeaderSize + 0x10;
    }
    return kStandardHeaderSize;
}

// A float that is allowed into a key: NaN or infinity in a bank becomes a
// NaN pose downstream (section 4's lesson), and a float time outside the
// int16 range would make that cast undefined -- both are malformed data and
// fail here, naming the track.
float checkedFloat(float value, const char* format, const char* what) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " contains a non-finite value");
    }
    return value;
}

std::int16_t timeFromTable(float value, const char* format, const char* what) {
    checkedFloat(value, format, what);
    if (value < -32768.0F || value > 32767.0F) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " has a key time outside the int16 range");
    }
    return static_cast<std::int16_t>(value); // truncates toward zero, as Java's (short) cast
}

// Java reads the rotation multiplier as a SIGNED byte (Bck.java:54,
// Btk.java:52); reading it unsigned would let 0x80.. make pow(2, n) infinite
// and poison every rotation. The raw byte is kept for the round trip; only
// the arithmetic uses the signed reading.
float bckRotationScale(std::uint8_t fraction) {
    const auto signedFraction = static_cast<std::int8_t>(fraction);
    return std::pow(2.0F, static_cast<float>(signedFraction)) * (180.0F / 32767.0F);
}

float btkRotationScale(std::uint8_t fraction) {
    const auto signedFraction = static_cast<std::int8_t>(fraction);
    return std::pow(2.0F, static_cast<float>(signedFraction)) / 0x7FFF;
}

// Captures AnimFileHeader's two preserved regions from a parsed file:
// prefix = [0, firstField), fields = [firstField, templateEnd), where
// templateEnd stretches at least to fieldEnd and, when the file already has
// tables, to the first one -- so padding between the header and the data
// rides along in `fields` instead of being silently dropped.
AnimFileHeader captureHeader(std::span<const std::uint8_t> data, Endian endian,
                             std::size_t firstField, std::size_t fieldEnd,
                             std::initializer_list<std::size_t> tableAbs,
                             const char* format) {
    if (data.size() < fieldEnd || fieldEnd < firstField) {
        throw std::runtime_error(std::string(format) + ": truncated header");
    }
    std::size_t templateEnd = fieldEnd;
    for (const std::size_t position : tableAbs) {
        templateEnd = std::max(templateEnd, position);
    }
    templateEnd = std::min(templateEnd, data.size()); // an offset past EOF: the table read names it

    AnimFileHeader header;
    header.prefix.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(firstField));
    header.fields.assign(data.begin() + static_cast<std::ptrdiff_t>(firstField),
                         data.begin() + static_cast<std::ptrdiff_t>(templateEnd));
    header.bigEndian = endian == Endian::big;
    return header;
}

// ---------------------------------------------------------------------------
// Write-side primitives. Every patch checks its own range: a model whose
// header bytes were never parsed (prefix/fields empty) throws here with a
// message that says so, instead of silently writing at position 0 of nothing.
// ---------------------------------------------------------------------------

void patchU8(std::vector<std::uint8_t>& out, std::size_t position, std::uint32_t value,
             const char* what) {
    if (value > 0xFFU) {
        throw std::runtime_error(std::string(what) + " does not fit the format's u8 field");
    }
    if (position >= out.size()) {
        throw std::runtime_error(std::string("animation header region is missing for ") + what +
                                 " (build write inputs through parse, or supply header bytes)");
    }
    out[position] = static_cast<std::uint8_t>(value);
}

void patchU16(std::vector<std::uint8_t>& out, std::size_t position, std::uint32_t value,
              Endian endian, const char* what) {
    if (value > 0xFFFFU) {
        throw std::runtime_error(std::string(what) + " does not fit the format's u16 field");
    }
    if (position + 2 > out.size()) {
        throw std::runtime_error(std::string("animation header region is missing for ") + what +
                                 " (build write inputs through parse, or supply header bytes)");
    }
    const auto low = static_cast<std::uint8_t>(value & 0xFFU);
    const auto high = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    if (endian == Endian::big) {
        out[position] = high;
        out[position + 1] = low;
    } else {
        out[position] = low;
        out[position + 1] = high;
    }
}

void patchU32(std::vector<std::uint8_t>& out, std::size_t position, std::uint32_t value,
              Endian endian, const char* what) {
    if (position + 4 > out.size()) {
        throw std::runtime_error(std::string("animation header region is missing for ") + what +
                                 " (build write inputs through parse, or supply header bytes)");
    }
    for (int index = 0; index < 4; ++index) {
        const int shift = endian == Endian::big ? 24 - index * 8 : index * 8;
        out[position + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value, Endian endian) {
    const auto low = static_cast<std::uint8_t>(value & 0xFFU);
    const auto high = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    if (endian == Endian::big) {
        out.push_back(high);
        out.push_back(low);
    } else {
        out.push_back(low);
        out.push_back(high);
    }
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value, Endian endian) {
    for (int index = 0; index < 4; ++index) {
        const int shift = endian == Endian::big ? 24 - index * 8 : index * 8;
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void appendF32(std::vector<std::uint8_t>& out, float value, Endian endian) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(out, bits, endian);
}

void alignOut(std::vector<std::uint8_t>& out) {
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
}

// Standard-header file size at 0x08 and chunk size at chunkStart+4, both
// recomputed on write (documented by WindEditor's standard header; Graffito
// notes loaders tolerate sloppy retail values, so writing the true one is
// always safe).
void patchSizes(std::vector<std::uint8_t>& out, Endian endian, const char* format) {
    if (out.size() > 0xFFFFFFFFU) {
        throw std::runtime_error(std::string(format) + ": file is too large to write");
    }
    const std::size_t chunkStart = chunkStartOf(out);
    const auto total = static_cast<std::uint32_t>(out.size());
    patchU32(out, 0x08, total, endian, "file size");
    patchU32(out, chunkStart + 4, static_cast<std::uint32_t>(out.size() - chunkStart), endian,
             "chunk size");
}

// ---------------------------------------------------------------------------
// Track decode/encode. The decode mirrors J3DAnimationTrack
// translateTrackOnLoad exactly: count 1 is a single value pinned at time 0
// with zero tangents; otherwise stride 3 (symmetric tangents: one value used
// as both in and out) or stride 4 for tangent mode 1.
// ---------------------------------------------------------------------------

AnimTrack decodeTrack(const Reader& reader, std::size_t position, std::span<const float> table,
                      float scale, const char* format, const char* what) {
    const std::uint16_t count = reader.u16(position);
    const std::uint16_t index = reader.u16(position + 2);
    const std::uint16_t mode = reader.u16(position + 4);
    if (count == 0) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " is a zero-length track (the reference refuses these too)");
    }

    AnimTrack track;
    if (count == 1) {
        if (index >= table.size()) {
            throw std::runtime_error(std::string(format) + ": " + what +
                                     " points past the end of its bank");
        }
        AnimKey key;
        key.time = 0; // Java: single-key tracks are pinned to frame 0 (J3DAnimationTrack:77)
        key.value = checkedFloat(table[index] * scale, format, what);
        track.keys.push_back(key);
        return track;
    }

    const std::size_t stride = mode == 1 ? 4U : 3U;
    const std::size_t needed = static_cast<std::size_t>(index) + stride * count;
    if (needed > table.size()) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " key data runs past the end of its bank");
    }
    track.keys.reserve(count);
    for (std::size_t key = 0; key < count; ++key) {
        const std::size_t base = static_cast<std::size_t>(index) + key * stride;
        AnimKey frame;
        frame.time = timeFromTable(table[base], format, what);
        frame.value = checkedFloat(table[base + 1] * scale, format, what);
        frame.inTangent = checkedFloat(table[base + 2] * scale, format, what);
        frame.outTangent =
            stride == 4 ? checkedFloat(table[base + 3] * scale, format, what) : frame.inTangent;
        track.keys.push_back(frame);
    }
    return track;
}

// The bank-side stores. Float banks take values verbatim; the rotation bank
// divides back through the scale and recovers the SAME s16 the file held
// (value = raw * scale on read, raw = value / scale on write, with the
// tolerance absorbing the single-ulp noise of that float round trip).
void storeTime(std::vector<float>& bank, std::int16_t time) {
    bank.push_back(static_cast<float>(time));
}

void storeTime(std::vector<std::int16_t>& bank, std::int16_t time) { bank.push_back(time); }

void storeValue(std::vector<float>& bank, float value, float /*scale*/, const char* format,
                const char* what) {
    bank.push_back(checkedFloat(value, format, what));
}

void storeValue(std::vector<std::int16_t>& bank, float value, float scale, const char* format,
                const char* what) {
    checkedFloat(value, format, what);
    const auto ratio = static_cast<double>(value) / static_cast<double>(scale);
    // 1e-3 of slack so a value read as raw * scale divides back to raw even
    // when float rounding puts it a hair outside the s16 range.
    if (!(ratio >= -32768.0 - 1e-3 && ratio <= 32767.0 + 1e-3)) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " value does not fit the rotation table");
    }
    const auto rounded = std::lround(ratio);
    const auto clamped = std::min<long>(32767L, std::max<long>(-32768L, rounded));
    bank.push_back(static_cast<std::int16_t>(clamped));
}

// One track's (count, index, mode) descriptor, appended into whichever bank
// the caller routes it to.
struct TrackEncoding {
    std::uint16_t count{0};
    std::uint16_t index{0};
    std::uint16_t mode{0};
};

template <typename Bank>
TrackEncoding encodeTrackInto(const AnimTrack& track, Bank& bank, float scale, const char* format,
                              const char* what) {
    if (track.keys.empty()) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " is empty; writing a track needs at least one key");
    }
    if (bank.size() > 0xFFFFU) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " starts past the bank's u16 index range");
    }
    const auto index = static_cast<std::uint16_t>(bank.size());

    if (track.keys.size() == 1) {
        // Format rule: a lone key carries only its value; time and tangents
        // come back as 0/0 (J3DAnimationTrack:75-78). Anything else the
        // caller put there cannot be represented -- the parse side can never
        // produce it, so a round trip starting from parse stays exact.
        storeValue(bank, track.keys.front().value, scale, format, what);
        return TrackEncoding{1, index, 0};
    }
    if (track.keys.size() > 0xFFFFU) {
        throw std::runtime_error(std::string(format) + ": " + what +
                                 " has more keys than the u16 count field holds");
    }

    std::uint16_t mode = 0;
    for (const AnimKey& key : track.keys) {
        if (key.outTangent != key.inTangent) {
            mode = 1; // piece-wise: both tangents stored
            break;
        }
    }
    for (const AnimKey& key : track.keys) {
        storeTime(bank, key.time);
        storeValue(bank, key.value, scale, format, what);
        storeValue(bank, key.inTangent, scale, format, what);
        if (mode == 1) {
            storeValue(bank, key.outTangent, scale, format, what);
        }
    }
    return TrackEncoding{static_cast<std::uint16_t>(track.keys.size()), index, mode};
}

// ---------------------------------------------------------------------------
// Key banks.
// ---------------------------------------------------------------------------

std::size_t tablePosition(std::size_t chunkStart, std::uint32_t relativeOffset,
                          std::size_t count, const char* format, const char* what) {
    if (relativeOffset == 0) {
        if (count > 0) {
            throw std::runtime_error(std::string(format) + ": " + what +
                                     " has data but a zero table offset");
        }
        return 0; // callers skip the read when the count is 0
    }
    return chunkStart + relativeOffset;
}

std::vector<float> readFloatBank(const Reader& reader, std::size_t position, std::size_t count,
                                 const char* format, const char* what) {
    std::vector<float> bank;
    if (count == 0) {
        return bank;
    }
    bank.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        bank.push_back(checkedFloat(reader.f32(position + index * 4), format, what));
    }
    return bank;
}

// Rotation banks are s16: every value is finite by construction, so there is
// nothing to check here (the scale arithmetic downstream is what could go
// wrong, and that is checked in storeValue/decodeTrack).
std::vector<float> readShortBank(const Reader& reader, std::size_t position, std::size_t count) {
    std::vector<float> bank;
    if (count == 0) {
        return bank;
    }
    bank.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        bank.push_back(static_cast<float>(reader.s16(position + index * 2)));
    }
    return bank;
}

void appendFloatBank(std::vector<std::uint8_t>& out, const std::vector<float>& bank, Endian endian) {
    for (const float value : bank) {
        appendF32(out, value, endian);
    }
}

void appendShortBank(std::vector<std::uint8_t>& out, const std::vector<std::int16_t>& bank,
                     Endian endian) {
    for (const std::int16_t value : bank) {
        appendU16(out, static_cast<std::uint16_t>(value), endian);
    }
}

// ---------------------------------------------------------------------------
// Name tables: u32 count, {u16 hash, u16 offset-from-table-start} entries,
// then NUL-terminated strings. The reference reader reads `count` entries
// from the CALLER (the animation count), never the stored u32 -- parity kept
// here; the u32 is still written so a game-side reader that does check it
// sees the truth.
// ---------------------------------------------------------------------------

struct NameEntryData {
    std::string name;
    std::uint16_t hash{0};
};

std::vector<NameEntryData> readNameTable(const Reader& reader, std::size_t tablePosition,
                                         std::size_t count, const char* format,
                                         const char* what) {
    std::vector<NameEntryData> entries;
    entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t entry = tablePosition + 4 + index * 4;
        const std::uint16_t hash = reader.u16(entry);
        const std::uint16_t relative = reader.u16(entry + 2);
        entries.push_back(NameEntryData{reader.string(tablePosition + relative), hash});
    }
    if (entries.size() != count) {
        throw std::runtime_error(std::string(format) + ": " + what + " name table is truncated");
    }
    return entries;
}

std::vector<std::uint8_t> buildNameTable(const std::vector<NameEntryData>& entries, Endian endian,
                                         const char* format, const char* what) {
    std::vector<std::uint8_t> table;
    appendU32(table, static_cast<std::uint32_t>(entries.size()), endian);
    table.resize(4 + entries.size() * 4, 0); // descriptor slots, filled below
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto offset = static_cast<std::uint32_t>(table.size());
        if (offset > 0xFFFFU) {
            throw std::runtime_error(std::string(format) + ": " + what +
                                     " name table exceeds the u16 offset range");
        }
        patchU16(table, 4 + index * 4, entries[index].hash, endian, "name hash");
        patchU16(table, 4 + index * 4 + 2, offset, endian, "name offset");
        for (const char character : entries[index].name) {
            table.push_back(static_cast<std::uint8_t>(character));
        }
        table.push_back(0);
    }
    return table;
}

} // namespace

// ---------------------------------------------------------------------------
// Evaluation (shared) and the BCK pair.
// ---------------------------------------------------------------------------

float AnimTrack::valueAt(float frame) const {
    if (keys.empty()) {
        return 0.0F; // Java parity: getValueAtFrame on an empty track returns 0
    }
    if (frame <= static_cast<float>(keys.front().time)) {
        return keys.front().value;
    }
    if (frame >= static_cast<float>(keys.back().time)) {
        return keys.back().value;
    }
    // First key whose time is STRICTLY greater than `frame` -- the same
    // search J3DAnimationTrack.getNextKeyframeIndex does.
    std::size_t next = 0;
    while (next < keys.size() && static_cast<float>(keys[next].time) <= frame) {
        ++next;
    }
    if (next == 0) {
        return keys.front().value;
    }
    if (next >= keys.size()) {
        return keys.back().value;
    }
    const AnimKey& left = keys[next - 1];
    const AnimKey& right = keys[next];
    const float length = static_cast<float>(right.time - left.time);
    if (length <= 0.0F) {
        // Two keys on the same frame: Java divides by zero here and returns
        // NaN. The later key wins instead -- a degenerate file should not be
        // able to make a whole pose invisible (section 4's NaN lesson).
        return right.value;
    }
    const float t = (frame - static_cast<float>(left.time)) / length;
    const float s0 = left.outTangent * length;
    const float s1 = right.inTangent * length;
    const float a = 2.0F * left.value - 2.0F * right.value + s0 + s1;
    const float b = -3.0F * left.value + 3.0F * right.value - 2.0F * s0 - s1;
    return ((a * t + b) * t + s0) * t + left.value;
}

std::uint16_t j3dNameHash(std::string_view name) noexcept {
    std::uint16_t hash = 0;
    for (const char character : name) {
        hash = static_cast<std::uint16_t>(static_cast<std::uint32_t>(hash) * 3U +
                                          static_cast<std::uint32_t>(
                                              static_cast<std::uint8_t>(character)));
    }
    return hash;
}

BckAnimationFile parseBck(std::span<const std::uint8_t> data) {
    static constexpr const char* kFormat = "BCK";
    const Endian endian = sniffEndian(data, kFormat);
    const Reader reader{data, endian};
    const std::size_t chunkStart = chunkStartOf(data);
    const std::size_t firstField = chunkStart + 0x09;
    const std::size_t fieldEnd = chunkStart + 0x24;
    if (data.size() < fieldEnd) {
        throw std::runtime_error("BCK: truncated header");
    }

    BckAnimationFile file;
    file.rotationFraction = reader.u8(chunkStart + 0x09);
    file.duration = reader.u16(chunkStart + 0x0A);
    const std::size_t jointCount = reader.u16(chunkStart + 0x0C);
    const std::size_t scaleCount = reader.u16(chunkStart + 0x0E);
    const std::size_t rotationCount = reader.u16(chunkStart + 0x10);
    const std::size_t translationCount = reader.u16(chunkStart + 0x12);
    const std::uint32_t animOffset = reader.u32(chunkStart + 0x14);
    const std::uint32_t scaleOffset = reader.u32(chunkStart + 0x18);
    const std::uint32_t rotationOffset = reader.u32(chunkStart + 0x1C);
    const std::uint32_t translationOffset = reader.u32(chunkStart + 0x20);

    file.header = captureHeader(
        data, endian, firstField, fieldEnd,
        {animOffset == 0 ? std::size_t{0} : chunkStart + animOffset,
         scaleOffset == 0 ? std::size_t{0} : chunkStart + scaleOffset,
         rotationOffset == 0 ? std::size_t{0} : chunkStart + rotationOffset,
         translationOffset == 0 ? std::size_t{0} : chunkStart + translationOffset},
        kFormat);

    const std::vector<float> scaleBank =
        readFloatBank(reader, tablePosition(chunkStart, scaleOffset, scaleCount, kFormat, "scale bank"),
                      scaleCount, kFormat, "scale bank");
    const std::vector<float> rotationBank =
        readShortBank(reader, tablePosition(chunkStart, rotationOffset, rotationCount, kFormat,
                                            "rotation bank"),
                      rotationCount);
    const std::vector<float> translationBank = readFloatBank(
        reader, tablePosition(chunkStart, translationOffset, translationCount, kFormat,
                              "translation bank"),
        translationCount, kFormat, "translation bank");

    const std::size_t animTable =
        tablePosition(chunkStart, animOffset, jointCount, kFormat, "joint animation table");
    const float rotationScale = bckRotationScale(file.rotationFraction);
    file.joints.resize(jointCount);
    for (std::size_t index = 0; index < jointCount; ++index) {
        const std::size_t entry = animTable + index * 0x36;
        BckJointTracks& joint = file.joints[index];
        joint.scale[0] = decodeTrack(reader, entry + 0x00, scaleBank, 1.0F, kFormat, "scale X");
        joint.rotationDegrees[0] =
            decodeTrack(reader, entry + 0x06, rotationBank, rotationScale, kFormat, "rotation X");
        joint.translation[0] =
            decodeTrack(reader, entry + 0x0C, translationBank, 1.0F, kFormat, "translation X");
        joint.scale[1] = decodeTrack(reader, entry + 0x12, scaleBank, 1.0F, kFormat, "scale Y");
        joint.rotationDegrees[1] =
            decodeTrack(reader, entry + 0x18, rotationBank, rotationScale, kFormat, "rotation Y");
        joint.translation[1] =
            decodeTrack(reader, entry + 0x1E, translationBank, 1.0F, kFormat, "translation Y");
        joint.scale[2] = decodeTrack(reader, entry + 0x24, scaleBank, 1.0F, kFormat, "scale Z");
        joint.rotationDegrees[2] =
            decodeTrack(reader, entry + 0x2A, rotationBank, rotationScale, kFormat, "rotation Z");
        joint.translation[2] =
            decodeTrack(reader, entry + 0x30, translationBank, 1.0F, kFormat, "translation Z");
    }
    return file;
}

std::vector<std::uint8_t> writeBck(const BckAnimationFile& file) {
    static constexpr const char* kFormat = "BCK";
    const Endian endian = file.header.bigEndian ? Endian::big : Endian::little;
    const float rotationScale = bckRotationScale(file.rotationFraction);

    // 1) Encode every track into its bank first: the animation table only
    //    needs (count, index, mode) descriptors, so banks can be laid out
    //    afterwards in any canonical order.
    struct JointEncodings {
        TrackEncoding scale[3];
        TrackEncoding rotation[3];
        TrackEncoding translation[3];
    };
    std::vector<float> scaleBank;
    std::vector<float> translationBank;
    std::vector<std::int16_t> rotationBank;
    std::vector<JointEncodings> encodings;
    encodings.reserve(file.joints.size());
    for (const BckJointTracks& joint : file.joints) {
        JointEncodings encoding;
        for (int axis = 0; axis < 3; ++axis) {
            const std::string channel = axis == 0 ? "X" : (axis == 1 ? "Y" : "Z");
            encoding.scale[axis] = encodeTrackInto(joint.scale[static_cast<std::size_t>(axis)],
                                                   scaleBank, 1.0F, kFormat,
                                                   ("scale " + channel).c_str());
            encoding.rotation[axis] =
                encodeTrackInto(joint.rotationDegrees[static_cast<std::size_t>(axis)],
                                rotationBank, rotationScale, kFormat,
                                ("rotation " + channel).c_str());
            encoding.translation[axis] = encodeTrackInto(
                joint.translation[static_cast<std::size_t>(axis)], translationBank, 1.0F, kFormat,
                ("translation " + channel).c_str());
        }
        encodings.push_back(encoding);
    }

    // 2) Header regions verbatim, then the data region in canonical order:
    //    animation table, scale bank, rotation bank, translation bank.
    std::vector<std::uint8_t> out;
    out.reserve(file.header.prefix.size() + file.header.fields.size() + 1024);
    out.insert(out.end(), file.header.prefix.begin(), file.header.prefix.end());
    out.insert(out.end(), file.header.fields.begin(), file.header.fields.end());
    alignOut(out);

    const std::size_t animAbs = out.size();
    for (const JointEncodings& encoding : encodings) {
        for (int axis = 0; axis < 3; ++axis) {
            // Java's read order per joint: SX, RX, TX, SY, RY, TY, SZ, RZ, TZ.
            const auto append = [&out, endian](const TrackEncoding& track) {
                appendU16(out, track.count, endian);
                appendU16(out, track.index, endian);
                appendU16(out, track.mode, endian);
            };
            append(encoding.scale[static_cast<std::size_t>(axis)]);
            append(encoding.rotation[static_cast<std::size_t>(axis)]);
            append(encoding.translation[static_cast<std::size_t>(axis)]);
        }
    }
    alignOut(out);
    const std::size_t scaleAbs = out.size();
    appendFloatBank(out, scaleBank, endian);
    alignOut(out);
    const std::size_t rotationAbs = out.size();
    appendShortBank(out, rotationBank, endian);
    alignOut(out);
    const std::size_t translationAbs = out.size();
    appendFloatBank(out, translationBank, endian);

    // 3) Patch the modelled fields over the preserved copy, then the sizes.
    const std::size_t chunkStart = chunkStartOf(out);
    const auto relative = [chunkStart](std::size_t position) -> std::uint32_t {
        const std::size_t offset = position - chunkStart;
        if (offset > 0xFFFFFFFFU) {
            throw std::runtime_error("animation: file is too large to write");
        }
        return static_cast<std::uint32_t>(offset);
    };
    // Zero joints means no tables at all: every offset is written 0, the
    // "section absent" spelling (see BrkAnimationFile for why raw 0 matters).
    const bool haveData = !file.joints.empty();

    patchU8(out, chunkStart + 0x09, file.rotationFraction, "BCK rotation fraction");
    patchU16(out, chunkStart + 0x0A, static_cast<std::uint32_t>(file.duration), endian,
             "BCK duration");
    patchU16(out, chunkStart + 0x0C, static_cast<std::uint32_t>(file.joints.size()), endian,
             "BCK joint count");
    patchU16(out, chunkStart + 0x0E, static_cast<std::uint32_t>(scaleBank.size()), endian,
             "BCK scale bank count");
    patchU16(out, chunkStart + 0x10, static_cast<std::uint32_t>(rotationBank.size()), endian,
             "BCK rotation bank count");
    patchU16(out, chunkStart + 0x12, static_cast<std::uint32_t>(translationBank.size()), endian,
             "BCK translation bank count");
    patchU32(out, chunkStart + 0x14, haveData ? relative(animAbs) : 0U, endian, "BCK table offset");
    patchU32(out, chunkStart + 0x18, haveData ? relative(scaleAbs) : 0U, endian, "BCK table offset");
    patchU32(out, chunkStart + 0x1C, haveData ? relative(rotationAbs) : 0U, endian,
             "BCK table offset");
    patchU32(out, chunkStart + 0x20, haveData ? relative(translationAbs) : 0U, endian,
             "BCK table offset");
    patchSizes(out, endian, kFormat);
    return out;
}

// ---------------------------------------------------------------------------
// BTK: one 0x36-byte entry per material (nine tracks laid out as
// Scale/Rotation/Translation triplets for U, then V, then W), plus a
// texture-generator id and a centre per material, over shared key banks.
// ---------------------------------------------------------------------------

BtkAnimationFile parseBtk(std::span<const std::uint8_t> data) {
    static constexpr const char* kFormat = "BTK";
    const Endian endian = sniffEndian(data, kFormat);
    const Reader reader{data, endian};
    const std::size_t chunkStart = chunkStartOf(data);
    const std::size_t firstField = chunkStart + 0x09;
    const std::size_t fieldEnd = chunkStart + 0x60;
    if (data.size() < fieldEnd) {
        throw std::runtime_error("BTK: truncated header");
    }

    BtkAnimationFile file;
    file.rotationFraction = reader.u8(chunkStart + 0x09);
    file.duration = reader.u16(chunkStart + 0x0A);
    const std::size_t tableCount = reader.u16(chunkStart + 0x0C);
    if (tableCount % 3 != 0) {
        // The stored count is one transform table PER AXIS (U, V, W) per
        // material -- what Java's (short)(n / 3) at Btk.java:56 divides
        // out. A non-multiple cannot name whole materials.
        throw std::runtime_error("BTK: animation table count is not a multiple of 3");
    }
    const std::size_t materialCount = tableCount / 3;
    const std::size_t scaleCount = reader.u16(chunkStart + 0x0E);
    const std::size_t rotationCount = reader.u16(chunkStart + 0x10);
    const std::size_t translationCount = reader.u16(chunkStart + 0x12);
    const std::uint32_t animOffset = reader.u32(chunkStart + 0x14);
    const std::uint32_t remapOffset = reader.u32(chunkStart + 0x18);
    const std::uint32_t namesOffset = reader.u32(chunkStart + 0x1C);
    const std::uint32_t generatorOffset = reader.u32(chunkStart + 0x20);
    const std::uint32_t centerOffset = reader.u32(chunkStart + 0x24);
    const std::uint32_t scaleOffset = reader.u32(chunkStart + 0x28);
    const std::uint32_t rotationOffset = reader.u32(chunkStart + 0x2C);
    const std::uint32_t translationOffset = reader.u32(chunkStart + 0x30);
    file.useMaya = reader.u32(chunkStart + 0x5C) == 1;

    const auto absIfPresent = [chunkStart](std::uint32_t relative) -> std::size_t {
        return relative == 0 ? std::size_t{0} : chunkStart + relative;
    };
    file.header = captureHeader(
        data, endian, firstField, fieldEnd,
        {absIfPresent(animOffset), absIfPresent(remapOffset), absIfPresent(namesOffset),
         absIfPresent(generatorOffset), absIfPresent(centerOffset), absIfPresent(scaleOffset),
         absIfPresent(rotationOffset), absIfPresent(translationOffset)},
        kFormat);

    const std::vector<float> scaleBank = readFloatBank(
        reader, tablePosition(chunkStart, scaleOffset, scaleCount, kFormat, "scale bank"),
        scaleCount, kFormat, "scale bank");
    const std::vector<float> rotationBank =
        readShortBank(reader, tablePosition(chunkStart, rotationOffset, rotationCount, kFormat,
                                            "rotation bank"),
                      rotationCount);
    const std::vector<float> translationBank = readFloatBank(
        reader, tablePosition(chunkStart, translationOffset, translationCount, kFormat,
                              "translation bank"),
        translationCount, kFormat, "translation bank");

    const std::size_t namesTable =
        tablePosition(chunkStart, namesOffset, materialCount, kFormat, "material name table");
    const std::vector<NameEntryData> names =
        readNameTable(reader, namesTable, materialCount, kFormat, "material names");
    // Remap: no count of its own -- one entry per material (Graffito's
    // material_remap). Absent offset = absent table, not an error: Java
    // never reads it either.
    if (remapOffset != 0) {
        const std::size_t remapTable = chunkStart + remapOffset;
        file.materialRemap.reserve(materialCount);
        for (std::size_t index = 0; index < materialCount; ++index) {
            file.materialRemap.push_back(reader.u16(remapTable + index * 2));
        }
    }
    const std::size_t generatorTable = tablePosition(
        chunkStart, generatorOffset, materialCount, kFormat, "texture generator ids");
    const std::size_t centersTable =
        tablePosition(chunkStart, centerOffset, materialCount, kFormat, "texture centres");
    const std::size_t animTable =
        tablePosition(chunkStart, animOffset, materialCount, kFormat, "material animation table");

    const float rotationScale = btkRotationScale(file.rotationFraction);
    file.animations.reserve(materialCount);
    for (std::size_t index = 0; index < materialCount; ++index) {
        BtkMaterialAnimation animation;
        animation.materialName = names[index].name;
        animation.nameHash = names[index].hash;
        animation.textureGeneratorId = reader.u8(generatorTable + index);
        const std::size_t center = centersTable + index * 0x0C;
        animation.center = {reader.f32(center), reader.f32(center + 4), reader.f32(center + 8)};

        const std::size_t entry = animTable + index * 0x36;
        for (int axis = 0; axis < 3; ++axis) {
            const std::string channel = axis == 0 ? "U" : (axis == 1 ? "V" : "W");
            const std::size_t base = entry + static_cast<std::size_t>(axis) * 0x12;
            // Java's per-axis read order: Scale, Rotation, Translation.
            animation.scale[static_cast<std::size_t>(axis)] =
                decodeTrack(reader, base, scaleBank, 1.0F, kFormat, ("scale " + channel).c_str());
            animation.rotation[static_cast<std::size_t>(axis)] =
                decodeTrack(reader, base + 6, rotationBank, rotationScale, kFormat,
                            ("rotation " + channel).c_str());
            animation.translation[static_cast<std::size_t>(axis)] =
                decodeTrack(reader, base + 12, translationBank, 1.0F, kFormat,
                            ("translation " + channel).c_str());
        }
        file.animations.push_back(std::move(animation));
    }
    return file;
}

std::vector<std::uint8_t> writeBtk(const BtkAnimationFile& file) {
    static constexpr const char* kFormat = "BTK";
    const Endian endian = file.header.bigEndian ? Endian::big : Endian::little;
    const float rotationScale = btkRotationScale(file.rotationFraction);

    struct MaterialEncodings {
        TrackEncoding scale[3];
        TrackEncoding rotation[3];
        TrackEncoding translation[3];
    };
    std::vector<float> scaleBank;
    std::vector<float> translationBank;
    std::vector<std::int16_t> rotationBank;
    std::vector<MaterialEncodings> encodings;
    encodings.reserve(file.animations.size());
    for (const BtkMaterialAnimation& animation : file.animations) {
        MaterialEncodings encoding;
        for (int axis = 0; axis < 3; ++axis) {
            const std::string channel = axis == 0 ? "U" : (axis == 1 ? "V" : "W");
            encoding.scale[axis] = encodeTrackInto(animation.scale[static_cast<std::size_t>(axis)],
                                                   scaleBank, 1.0F, kFormat,
                                                   ("scale " + channel).c_str());
            encoding.rotation[axis] =
                encodeTrackInto(animation.rotation[static_cast<std::size_t>(axis)], rotationBank,
                                rotationScale, kFormat, ("rotation " + channel).c_str());
            encoding.translation[axis] = encodeTrackInto(
                animation.translation[static_cast<std::size_t>(axis)], translationBank, 1.0F,
                kFormat, ("translation " + channel).c_str());
        }
        encodings.push_back(encoding);
    }
    std::vector<NameEntryData> names;
    names.reserve(file.animations.size());
    for (const BtkMaterialAnimation& animation : file.animations) {
        names.push_back(NameEntryData{animation.materialName, animation.nameHash});
    }

    // Data region in canonical order: entries, remap, names, generator ids,
    // centres, then the three key banks.
    std::vector<std::uint8_t> out;
    out.reserve(file.header.prefix.size() + file.header.fields.size() + 1024);
    out.insert(out.end(), file.header.prefix.begin(), file.header.prefix.end());
    out.insert(out.end(), file.header.fields.begin(), file.header.fields.end());
    alignOut(out);

    const std::size_t animAbs = out.size();
    for (const MaterialEncodings& encoding : encodings) {
        for (int axis = 0; axis < 3; ++axis) {
            const auto append = [&out, endian](const TrackEncoding& track) {
                appendU16(out, track.count, endian);
                appendU16(out, track.index, endian);
                appendU16(out, track.mode, endian);
            };
            append(encoding.scale[static_cast<std::size_t>(axis)]);
            append(encoding.rotation[static_cast<std::size_t>(axis)]);
            append(encoding.translation[static_cast<std::size_t>(axis)]);
        }
    }
    const bool haveData = !file.animations.empty();
    alignOut(out);
    const std::size_t remapAbs = out.size();
    if (haveData && !file.materialRemap.empty()) {
        for (const std::uint16_t entry : file.materialRemap) {
            appendU16(out, entry, endian);
        }
    }
    alignOut(out);
    const std::size_t namesAbs = out.size();
    if (haveData) {
        const std::vector<std::uint8_t> table = buildNameTable(names, endian, kFormat, "BTK");
        out.insert(out.end(), table.begin(), table.end());
    }
    alignOut(out);
    const std::size_t generatorAbs = out.size();
    for (const BtkMaterialAnimation& animation : file.animations) {
        if (animation.textureGeneratorId < 0 || animation.textureGeneratorId > 0xFF) {
            throw std::runtime_error("BTK: texture generator id does not fit the u8 field");
        }
        out.push_back(static_cast<std::uint8_t>(animation.textureGeneratorId));
    }
    alignOut(out);
    const std::size_t centersAbs = out.size();
    for (const BtkMaterialAnimation& animation : file.animations) {
        appendF32(out, animation.center.x, endian);
        appendF32(out, animation.center.y, endian);
        appendF32(out, animation.center.z, endian);
    }

    alignOut(out);
    const std::size_t scaleAbs = out.size();
    appendFloatBank(out, scaleBank, endian);
    alignOut(out);
    const std::size_t rotationAbs = out.size();
    appendShortBank(out, rotationBank, endian);
    alignOut(out);
    const std::size_t translationAbs = out.size();
    appendFloatBank(out, translationBank, endian);

    const std::size_t chunkStart = chunkStartOf(out);
    const auto relative = [chunkStart](std::size_t position) -> std::uint32_t {
        const std::size_t offset = position - chunkStart;
        if (offset > 0xFFFFFFFFU) {
            throw std::runtime_error("animation: file is too large to write");
        }
        return static_cast<std::uint32_t>(offset);
    };
    const auto offset = [&](std::size_t position) -> std::uint32_t {
        return haveData ? relative(position) : 0U;
    };

    patchU8(out, chunkStart + 0x09, file.rotationFraction, "BTK rotation fraction");
    patchU16(out, chunkStart + 0x0A, static_cast<std::uint32_t>(file.duration), endian,
             "BTK duration");
    patchU16(out, chunkStart + 0x0C, static_cast<std::uint32_t>(file.animations.size()) * 3U,
             endian, "BTK transform table count");
    patchU16(out, chunkStart + 0x0E, static_cast<std::uint32_t>(scaleBank.size()), endian,
             "BTK scale bank count");
    patchU16(out, chunkStart + 0x10, static_cast<std::uint32_t>(rotationBank.size()), endian,
             "BTK rotation bank count");
    patchU16(out, chunkStart + 0x12, static_cast<std::uint32_t>(translationBank.size()), endian,
             "BTK translation bank count");
    patchU32(out, chunkStart + 0x14, offset(animAbs), endian, "BTK animation offset");
    patchU32(out, chunkStart + 0x18,
             (haveData && !file.materialRemap.empty()) ? relative(remapAbs) : 0U, endian,
             "BTK remap offset");
    patchU32(out, chunkStart + 0x1C, offset(namesAbs), endian, "BTK name table offset");
    patchU32(out, chunkStart + 0x20, offset(generatorAbs), endian, "BTK generator id offset");
    patchU32(out, chunkStart + 0x24, offset(centersAbs), endian, "BTK centre offset");
    patchU32(out, chunkStart + 0x28, offset(scaleAbs), endian, "BTK scale bank offset");
    patchU32(out, chunkStart + 0x2C, offset(rotationAbs), endian, "BTK rotation bank offset");
    patchU32(out, chunkStart + 0x30, offset(translationAbs), endian, "BTK translation bank offset");
    patchU32(out, chunkStart + 0x5C, file.useMaya ? 1U : 0U, endian, "BTK useMaya flag");
    patchSizes(out, endian, kFormat);
    return out;
}

const BtkMaterialAnimation* findBtkAnimation(const BtkAnimationFile& file,
                                             std::string_view materialName) {
    for (const BtkMaterialAnimation& animation : file.animations) {
        if (animation.materialName == materialName) {
            return &animation;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// BRK: two parallel sections (TEV registers and konst colours), each with
// its own animation table, remap, name table and four s16 banks. A section
// whose ANIMATION offset is raw 0 is absent -- see BrkAnimationFile.
// ---------------------------------------------------------------------------

BrkAnimationFile parseBrk(std::span<const std::uint8_t> data) {
    static constexpr const char* kFormat = "BRK";
    const Endian endian = sniffEndian(data, kFormat);
    const Reader reader{data, endian};
    const std::size_t chunkStart = chunkStartOf(data);
    const std::size_t firstField = chunkStart + 0x0A;
    const std::size_t fieldEnd = chunkStart + 0x58;
    if (data.size() < fieldEnd) {
        throw std::runtime_error("BRK: truncated header");
    }

    BrkAnimationFile file;
    file.duration = reader.u16(chunkStart + 0x0A);
    const std::size_t registerCount = reader.u16(chunkStart + 0x0C);
    const std::size_t constantCount = reader.u16(chunkStart + 0x0E);
    const std::size_t bankCounts[2][4] = {
        {reader.u16(chunkStart + 0x10), reader.u16(chunkStart + 0x12),
         reader.u16(chunkStart + 0x14), reader.u16(chunkStart + 0x16)},
        {reader.u16(chunkStart + 0x18), reader.u16(chunkStart + 0x1A),
         reader.u16(chunkStart + 0x1C), reader.u16(chunkStart + 0x1E)},
    };
    std::uint32_t offsets[14];
    for (std::size_t index = 0; index < 14; ++index) {
        offsets[index] = reader.u32(chunkStart + 0x20 + index * 4);
    }
    // Offset slots: 0 regAnim, 1 conAnim, 2 regRemap, 3 conRemap, 4 regNames,
    // 5 conNames, 6-9 regRGBA banks, 10-13 conRGBA banks (Java's read order).
    const bool hasRegister = offsets[0] != 0;
    const bool hasConstant = offsets[1] != 0;
    // A section that declares entries must have its table: skipping it would
    // silently drop rows the counts promise. (Java's own presence check --
    // (raw + 0x20) != 0 -- can never be false, so it would read garbage
    // instead; see BrkAnimationFile.)
    if (registerCount != 0 && !hasRegister) {
        throw std::runtime_error("BRK: the register section declares entries but has no table");
    }
    if (constantCount != 0 && !hasConstant) {
        throw std::runtime_error("BRK: the constant section declares entries but has no table");
    }

    const auto absIfPresent = [chunkStart](std::uint32_t relative) -> std::size_t {
        return relative == 0 ? std::size_t{0} : chunkStart + relative;
    };
    file.header = captureHeader(
        data, endian, firstField, fieldEnd,
        {absIfPresent(offsets[0]), absIfPresent(offsets[1]), absIfPresent(offsets[2]),
         absIfPresent(offsets[3]), absIfPresent(offsets[4]), absIfPresent(offsets[5]),
         absIfPresent(offsets[6]), absIfPresent(offsets[7]), absIfPresent(offsets[8]),
         absIfPresent(offsets[9]), absIfPresent(offsets[10]), absIfPresent(offsets[11]),
         absIfPresent(offsets[12]), absIfPresent(offsets[13])},
        kFormat);

    for (int section = 0; section < 2; ++section) {
        const bool isRegister = section == 0;
        const std::size_t entryCount = isRegister ? registerCount : constantCount;
        if (entryCount == 0) {
            continue;
        }
        const std::size_t bankBase = isRegister ? 6 : 10;
        const std::string sectionName = isRegister ? "register" : "constant";
        std::vector<float> banks[4];
        static constexpr const char* kChannelNames[4] = {"red", "green", "blue", "alpha"};
        for (int channel = 0; channel < 4; ++channel) {
            banks[channel] = readShortBank(
                reader,
                tablePosition(chunkStart, offsets[bankBase + static_cast<std::size_t>(channel)],
                              bankCounts[section][channel], kFormat,
                              (sectionName + " " + kChannelNames[channel] + " bank").c_str()),
                bankCounts[section][channel]);
        }
        const std::vector<NameEntryData> names =
            readNameTable(reader,
                          tablePosition(chunkStart, offsets[isRegister ? 4 : 5], entryCount,
                                        kFormat, (sectionName + " name table").c_str()),
                          entryCount, kFormat, (sectionName + " names").c_str());
        const std::size_t remapSlot = isRegister ? 2 : 3;
        std::vector<std::uint16_t>& remap =
            isRegister ? file.registerRemap : file.constantRemap;
        if (offsets[remapSlot] != 0) {
            const std::size_t remapTable = chunkStart + offsets[remapSlot];
            remap.reserve(entryCount);
            for (std::size_t index = 0; index < entryCount; ++index) {
                remap.push_back(reader.u16(remapTable + index * 2));
            }
        }
        const std::size_t animTable =
            tablePosition(chunkStart, offsets[0 + (isRegister ? 0 : 1)], entryCount, kFormat,
                          (sectionName + " animation table").c_str());
        for (std::size_t index = 0; index < entryCount; ++index) {
            const std::size_t entry = animTable + index * 0x1C;
            BrkAnimation animation;
            animation.materialName = names[index].name;
            animation.nameHash = names[index].hash;
            animation.target = isRegister ? BrkTarget::reg : BrkTarget::constant;
            for (int channel = 0; channel < 4; ++channel) {
                animation.rgba[static_cast<std::size_t>(channel)] =
                    decodeTrack(reader, entry + static_cast<std::size_t>(channel) * 6,
                                banks[channel], 1.0F, kFormat,
                                (sectionName + " " + kChannelNames[channel]).c_str());
            }
            animation.targetValueId = reader.u8(entry + 24);
            file.animations.push_back(std::move(animation));
        }
    }
    return file;
}

std::vector<std::uint8_t> writeBrk(const BrkAnimationFile& file) {
    static constexpr const char* kFormat = "BRK";
    const Endian endian = file.header.bigEndian ? Endian::big : Endian::little;

    // Split by target, preserving model order inside each section (parse
    // always yields registers first, so parse -> write is stable).
    std::vector<const BrkAnimation*> sections[2];
    for (const BrkAnimation& animation : file.animations) {
        sections[animation.target == BrkTarget::reg ? 0 : 1].push_back(&animation);
    }
    const bool haveRegister = !sections[0].empty();
    const bool haveConstant = !sections[1].empty();

    struct SectionEncodings {
        TrackEncoding rgba[4];
    };
    static constexpr const char* kChannelNames[4] = {"red", "green", "blue", "alpha"};
    std::vector<std::int16_t> banks[2][4];
    std::vector<SectionEncodings> encodings[2];
    std::vector<NameEntryData> names[2];
    for (int section = 0; section < 2; ++section) {
        const std::string sectionName = section == 0 ? "register" : "constant";
        encodings[section].reserve(sections[section].size());
        names[section].reserve(sections[section].size());
        for (const BrkAnimation* animation : sections[section]) {
            SectionEncodings encoding;
            for (int channel = 0; channel < 4; ++channel) {
                encoding.rgba[static_cast<std::size_t>(channel)] = encodeTrackInto(
                    animation->rgba[static_cast<std::size_t>(channel)],
                    banks[section][channel], 1.0F, kFormat,
                    (sectionName + " " + kChannelNames[channel]).c_str());
            }
            encodings[section].push_back(encoding);
            names[section].push_back(NameEntryData{animation->materialName, animation->nameHash});
        }
    }

    // Data region in Java's offset order: register animations, constant
    // animations, both remaps, both name tables, then the eight banks.
    std::vector<std::uint8_t> out;
    out.reserve(file.header.prefix.size() + file.header.fields.size() + 1024);
    out.insert(out.end(), file.header.prefix.begin(), file.header.prefix.end());
    out.insert(out.end(), file.header.fields.begin(), file.header.fields.end());
    alignOut(out);

    const auto appendSection = [&out, endian](int section,
                                              const std::vector<SectionEncodings>& encodings,
                                              const std::vector<const BrkAnimation*>& entries) {
        for (std::size_t index = 0; index < encodings.size(); ++index) {
            for (int channel = 0; channel < 4; ++channel) {
                const TrackEncoding& track = encodings[index].rgba[static_cast<std::size_t>(channel)];
                appendU16(out, track.count, endian);
                appendU16(out, track.index, endian);
                appendU16(out, track.mode, endian);
            }
            const int targetValueId = entries[index]->targetValueId;
            if (targetValueId < 0 || targetValueId > 0xFF) {
                throw std::runtime_error(std::string(kFormat) + ": target value id does not fit");
            }
            out.push_back(static_cast<std::uint8_t>(targetValueId));
            out.push_back(0); // three bytes the reference skips (stride 0x1C)
            out.push_back(0);
            out.push_back(0);
        }
        (void)section;
    };

    const std::size_t registerAnimAbs = out.size();
    if (haveRegister) {
        appendSection(0, encodings[0], sections[0]);
    }
    alignOut(out);
    const std::size_t constantAnimAbs = out.size();
    if (haveConstant) {
        appendSection(1, encodings[1], sections[1]);
    }
    alignOut(out);
    const std::size_t registerRemapAbs = out.size();
    if (haveRegister && !file.registerRemap.empty()) {
        for (const std::uint16_t entry : file.registerRemap) {
            appendU16(out, entry, endian);
        }
    }
    alignOut(out);
    const std::size_t constantRemapAbs = out.size();
    if (haveConstant && !file.constantRemap.empty()) {
        for (const std::uint16_t entry : file.constantRemap) {
            appendU16(out, entry, endian);
        }
    }
    alignOut(out);
    const std::size_t registerNamesAbs = out.size();
    if (haveRegister) {
        const std::vector<std::uint8_t> table =
            buildNameTable(names[0], endian, kFormat, "BRK register names");
        out.insert(out.end(), table.begin(), table.end());
    }
    alignOut(out);
    const std::size_t constantNamesAbs = out.size();
    if (haveConstant) {
        const std::vector<std::uint8_t> table =
            buildNameTable(names[1], endian, kFormat, "BRK constant names");
        out.insert(out.end(), table.begin(), table.end());
    }
    std::size_t bankAbs[2][4] = {};
    for (int section = 0; section < 2; ++section) {
        for (int channel = 0; channel < 4; ++channel) {
            alignOut(out);
            bankAbs[section][channel] = out.size();
            appendShortBank(out, banks[section][channel], endian);
        }
    }

    const std::size_t chunkStart = chunkStartOf(out);
    const auto relative = [chunkStart](std::size_t position) -> std::uint32_t {
        const std::size_t offset = position - chunkStart;
        if (offset > 0xFFFFFFFFU) {
            throw std::runtime_error("animation: file is too large to write");
        }
        return static_cast<std::uint32_t>(offset);
    };
    const auto sectionOffset = [&](int section, std::size_t position) -> std::uint32_t {
        const bool present = section == 0 ? haveRegister : haveConstant;
        return present ? relative(position) : 0U;
    };
    const auto remapOffset = [&](int section, bool nonEmpty, std::size_t position) -> std::uint32_t {
        const bool present = section == 0 ? haveRegister : haveConstant;
        return present && nonEmpty ? relative(position) : 0U;
    };

    patchU16(out, chunkStart + 0x0A, static_cast<std::uint32_t>(file.duration), endian,
             "BRK duration");
    patchU16(out, chunkStart + 0x0C, static_cast<std::uint32_t>(sections[0].size()), endian,
             "BRK register count");
    patchU16(out, chunkStart + 0x0E, static_cast<std::uint32_t>(sections[1].size()), endian,
             "BRK constant count");
    for (int section = 0; section < 2; ++section) {
        for (int channel = 0; channel < 4; ++channel) {
            patchU16(out, chunkStart + 0x10 + (section * 4 + channel) * 2,
                     static_cast<std::uint32_t>(banks[section][channel].size()), endian,
                     "BRK bank count");
        }
    }
    const std::uint32_t offsets[14] = {
        sectionOffset(0, registerAnimAbs),
        sectionOffset(1, constantAnimAbs),
        remapOffset(0, !file.registerRemap.empty(), registerRemapAbs),
        remapOffset(1, !file.constantRemap.empty(), constantRemapAbs),
        sectionOffset(0, registerNamesAbs),
        sectionOffset(1, constantNamesAbs),
        sectionOffset(0, bankAbs[0][0]), sectionOffset(0, bankAbs[0][1]),
        sectionOffset(0, bankAbs[0][2]), sectionOffset(0, bankAbs[0][3]),
        sectionOffset(1, bankAbs[1][0]), sectionOffset(1, bankAbs[1][1]),
        sectionOffset(1, bankAbs[1][2]), sectionOffset(1, bankAbs[1][3]),
    };
    for (std::size_t index = 0; index < 14; ++index) {
        patchU32(out, chunkStart + 0x20 + index * 4, offsets[index], endian, "BRK table offset");
    }
    patchSizes(out, endian, kFormat);
    return out;
}

const BrkAnimation* findBrkAnimation(const BrkAnimationFile& file, std::string_view materialName,
                                     BrkTarget target) {
    for (const BrkAnimation& animation : file.animations) {
        if (animation.target == target && animation.materialName == materialName) {
            return &animation;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// BPK: one 0x18-byte entry per material (four s16 tracks) over four shared
// banks, with the usual name and remap tables.
// ---------------------------------------------------------------------------

BpkAnimationFile parseBpk(std::span<const std::uint8_t> data) {
    static constexpr const char* kFormat = "BPK";
    static constexpr const char* kChannelNames[4] = {"red", "green", "blue", "alpha"};
    const Endian endian = sniffEndian(data, kFormat);
    const Reader reader{data, endian};
    const std::size_t chunkStart = chunkStartOf(data);
    const std::size_t firstField = chunkStart + 0x0C;
    const std::size_t fieldEnd = chunkStart + 0x34;
    if (data.size() < fieldEnd) {
        throw std::runtime_error("BPK: truncated header");
    }

    BpkAnimationFile file;
    file.duration = reader.u16(chunkStart + 0x0C);
    const std::size_t animationCount = reader.u16(chunkStart + 0x0E);
    std::size_t bankCounts[4];
    for (std::size_t channel = 0; channel < 4; ++channel) {
        bankCounts[channel] = reader.u16(chunkStart + 0x10 + channel * 2);
    }
    std::uint32_t offsets[7];
    for (std::size_t index = 0; index < 7; ++index) {
        offsets[index] = reader.u32(chunkStart + 0x18 + index * 4);
    }
    // Slots: 0 animations, 1 remap, 2 names, 3-6 RGBA banks.
    const auto absIfPresent = [chunkStart](std::uint32_t relative) -> std::size_t {
        return relative == 0 ? std::size_t{0} : chunkStart + relative;
    };
    file.header = captureHeader(
        data, endian, firstField, fieldEnd,
        {absIfPresent(offsets[0]), absIfPresent(offsets[1]), absIfPresent(offsets[2]),
         absIfPresent(offsets[3]), absIfPresent(offsets[4]), absIfPresent(offsets[5]),
         absIfPresent(offsets[6])},
        kFormat);
    if (animationCount == 0) {
        return file; // declared bank leftovers are unread, exactly as Java's are
    }

    std::vector<float> banks[4];
    for (std::size_t channel = 0; channel < 4; ++channel) {
        banks[channel] = readShortBank(
            reader,
            tablePosition(chunkStart, offsets[3 + channel], bankCounts[channel], kFormat,
                          (std::string(kChannelNames[channel]) + " bank").c_str()),
            bankCounts[channel]);
    }
    const std::vector<NameEntryData> names = readNameTable(
        reader, tablePosition(chunkStart, offsets[2], animationCount, kFormat, "name table"),
        animationCount, kFormat, "BPK names");
    if (offsets[1] != 0) {
        const std::size_t remapTable = chunkStart + offsets[1];
        file.materialRemap.reserve(animationCount);
        for (std::size_t index = 0; index < animationCount; ++index) {
            file.materialRemap.push_back(reader.u16(remapTable + index * 2));
        }
    }
    const std::size_t animTable = tablePosition(chunkStart, offsets[0], animationCount, kFormat,
                                                "animation table");
    file.animations.reserve(animationCount);
    for (std::size_t index = 0; index < animationCount; ++index) {
        const std::size_t entry = animTable + index * 0x18;
        BpkAnimation animation;
        animation.materialName = names[index].name;
        animation.nameHash = names[index].hash;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            animation.rgba[channel] = decodeTrack(reader, entry + channel * 6, banks[channel],
                                                  1.0F, kFormat, kChannelNames[channel]);
        }
        file.animations.push_back(std::move(animation));
    }
    return file;
}

std::vector<std::uint8_t> writeBpk(const BpkAnimationFile& file) {
    static constexpr const char* kFormat = "BPK";
    static constexpr const char* kChannelNames[4] = {"red", "green", "blue", "alpha"};
    const Endian endian = file.header.bigEndian ? Endian::big : Endian::little;

    struct EntryEncodings {
        TrackEncoding rgba[4];
    };
    std::vector<std::int16_t> banks[4];
    std::vector<EntryEncodings> encodings;
    std::vector<NameEntryData> names;
    encodings.reserve(file.animations.size());
    names.reserve(file.animations.size());
    for (const BpkAnimation& animation : file.animations) {
        EntryEncodings encoding;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            encoding.rgba[channel] = encodeTrackInto(animation.rgba[channel], banks[channel], 1.0F,
                                                     kFormat, kChannelNames[channel]);
        }
        encodings.push_back(encoding);
        names.push_back(NameEntryData{animation.materialName, animation.nameHash});
    }

    const bool haveData = !file.animations.empty();
    std::vector<std::uint8_t> out;
    out.reserve(file.header.prefix.size() + file.header.fields.size() + 1024);
    out.insert(out.end(), file.header.prefix.begin(), file.header.prefix.end());
    out.insert(out.end(), file.header.fields.begin(), file.header.fields.end());
    alignOut(out);

    const std::size_t animAbs = out.size();
    for (const EntryEncodings& encoding : encodings) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            appendU16(out, encoding.rgba[channel].count, endian);
            appendU16(out, encoding.rgba[channel].index, endian);
            appendU16(out, encoding.rgba[channel].mode, endian);
        }
    }
    alignOut(out);
    const std::size_t remapAbs = out.size();
    if (haveData && !file.materialRemap.empty()) {
        for (const std::uint16_t entry : file.materialRemap) {
            appendU16(out, entry, endian);
        }
    }
    alignOut(out);
    const std::size_t namesAbs = out.size();
    if (haveData) {
        const std::vector<std::uint8_t> table = buildNameTable(names, endian, kFormat, "BPK");
        out.insert(out.end(), table.begin(), table.end());
    }
    std::size_t bankAbs[4] = {};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        alignOut(out);
        bankAbs[channel] = out.size();
        appendShortBank(out, banks[channel], endian);
    }

    const std::size_t chunkStart = chunkStartOf(out);
    const auto relative = [chunkStart](std::size_t position) -> std::uint32_t {
        const std::size_t offset = position - chunkStart;
        if (offset > 0xFFFFFFFFU) {
            throw std::runtime_error("animation: file is too large to write");
        }
        return static_cast<std::uint32_t>(offset);
    };
    const auto offset = [&](std::size_t position) -> std::uint32_t {
        return haveData ? relative(position) : 0U;
    };

    patchU16(out, chunkStart + 0x0C, static_cast<std::uint32_t>(file.duration), endian,
             "BPK duration");
    patchU16(out, chunkStart + 0x0E, static_cast<std::uint32_t>(file.animations.size()), endian,
             "BPK animation count");
    for (std::size_t channel = 0; channel < 4; ++channel) {
        patchU16(out, chunkStart + 0x10 + channel * 2,
                 static_cast<std::uint32_t>(banks[channel].size()), endian, "BPK bank count");
    }
    patchU32(out, chunkStart + 0x18, offset(animAbs), endian, "BPK animation offset");
    patchU32(out, chunkStart + 0x1C,
             (haveData && !file.materialRemap.empty()) ? relative(remapAbs) : 0U, endian,
             "BPK remap offset");
    patchU32(out, chunkStart + 0x20, offset(namesAbs), endian, "BPK name table offset");
    for (std::size_t channel = 0; channel < 4; ++channel) {
        patchU32(out, chunkStart + 0x24 + channel * 4, offset(bankAbs[channel]), endian,
                 "BPK bank offset");
    }
    patchSizes(out, endian, kFormat);
    return out;
}

const BpkAnimation* findBpkAnimation(const BpkAnimationFile& file, std::string_view materialName) {
    for (const BpkAnimation& animation : file.animations) {
        if (animation.materialName == materialName) {
            return &animation;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// BTP: per-material frame swaps, no keyframing. Batch entries are
// {count, first, textureId, 3 skipped bytes}; `first` indexes the shared
// frame bank.
// ---------------------------------------------------------------------------

BtpAnimationFile parseBtp(std::span<const std::uint8_t> data) {
    static constexpr const char* kFormat = "BTP";
    const Endian endian = sniffEndian(data, kFormat);
    const Reader reader{data, endian};
    const std::size_t chunkStart = chunkStartOf(data);
    const std::size_t firstField = chunkStart + 0x0C;
    const std::size_t fieldEnd = chunkStart + 0x20;
    if (data.size() < fieldEnd) {
        throw std::runtime_error("BTP: truncated header");
    }

    BtpAnimationFile file;
    const std::size_t batchCount = reader.u16(chunkStart + 0x0C);
    const std::uint32_t tablesOffset = reader.u32(chunkStart + 0x10);
    const std::uint32_t framesOffset = reader.u32(chunkStart + 0x14);
    const std::uint32_t remapOffset = reader.u32(chunkStart + 0x18);
    const std::uint32_t namesOffset = reader.u32(chunkStart + 0x1C);
    const auto absIfPresent = [chunkStart](std::uint32_t relative) -> std::size_t {
        return relative == 0 ? std::size_t{0} : chunkStart + relative;
    };
    file.header = captureHeader(
        data, endian, firstField, fieldEnd,
        {absIfPresent(tablesOffset), absIfPresent(framesOffset), absIfPresent(remapOffset),
         absIfPresent(namesOffset)},
        kFormat);
    if (batchCount == 0) {
        return file;
    }

    const std::vector<NameEntryData> names = readNameTable(
        reader, tablePosition(chunkStart, namesOffset, batchCount, kFormat, "name table"),
        batchCount, kFormat, "BTP names");
    if (remapOffset != 0) {
        const std::size_t remapTable = chunkStart + remapOffset;
        file.materialRemap.reserve(batchCount);
        for (std::size_t index = 0; index < batchCount; ++index) {
            file.materialRemap.push_back(reader.u16(remapTable + index * 2));
        }
    }
    const std::size_t tables =
        tablePosition(chunkStart, tablesOffset, batchCount, kFormat, "pattern tables");
    struct BatchEntry {
        std::size_t count;
        std::size_t first;
        int textureId;
    };
    std::vector<BatchEntry> batches;
    batches.reserve(batchCount);
    std::size_t frameTotal = 0;
    for (std::size_t index = 0; index < batchCount; ++index) {
        const std::size_t entry = tables + index * 8;
        const BatchEntry batch{reader.u16(entry), reader.u16(entry + 2),
                               static_cast<int>(reader.u8(entry + 4))};
        frameTotal += batch.count;
        batches.push_back(batch);
    }
    // Only require the frame bank when frames exist: a file whose batches
    // are all empty can legally store 0 here (Java would never touch it).
    const std::size_t frames =
        tablePosition(chunkStart, framesOffset, frameTotal, kFormat, "pattern frame bank");
    file.animations.reserve(batchCount);
    for (std::size_t index = 0; index < batchCount; ++index) {
        const BatchEntry& batch = batches[index];
        BtpAnimation animation;
        animation.materialName = names[index].name;
        animation.nameHash = names[index].hash;
        animation.textureId = batch.textureId;
        animation.frames.reserve(batch.count);
        for (std::size_t frame = 0; frame < batch.count; ++frame) {
            animation.frames.push_back(reader.s16(frames + (batch.first + frame) * 2));
        }
        file.animations.push_back(std::move(animation));
    }
    return file;
}

std::vector<std::uint8_t> writeBtp(const BtpAnimationFile& file) {
    static constexpr const char* kFormat = "BTP";
    const Endian endian = file.header.bigEndian ? Endian::big : Endian::little;
    std::vector<NameEntryData> names;
    names.reserve(file.animations.size());
    for (const BtpAnimation& animation : file.animations) {
        names.push_back(NameEntryData{animation.materialName, animation.nameHash});
    }

    const bool haveData = !file.animations.empty();
    std::vector<std::uint8_t> out;
    out.reserve(file.header.prefix.size() + file.header.fields.size() + 1024);
    out.insert(out.end(), file.header.prefix.begin(), file.header.prefix.end());
    out.insert(out.end(), file.header.fields.begin(), file.header.fields.end());
    alignOut(out);

    const std::size_t tablesAbs = out.size();
    std::size_t frameIndex = 0;
    for (const BtpAnimation& animation : file.animations) {
        if (frameIndex > 0xFFFFU) {
            throw std::runtime_error("BTP: frame bank exceeds the u16 first-index field");
        }
        appendU16(out, static_cast<std::uint16_t>(animation.frames.size()), endian);
        appendU16(out, static_cast<std::uint16_t>(frameIndex), endian);
        if (animation.textureId < 0 || animation.textureId > 0xFF) {
            throw std::runtime_error("BTP: texture id does not fit the u8 field");
        }
        out.push_back(static_cast<std::uint8_t>(animation.textureId));
        out.push_back(0); // padding + reserved, skipped by the reference (Btp.java:71)
        out.push_back(0);
        out.push_back(0);
        frameIndex += animation.frames.size();
    }
    alignOut(out);
    const std::size_t framesAbs = out.size();
    for (const BtpAnimation& animation : file.animations) {
        for (const std::int16_t frame : animation.frames) {
            appendU16(out, static_cast<std::uint16_t>(frame), endian);
        }
    }
    alignOut(out);
    const std::size_t remapAbs = out.size();
    if (haveData && !file.materialRemap.empty()) {
        for (const std::uint16_t entry : file.materialRemap) {
            appendU16(out, entry, endian);
        }
    }
    alignOut(out);
    const std::size_t namesAbs = out.size();
    if (haveData) {
        const std::vector<std::uint8_t> table = buildNameTable(names, endian, kFormat, "BTP");
        out.insert(out.end(), table.begin(), table.end());
    }

    const std::size_t chunkStart = chunkStartOf(out);
    const auto relative = [chunkStart](std::size_t position) -> std::uint32_t {
        const std::size_t offset = position - chunkStart;
        if (offset > 0xFFFFFFFFU) {
            throw std::runtime_error("animation: file is too large to write");
        }
        return static_cast<std::uint32_t>(offset);
    };
    const auto offset = [&](std::size_t position) -> std::uint32_t {
        return haveData ? relative(position) : 0U;
    };
    patchU16(out, chunkStart + 0x0C, static_cast<std::uint32_t>(file.animations.size()), endian,
             "BTP batch count");
    patchU32(out, chunkStart + 0x10, offset(tablesAbs), endian, "BTP table offset");
    patchU32(out, chunkStart + 0x14, offset(framesAbs), endian, "BTP frame bank offset");
    patchU32(out, chunkStart + 0x18,
             (haveData && !file.materialRemap.empty()) ? relative(remapAbs) : 0U, endian,
             "BTP remap offset");
    patchU32(out, chunkStart + 0x1C, offset(namesAbs), endian, "BTP name table offset");
    patchSizes(out, endian, kFormat);
    return out;
}

std::optional<std::int16_t> btpFrameAt(const BtpAnimationFile& file,
                                       std::string_view materialName, int textureId,
                                       int frameIndex) {
    for (const BtpAnimation& animation : file.animations) {
        if (animation.materialName != materialName || animation.textureId != textureId) {
            continue;
        }
        if (animation.frames.empty()) {
            return std::nullopt; // Java indexes into the empty list here and throws
        }
        const auto clamped = std::max(0, frameIndex);
        const auto last = static_cast<int>(animation.frames.size() - 1);
        return animation.frames[static_cast<std::size_t>(std::min(clamped, last))];
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// BVA: {size, byteOffset} per batch over a raw byte bank. Values are kept as
// bytes, not booleans, so the round trip cannot flatten them (Bva.java did).
// ---------------------------------------------------------------------------

BvaAnimationFile parseBva(std::span<const std::uint8_t> data) {
    static constexpr const char* kFormat = "BVA";
    const Endian endian = sniffEndian(data, kFormat);
    const Reader reader{data, endian};
    const std::size_t chunkStart = chunkStartOf(data);
    const std::size_t firstField = chunkStart + 0x0C;
    const std::size_t fieldEnd = chunkStart + 0x18;
    if (data.size() < fieldEnd) {
        throw std::runtime_error("BVA: truncated header");
    }

    BvaAnimationFile file;
    const std::size_t batchCount = reader.u16(chunkStart + 0x0C);
    const std::uint32_t tablesOffset = reader.u32(chunkStart + 0x10);
    const std::uint32_t dataOffset = reader.u32(chunkStart + 0x14);
    const auto absIfPresent = [chunkStart](std::uint32_t relative) -> std::size_t {
        return relative == 0 ? std::size_t{0} : chunkStart + relative;
    };
    file.header = captureHeader(data, endian, firstField, fieldEnd,
                                {absIfPresent(tablesOffset), absIfPresent(dataOffset)}, kFormat);
    if (batchCount == 0) {
        return file;
    }

    const std::size_t tables =
        tablePosition(chunkStart, tablesOffset, batchCount, kFormat, "visibility tables");
    // The size count lives in the table itself, so the bank is required only
    // when some batch is non-empty -- same reasoning as BTP's frame bank.
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(batchCount);
    std::size_t byteTotal = 0;
    for (std::size_t index = 0; index < batchCount; ++index) {
        const std::size_t entry = tables + index * 4;
        const std::size_t size = reader.u16(entry);
        const std::size_t start = reader.u16(entry + 2);
        ranges.emplace_back(size, start);
        byteTotal += size;
    }
    const std::size_t bank =
        tablePosition(chunkStart, dataOffset, byteTotal, kFormat, "visibility byte bank");
    file.batches.reserve(batchCount);
    for (const auto& [size, start] : ranges) {
        std::vector<std::uint8_t> frames;
        frames.reserve(size);
        for (std::size_t frame = 0; frame < size; ++frame) {
            frames.push_back(reader.u8(bank + start + frame));
        }
        file.batches.push_back(std::move(frames));
    }
    return file;
}

std::vector<std::uint8_t> writeBva(const BvaAnimationFile& file) {
    static constexpr const char* kFormat = "BVA";
    const Endian endian = file.header.bigEndian ? Endian::big : Endian::little;
    const bool haveData = !file.batches.empty();

    std::vector<std::uint8_t> out;
    out.reserve(file.header.prefix.size() + file.header.fields.size() + 1024);
    out.insert(out.end(), file.header.prefix.begin(), file.header.prefix.end());
    out.insert(out.end(), file.header.fields.begin(), file.header.fields.end());
    alignOut(out);

    const std::size_t tablesAbs = out.size();
    std::size_t byteOffset = 0;
    for (const std::vector<std::uint8_t>& batch : file.batches) {
        if (byteOffset > 0xFFFFU || batch.size() > 0xFFFFU) {
            throw std::runtime_error("BVA: byte bank exceeds the u16 range");
        }
        appendU16(out, static_cast<std::uint16_t>(batch.size()), endian);
        appendU16(out, static_cast<std::uint16_t>(byteOffset), endian);
        byteOffset += batch.size();
    }
    alignOut(out);
    const std::size_t bankAbs = out.size();
    for (const std::vector<std::uint8_t>& batch : file.batches) {
        out.insert(out.end(), batch.begin(), batch.end());
    }

    const std::size_t chunkStart = chunkStartOf(out);
    const auto relative = [chunkStart](std::size_t position) -> std::uint32_t {
        const std::size_t offset = position - chunkStart;
        if (offset > 0xFFFFFFFFU) {
            throw std::runtime_error("animation: file is too large to write");
        }
        return static_cast<std::uint32_t>(offset);
    };
    patchU16(out, chunkStart + 0x0C, static_cast<std::uint32_t>(file.batches.size()), endian,
             "BVA batch count");
    patchU32(out, chunkStart + 0x10, haveData ? relative(tablesAbs) : 0U, endian,
             "BVA table offset");
    patchU32(out, chunkStart + 0x14, haveData ? relative(bankAbs) : 0U, endian,
             "BVA byte bank offset");
    patchSizes(out, endian, kFormat);
    return out;
}



















} // namespace

} // namespace whitehole::smg
