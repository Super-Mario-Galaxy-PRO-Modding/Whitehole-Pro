#include "whitehole/io/binary_file.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace whitehole::io {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Could not open file for reading: " + path.string());
    }

    const auto end = stream.tellg();
    if (end < 0) {
        throw std::runtime_error("Could not determine file size: " + path.string());
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("File is too large to load: " + path.string());
    }

    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!result.empty() && !stream.read(reinterpret_cast<char*>(result.data()), end)) {
        throw std::runtime_error("Could not read file: " + path.string());
    }
    return result;
}

// A temp name unique to this writer. A fixed ".tmp" suffix looks safe but is a
// race: two writers saving the same archive both open the SAME temporary, and
// whichever renames first deletes it out from under the other -- which is how a
// save could fail with a missing-file error naming a file the caller never
// created. The address of a local plus the counter is enough entropy here: two
// concurrent writers in one process are separated by the counter, and two
// processes never share an address space.
//
// Deliberately portable -- no Win32 PID and no clock. This file is core code
// that has to build on every platform (roadmap item 8), so the uniqueness comes
// from C++ only.
std::filesystem::path temporaryPathFor(const std::filesystem::path& path) {
    static std::atomic<unsigned long long> counter{0};
    auto temporary = path;
    temporary += ".tmp";
    const auto local = static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(&path));
    temporary += std::to_string(local) + "-" +
                 std::to_string(counter.fetch_add(1) + 1);
    return temporary;
}

void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    // Write to a sibling temporary file first: an interrupted or failed save can
    // then never destroy the user's existing archive.
    const auto temporary = temporaryPathFor(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("Could not open file for writing: " + path.string());
        }
        if (!data.empty()) {
            stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        stream.flush();
        if (!stream) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("Could not write file: " + path.string());
        }
    }

    // Best effort only: a backup that cannot be taken must not stop the save.
    // Clearing `error` afterwards is deliberate -- the rename below reports the
    // outcome that matters, and a stale code here would be misread as its
    // failure.
    std::error_code error;
    if (std::filesystem::is_regular_file(path)) {
        auto backup = path;
        backup += ".bak";
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, error);
        error.clear();
    }

    std::filesystem::rename(temporary, path, error);
    if (!error) {
        return;
    }

    // The rename failed, most often because the destination is held open by
    // another process (a hex editor, or a second Whitehole Pro saving the same
    // archive). Fall back to an in-place copy.
    //
    // This used to call the THROWING copy_file overload, which was a real bug:
    // it discarded the error_code discipline the rest of the function is built
    // on, and reported a bare "cannot copy file" that named neither the cause
    // nor the destination. Worse, when the failure was a race for the .tmp (two
    // writers, one temp name) the copy then failed because the temporary had
    // already been consumed -- surfacing as a missing-file error pointing at a
    // file the caller never knew existed.
    const auto renameReason = error.message();
    error.clear();
    std::filesystem::copy_file(temporary, path,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        // Leave nothing behind, and say what actually went wrong: the first
        // failure (the rename) is the cause, the second only the symptom.
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Could not save " + path.string() +
                                 ": rename failed (" + renameReason +
                                 ") and the in-place copy failed (" + error.message() +
                                 ")");
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    writeFile(path, std::span<const std::uint8_t>{data});
}

BinaryReader::BinaryReader(const std::vector<std::uint8_t>& data, Endian endian)
    : endian_(endian), data_(data) {}

void BinaryReader::require(std::size_t count) const {
    if (count > remaining()) {
        throw std::runtime_error("Unexpected end of binary data at offset " + std::to_string(position_));
    }
}

void BinaryReader::seek(std::size_t position) {
    if (position > data_.size()) {
        throw std::runtime_error("Binary seek is outside the file");
    }
    position_ = position;
}

void BinaryReader::skip(std::ptrdiff_t amount) {
    if (amount < 0) {
        const auto distance = static_cast<std::size_t>(-amount);
        if (distance > position_) {
            throw std::runtime_error("Binary seek is before the start of the file");
        }
        position_ -= distance;
        return;
    }
    const auto distance = static_cast<std::size_t>(amount);
    require(distance);
    position_ += distance;
}

std::uint8_t BinaryReader::readU8() {
    require(1);
    return data_[position_++];
}

std::uint16_t BinaryReader::readU16() {
    require(2);
    const auto a = static_cast<std::uint16_t>(data_[position_]);
    const auto b = static_cast<std::uint16_t>(data_[position_ + 1]);
    position_ += 2;
    return endian_ == Endian::big ? static_cast<std::uint16_t>((a << 8U) | b)
                                  : static_cast<std::uint16_t>(a | (b << 8U));
}

std::uint32_t BinaryReader::readU32() {
    require(4);
    std::uint32_t value = 0;
    if (endian_ == Endian::big) {
        for (int i = 0; i < 4; ++i) {
            value = (value << 8U) | data_[position_++];
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(data_[position_++]) << (i * 8U);
        }
    }
    return value;
}

