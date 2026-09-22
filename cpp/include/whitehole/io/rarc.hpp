#pragma once

#include "whitehole/io/binary_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whitehole::io {

struct RarcEntry {
    std::string path;
    bool directory{false};
    std::size_t size{0};
    std::size_t dataOffset{0};
};

class RarcArchive {
public:
    explicit RarcArchive(std::vector<std::uint8_t> bytes);

    [[nodiscard]] static RarcArchive open(const std::filesystem::path& path);
    [[nodiscard]] Endian endian() const noexcept { return endian_; }
    [[nodiscard]] bool wasCompressed() const noexcept { return wasCompressed_; }
    [[nodiscard]] std::string_view rootName() const noexcept { return rootName_; }
    [[nodiscard]] const std::vector<RarcEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const RarcEntry* find(std::string_view path) const;
    [[nodiscard]] bool fileExists(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> directories(std::string_view parent) const;
    [[nodiscard]] std::vector<std::string> files(std::string_view parent) const;
    [[nodiscard]] std::vector<std::uint8_t> read(const RarcEntry& entry) const;
    [[nodiscard]] std::vector<std::uint8_t> read(std::string_view path) const;
    void replace(std::string_view path, std::vector<std::uint8_t> data);
    // Adds a new file under an existing directory (or replaces it when the
    // path already exists). The path keeps its given casing so a new entry
    // reads like its siblings; lookups stay case-insensitive.
    void insert(std::string_view path, std::vector<std::uint8_t> data);
    [[nodiscard]] std::vector<std::uint8_t> serialize(bool compress) const;

    void extractAll(const std::filesystem::path& destination) const;

private:
    void parse();
    void parseNode(std::uint32_t index, const std::string& path, std::vector<bool>& visited,
                   std::size_t nodeOffset, std::size_t entryOffset, std::size_t stringOffset,
                   std::size_t dataOffset, std::uint32_t nodeCount, std::uint32_t entryCount,
                   std::size_t depth = 0);
    [[nodiscard]] std::string readName(std::size_t stringOffset, std::uint32_t relativeOffset) const;
    [[nodiscard]] std::string normalizePath(std::string_view path) const;
    void buildLookup();

    struct FileLookup {
        std::uint32_t hash{0};
        std::uint32_t pathLen{0};
        std::uint32_t index{0};
    };

    std::vector<std::uint8_t> bytes_;
    std::vector<RarcEntry> entries_;
    Endian endian_{Endian::big};
    bool wasCompressed_{false};
    std::string rootName_;
    std::size_t stringTableEnd_{0};
    std::uint32_t metadata_{0};
    std::vector<std::optional<std::vector<std::uint8_t>>> replacements_;
    std::vector<FileLookup> fileLookup_;
};

} // namespace whitehole::io
