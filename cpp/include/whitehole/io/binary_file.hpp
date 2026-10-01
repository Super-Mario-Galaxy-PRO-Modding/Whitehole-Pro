#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace whitehole::io {

enum class Endian : std::uint8_t { little, big };

std::vector<std::uint8_t> readFile(const std::filesystem::path& path);
void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> data);
void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data);

// A sibling temporary path unique to THIS writer -- "<path>.tmp<addr>-<n>".
//
// Exposed because the rule is that every writer goes through writeFile(), and a
// caller that cannot (it streams, or it wants to stage bytes before committing)
// still must not invent a FIXED suffix. A fixed ".tmp" is a race: two writers
// saving the same file open the SAME temporary, and whichever commits first
// deletes it out from under the other. That is a bug this repo has already
// shipped once in writeFile() and once again in the object-database download,
// so the helper is public: there is exactly one naming scheme.
[[nodiscard]] std::filesystem::path temporaryPathFor(const std::filesystem::path& path);

// ----------------------------------------------------------------------------

class BinaryReader {
public:
    explicit BinaryReader(const std::vector<std::uint8_t>& data, Endian endian = Endian::little);

    void setEndian(Endian endian) noexcept { endian_ = endian; }
    [[nodiscard]] Endian endian() const noexcept { return endian_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }

    void seek(std::size_t position);
    void skip(std::ptrdiff_t amount);

    [[nodiscard]] std::uint8_t readU8();
    [[nodiscard]] std::uint16_t readU16();
    [[nodiscard]] std::uint32_t readU32();
    [[nodiscard]] float readF32();

    [[nodiscard]] std::vector<std::uint8_t> readBytes(std::size_t count);
    [[nodiscard]] std::span<const std::uint8_t> peekBytes(std::size_t count) const;

    [[nodiscard]] std::string readString(std::size_t maxLength = 0);

    [[nodiscard]] bool trySkip(std::size_t amount) noexcept;

private:
    void require(std::size_t count) const;

    Endian endian_ = Endian::little;
    std::size_t position_ = 0;
    const std::vector<std::uint8_t>& data_;
};

// ----------------------------------------------------------------------------

class BinaryWriter {
public:
    explicit BinaryWriter(Endian endian = Endian::little) : endian_(endian) {}

    void setEndian(Endian endian) noexcept { endian_ = endian; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return data_.capacity(); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    std::vector<std::uint8_t>&& take() && { return std::move(data_); }

    void seek(std::size_t position);
    void writeU8(std::uint8_t value);
    void writeU16(std::uint16_t value);
    void writeU32(std::uint32_t value);
    void writeF32(float value);

    void writeBytes(std::span<const std::uint8_t> value);
    void writeBytes(const std::vector<std::uint8_t>& value);

    void writeString(std::string_view value, bool nullTerminate = true);

    void writeSpanRepeated(std::size_t count, std::uint8_t value) noexcept;
    void reserve(std::size_t count);
    void patchU16(std::size_t offset, std::uint16_t value);
    void patchU32(std::size_t offset, std::uint32_t value);
    void align32();

    std::vector<std::uint8_t>&& finalize() && { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
    Endian endian_ = Endian::little;
    std::size_t position_ = 0;
};

// ----------------------------------------------------------------------------

template<std::size_t N>
[[nodiscard]] std::array<std::uint8_t, N> readLittleBytes(const std::vector<std::uint8_t>& data,
                                                          std::size_t position) {
    if (position > data.size() || N > data.size() - position) {
        throw std::runtime_error("binary view overread at offset " + std::to_string(position));
    }
    std::array<std::uint8_t, N> out{};
    std::memcpy(out.data(), data.data() + position, N);
    return out;
}

} // namespace whitehole::io