float BinaryReader::readF32() {
    const auto bits = readU32();
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<std::uint8_t> BinaryReader::readBytes(std::size_t count) {
    require(count);
    const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(position_);
    position_ += count;
    return {begin, begin + static_cast<std::ptrdiff_t>(count)};
}

std::span<const std::uint8_t> BinaryReader::peekBytes(std::size_t count) const {
    require(count);
    return std::span<const std::uint8_t>{data_}.subspan(position_, count);
}

bool BinaryReader::trySkip(std::size_t amount) noexcept {
    if (amount > remaining()) {
        return false;
    }
    position_ += amount;
    return true;
}

std::string BinaryReader::readString(std::size_t maxLength) {
    std::string result;
    while (remaining() > 0 && (maxLength == 0 || result.size() < maxLength)) {
        const auto value = readU8();
        if (value == 0) {
            break;
        }
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void BinaryWriter::reserve(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() - position_) {
        throw std::overflow_error("Binary output is too large");
    }
    const auto required = position_ + count;
    if (required > data_.size()) {
        data_.resize(required, 0);
    }
}

void BinaryWriter::seek(std::size_t position) {
    if (position > data_.size()) {
        data_.resize(position, 0);
    }
    position_ = position;
}

void BinaryWriter::writeU8(std::uint8_t value) {
    reserve(1);
    data_[position_++] = value;
}

void BinaryWriter::writeU16(std::uint16_t value) {
    reserve(2);
    if (endian_ == Endian::big) {
        data_[position_++] = static_cast<std::uint8_t>(value >> 8U);
        data_[position_++] = static_cast<std::uint8_t>(value);
    } else {
        data_[position_++] = static_cast<std::uint8_t>(value);
        data_[position_++] = static_cast<std::uint8_t>(value >> 8U);
    }
}

void BinaryWriter::writeU32(std::uint32_t value) {
    reserve(4);
    if (endian_ == Endian::big) {
        for (int i = 3; i >= 0; --i) {
            data_[position_++] = static_cast<std::uint8_t>(value >> (i * 8U));
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            data_[position_++] = static_cast<std::uint8_t>(value >> (i * 8U));
        }
    }
}

void BinaryWriter::writeF32(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bits);
}

void BinaryWriter::writeBytes(std::span<const std::uint8_t> value) {
    if (value.empty()) {
        return;
    }
    reserve(value.size());
    std::memcpy(data_.data() + position_, value.data(), value.size());
    position_ += value.size();
}

void BinaryWriter::writeBytes(const std::vector<std::uint8_t>& value) {
    writeBytes(std::span<const std::uint8_t>{value});
}

void BinaryWriter::writeString(std::string_view value, bool nullTerminate) {
    if (value.empty() && !nullTerminate) {
        return;
    }
    reserve(value.size() + (nullTerminate ? 1U : 0U));
    std::memcpy(data_.data() + position_, value.data(), value.size());
    position_ += value.size();
    if (nullTerminate) {
        data_[position_++] = 0;
    }
}

void BinaryWriter::writeSpanRepeated(std::size_t count, std::uint8_t value) noexcept {
    if (count == 0) {
        return;
    }
    const auto required = position_ + count;
    data_.resize(std::max(data_.size(), required), 0);
    std::fill(data_.data() + position_, data_.data() + required, value);
    position_ = required;
}

void BinaryWriter::patchU16(std::size_t offset, std::uint16_t value) {
    if (offset > data_.size() || 2 > data_.size() - offset) {
        throw std::out_of_range("Binary patch offset is outside the output buffer");
    }
    const std::array<std::uint8_t, 2> bytes{
        endian_ == Endian::big ? static_cast<std::uint8_t>(value >> 8U) : static_cast<std::uint8_t>(value),
        endian_ == Endian::big ? static_cast<std::uint8_t>(value) : static_cast<std::uint8_t>(value >> 8U)};
    std::memcpy(data_.data() + offset, bytes.data(), bytes.size());
}

void BinaryWriter::patchU32(std::size_t offset, std::uint32_t value) {
    if (offset > data_.size() || 4 > data_.size() - offset) {
        throw std::out_of_range("Binary patch offset is outside the output buffer");
    }
    if (endian_ == Endian::big) {
        data_[offset] = static_cast<std::uint8_t>(value >> 24U);
        data_[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
        data_[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
        data_[offset + 3] = static_cast<std::uint8_t>(value);
    } else {
        data_[offset] = static_cast<std::uint8_t>(value);
        data_[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
        data_[offset + 2] = static_cast<std::uint8_t>(value >> 16U);
        data_[offset + 3] = static_cast<std::uint8_t>(value >> 24U);
    }
}

void BinaryWriter::align32() {
    constexpr std::size_t mask = 31;
    const std::size_t pad = (mask - (position_ & mask)) & mask;
    if (pad != 0) {
        writeSpanRepeated(pad, 0);
    }
}

} // namespace whitehole::io
