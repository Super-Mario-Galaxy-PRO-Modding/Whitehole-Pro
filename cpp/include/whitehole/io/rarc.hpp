#pragma once

// RarcArchive: the SMG archive format (RARC), read, written, and built from
// nothing.
//
// ---------------------------------------------------------------------------
// THE find() / findExact() RULE. Read this before touching any code here.
//
// There are two lookups and they are NOT interchangeable:
//
//   find()       Human/convenience lookup. Exact match first, then a FALLBACK
//                that also answers to a bare file name and to a path suffix.
//                So find("StartInfo") resolves, and so does
//                find("Placement/Common/ObjInfo") finding
//                ".../Placement/Common/ObjInfo" by suffix. This is deliberate:
//                SMG1 lowercases every path, the editor shows shortened paths,
//                and a human reading "CameraParam.bcam" means the one file by
//                that name.
//
//   findExact()  Exact (case-insensitive) path only. No fallback at all.
//
// THE RULE: anything that WRITES uses findExact() and nothing else. Anything
// that READS for display or for a human-facing answer may use find().
//
// This is not stylistic. Building a zone writes one file per layer, and every
// layer's tables share the same names by design -- Common/StartInfo,
// LayerA/StartInfo, LayerB/StartInfo, ... A writer using find() asks "does
// .../LayerA/StartInfo exist?", gets an answer from Common's "StartInfo" via
// the bare-filename fallback, REPLACES the Common layer's file with LayerA's,
// and reports success. The archive looks complete -- every layer directory is
// present -- while every non-Common layer silently holds no tables at all.
// That bug shipped once. createDirectory() lost data the same way, answering
// "does Stage/jmp/MapParts/Common exist?" with the FILE
// ".../MapParts/Common/MapPartsInfo" and skipping the directory.
//
// findExact() is currently file-local. If you add a writer, use it; if you
// need it from outside this file, promote it to a member rather than widening
// find(). testRarcWriterNeverLooseMatches pins both halves of the rule.
// ---------------------------------------------------------------------------

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

    // A brand-new, EMPTY archive with the given root name -- the entry point for
    // creating a zone or galaxy from nothing, which nothing could do before:
    // every other constructor has to parse bytes that already exist.
    //
    // It shares only the SERIALIZING side of the class with a parsed archive.
    // parse() is never called, so no read()/find()/serialize() path changes for
    // existing archives; the round trip is pinned by a test that builds an
    // archive this way, writes it out, re-parses it, and reopens it through the
    // same StageArchive the editor uses.
    //
    // `metadata` is the 4-byte value a retail archive carries at 0x38 (the game
    // stamps a build number there); 0 is the neutral choice.
    [[nodiscard]] static RarcArchive create(std::string_view rootName, std::uint32_t metadata = 0);

    // Creates a directory inside the archive. Like insert(), this is only
    // meaningful on an archive made by create(): a parsed one already has every
    // directory its file tree implies.
    void createDirectory(std::string_view path);

    [[nodiscard]] static RarcArchive open(const std::filesystem::path& path);
    [[nodiscard]] Endian endian() const noexcept { return endian_; }
    [[nodiscard]] bool wasCompressed() const noexcept { return wasCompressed_; }
    [[nodiscard]] std::string_view rootName() const noexcept { return rootName_; }
    [[nodiscard]] const std::vector<RarcEntry>& entries() const noexcept { return entries_; }
        // The CONVENIENCE lookup, for READERS. Falls back to a bare-file-name and a
        // path-suffix match, so "StartInfo" and "Placement/Common/ObjInfo" both
        // resolve to something useful for a human. Never call this from a writer.
    [[nodiscard]] const RarcEntry* find(std::string_view path) const;
    [[nodiscard]] bool fileExists(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> directories(std::string_view parent) const;
    [[nodiscard]] std::vector<std::string> files(std::string_view parent) const;
    [[nodiscard]] std::vector<std::uint8_t> read(const RarcEntry& entry) const;
    [[nodiscard]] std::vector<std::uint8_t> read(std::string_view path) const;
        // Overwrites an entry the caller already resolved, by identity rather than
        // by name. This is the unambiguous writer: nothing about the path can
        // redirect it, because the path is not consulted. Preferred when the
        // caller came from entries()/find() and already knows which file it means.
    void replace(const RarcEntry& entry, std::vector<std::uint8_t> data);
        // Overwrites the file at `path`, resolving it EXACTLY (no find() fallback).
        // Throws when no such file exists -- a writer must never quietly land on
        // a different entry that happens to share a file name.
    void replace(std::string_view path, std::vector<std::uint8_t> data);
        // Adds a new file under an existing directory (or replaces it when the
        // path already exists), resolving `path` EXACTLY. The path keeps its given
        // casing so a new entry reads like its siblings; lookups stay
        // case-insensitive.
    void insert(std::string_view path, std::vector<std::uint8_t> data);
    [[nodiscard]] std::vector<std::uint8_t> serialize(bool compress) const;

    void extractAll(const std::filesystem::path& destination) const;

private:
    // The non-parsing constructor, used only by create(). It exists because the
    // public constructor has to parse, and there are no bytes to parse when an
    // archive is being built from scratch.
    struct CreateTag {};
    explicit RarcArchive(CreateTag) noexcept {}

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
