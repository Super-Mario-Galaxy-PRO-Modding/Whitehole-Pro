#include "whitehole/io/rarc.hpp"

#include "whitehole/io/yaz0.hpp"
#include "whitehole/smg/hash.hpp"
#include "whitehole/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace whitehole::io {
namespace {

std::size_t checkedAdd(std::size_t left, std::uint32_t right, std::string_view field) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::runtime_error("RARC " + std::string(field) + " overflows the host address space");
    }
    return left + right;
}

std::string safeComponent(std::string_view component) {
    if (component.empty() || component == "." || component == ".."
        || component.find('/') != std::string_view::npos
        || component.find('\\') != std::string_view::npos) {
        throw std::runtime_error("RARC contains an unsafe path component");
    }
    return std::string(component);
}

std::size_t align32(std::size_t value) {
    if (value > std::numeric_limits<std::size_t>::max() - 0x1FU) {
        throw std::runtime_error("RARC size overflows while aligning");
    }
    return (value + 0x1FU) & ~std::size_t{0x1F};
}

std::uint16_t nameHash(std::string_view name) {
    std::uint16_t result = 0;
    for (const auto character : name) {
        result = static_cast<std::uint16_t>(result * 3U + static_cast<unsigned char>(character));
    }
    return result;
}

std::uint32_t directoryMagic(std::string_view name) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        result <<= 8U;
        result |= index < name.size()
            ? static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(name[index])))
            : 0x20U;
    }
    return result;
}

std::string filename(std::string_view path) {
    const auto separator = path.rfind('/');
    return std::string(path.substr(separator == std::string_view::npos ? 0 : separator + 1));
}

std::string parentPath(std::string_view path) {
    const auto separator = path.rfind('/');
    return separator == std::string_view::npos ? std::string{} : std::string(path.substr(0, separator));
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool matchesSuffix(std::string_view path, std::string_view suffix) {
    if (suffix.empty() || suffix.size() >= path.size()) {
        return false;
    }
    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    return path[path.size() - suffix.size() - 1] == '/';
}

} // namespace

RarcArchive::RarcArchive(std::vector<std::uint8_t> bytes)
    : bytes_(yaz0::decompress(bytes)), wasCompressed_(yaz0::isCompressed(bytes)) {
    parse();
}

RarcArchive RarcArchive::open(const std::filesystem::path& path) {
    return RarcArchive(readFile(path));
}

const RarcEntry* RarcArchive::find(std::string_view path) const {
    const auto wanted = normalizePath(path);
    if (wanted.empty()) {
        return nullptr;
    }
    const auto wantedHash = smg::jmapHash(wanted);
    for (const auto& candidate : fileLookup_) {
        if (candidate.hash != wantedHash || candidate.pathLen != wanted.size()) {
            continue;
        }
        const auto& entry = entries_[candidate.index];
        if (whitehole::util::equalIgnoreCase(entry.path, wanted)) {
            return &entry;
        }
    }

    // Fall back to partial paths ("Placement/Common/ObjInfo") and bare file names.
    const auto wantedName = filename(wanted);
    const RarcEntry* suffixMatch = nullptr;
    for (const auto& entry : entries_) {
        if (matchesSuffix(lowercase(entry.path), wanted)) {
            suffixMatch = &entry;
        } else if (whitehole::util::equalIgnoreCase(filename(entry.path), wantedName)) {
            suffixMatch = &entry;
        }
    }
    return suffixMatch;
}

bool RarcArchive::fileExists(std::string_view path) const {
    const auto* entry = find(path);
    return entry != nullptr && !entry->directory;
}

std::string RarcArchive::normalizePath(std::string_view path) const {
    std::string collapsed;
    collapsed.reserve(path.size() + rootName_.size() + 1);
    for (const auto character : path) {
        const auto normalized = character == '\\' ? '/' : character;
        if (normalized == '/' && (collapsed.empty() || collapsed.back() == '/')) {
            continue;
        }
        collapsed.push_back(normalized);
    }
    while (!collapsed.empty() && collapsed.back() == '/') {
        collapsed.pop_back();
    }
    if (collapsed.empty()) {
        return {};
    }

    // Callers usually address archives relative to their root ("/Stage/jmp/..."),
    // while entries are stored root-prefixed, so make both sides agree.
    if (!rootName_.empty()) {
        const auto root = lowercase(rootName_);
        const auto candidate = lowercase(collapsed);
        if (candidate != root && candidate.rfind(root + '/', 0) != 0) {
            collapsed.insert(0, "/");
            collapsed.insert(0, rootName_);
        }
    }
    return lowercase(collapsed);
}

std::vector<std::string> RarcArchive::directories(std::string_view parent) const {
    const auto wanted = normalizePath(parent);
    std::vector<std::string> names;
    if (wanted.empty()) {
        for (const auto& entry : entries_) {
            if (!entry.directory) {
                continue;
            }
            if (lowercase(parentPath(entry.path)) == lowercase(rootName_)) {
                names.push_back(filename(entry.path));
            }
        }
    } else {
        for (const auto& entry : entries_) {
            if (!entry.directory) {
                continue;
            }
            if (lowercase(parentPath(entry.path)) == wanted) {
                names.push_back(filename(entry.path));
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> RarcArchive::files(std::string_view parent) const {
    const auto wanted = normalizePath(parent);
    std::vector<std::string> names;
    if (wanted.empty()) {
        for (const auto& entry : entries_) {
            if (entry.directory) {
                continue;
            }
            if (lowercase(parentPath(entry.path)) == lowercase(rootName_)) {
                names.push_back(filename(entry.path));
            }
        }
    } else {
        for (const auto& entry : entries_) {
            if (entry.directory) {
                continue;
            }
            if (lowercase(parentPath(entry.path)) == wanted) {
                names.push_back(filename(entry.path));
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::uint8_t> RarcArchive::read(std::string_view path) const {
    const auto* entry = find(path);
    if (entry == nullptr) {
        throw std::runtime_error("RARC file does not exist: " + std::string(path));
    }
    return read(*entry);
}

void RarcArchive::parse() {
    if (bytes_.size() < 0x40) {
        throw std::runtime_error("RARC header is truncated");
    }
    const bool big = bytes_[0] == 'R' && bytes_[1] == 'A' && bytes_[2] == 'R' && bytes_[3] == 'C';
    const bool little = bytes_[0] == 'C' && bytes_[1] == 'R' && bytes_[2] == 'A' && bytes_[3] == 'R';
    if (!big && !little) {
        throw std::runtime_error("File is not a RARC archive");
    }
    endian_ = big ? Endian::big : Endian::little;

    BinaryReader reader(bytes_, endian_);
    reader.seek(0x0C);
    const auto dataOffset = checkedAdd(0x20, reader.readU32(), "data offset");
    reader.seek(0x20);
    const auto nodeCount = reader.readU32();
    const auto nodeOffset = checkedAdd(0x20, reader.readU32(), "node offset");
    const auto entryCount = reader.readU32();
    const auto entryOffset = checkedAdd(0x20, reader.readU32(), "entry offset");
    const auto stringTableSize = reader.readU32();
    const auto stringOffset = checkedAdd(0x20, reader.readU32(), "string-table offset");
    metadata_ = reader.readU32();

    if (nodeCount == 0) {
        throw std::runtime_error("RARC contains no root node");
    }
    if (nodeCount > bytes_.size() / 0x10 || entryCount > bytes_.size() / 0x14) {
        throw std::runtime_error("RARC table count is not plausible for the file size");
    }
    if (nodeOffset > bytes_.size() || entryOffset > bytes_.size()
        || stringOffset > bytes_.size() || dataOffset > bytes_.size()) {
        throw std::runtime_error("RARC table points outside the file");
    }
    stringTableEnd_ = checkedAdd(stringOffset, stringTableSize, "string-table size");
    if (stringTableEnd_ > bytes_.size()) {
        throw std::runtime_error("RARC string table extends outside the file");
    }

    reader.seek(nodeOffset + 4);
    rootName_ = safeComponent(readName(stringOffset, reader.readU32()));
    std::vector<bool> visited(nodeCount, false);
    parseNode(0, rootName_, visited, nodeOffset, entryOffset, stringOffset, dataOffset, nodeCount, entryCount);
    buildLookup();
}

void RarcArchive::buildLookup() {
    fileLookup_.clear();
    if (entries_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("RARC contains too many entries for its lookup table");
    }
    fileLookup_.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto key = lowercase(entries_[index].path);
        fileLookup_.push_back({smg::jmapHash(key), static_cast<std::uint32_t>(key.size()),
                               static_cast<std::uint32_t>(index)});
    }
}

std::string RarcArchive::readName(std::size_t stringOffset, std::uint32_t relativeOffset) const {
    const auto offset = checkedAdd(stringOffset, relativeOffset, "string offset");
    if (offset >= stringTableEnd_) {
        throw std::runtime_error("RARC name points outside the string table");
    }
    const auto terminator = std::find(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes_.begin() + static_cast<std::ptrdiff_t>(stringTableEnd_), 0);
    if (terminator == bytes_.begin() + static_cast<std::ptrdiff_t>(stringTableEnd_)) {
        throw std::runtime_error("RARC name is not terminated inside the string table");
    }
    return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset), terminator};
}

void RarcArchive::parseNode(std::uint32_t index, const std::string& path, std::vector<bool>& visited,
                            std::size_t nodeOffset, std::size_t entryOffset, std::size_t stringOffset,
                            std::size_t dataOffset, std::uint32_t nodeCount, std::uint32_t entryCount,
                            std::size_t depth) {
    constexpr std::size_t maximumDirectoryDepth = 256;
    if (depth > maximumDirectoryDepth) {
        throw std::runtime_error("RARC directory tree exceeds the safe nesting limit");
    }
    if (index >= nodeCount) {
        throw std::runtime_error("RARC directory references an invalid node");
    }
    if (visited[index]) {
        throw std::runtime_error("RARC directory tree contains a cycle");
    }
    visited[index] = true;

    BinaryReader reader(bytes_, endian_);
    const auto descriptor = nodeOffset + static_cast<std::size_t>(index) * 0x10U;
    if (descriptor + 0x10 > bytes_.size()) {
        throw std::runtime_error("RARC node descriptor is truncated");
    }
    reader.seek(descriptor + 0x0A);
    const auto childCount = reader.readU16();
    const auto firstChild = reader.readU32();
    if (firstChild > entryCount || childCount > entryCount - firstChild) {
        throw std::runtime_error("RARC node references entries outside the entry table");
    }

    struct ChildDirectory { std::uint32_t index; std::string path; };
    std::vector<ChildDirectory> childDirectories;
    for (std::uint32_t child = 0; child < childCount; ++child) {
        const auto tableIndex = firstChild + child;
        const auto descriptorOffset = entryOffset + static_cast<std::size_t>(tableIndex) * 0x14U;
        if (descriptorOffset + 0x14 > bytes_.size()) {
            throw std::runtime_error("RARC entry descriptor is truncated");
        }
        reader.seek(descriptorOffset + 4);
        std::uint16_t type = 0;
        std::uint16_t nameOffset = 0;
        if (endian_ == Endian::big) {
            type = reader.readU16();
            nameOffset = reader.readU16();
        } else {
            nameOffset = reader.readU16();
            type = reader.readU16();
        }
        const auto relativeDataOffset = reader.readU32();
        const auto size = reader.readU32();
        const auto name = readName(stringOffset, nameOffset);
        if (name == "." || name == "..") {
            continue;
        }
        const auto childPath = path + "/" + safeComponent(name);
        if ((type & 0x0200U) != 0) {
            entries_.push_back({childPath, true, 0, 0});
            replacements_.emplace_back();
            childDirectories.push_back({relativeDataOffset, childPath});
        } else {
            const auto absoluteDataOffset = checkedAdd(dataOffset, relativeDataOffset, "file data offset");
            if (absoluteDataOffset > bytes_.size() || size > bytes_.size() - absoluteDataOffset) {
                throw std::runtime_error("RARC file entry points outside the archive");
            }
            entries_.push_back({childPath, false, size, absoluteDataOffset});
            replacements_.emplace_back();
        }
    }

    for (const auto& child : childDirectories) {
        parseNode(child.index, child.path, visited, nodeOffset, entryOffset, stringOffset,
                  dataOffset, nodeCount, entryCount, depth + 1);
    }
}

std::vector<std::uint8_t> RarcArchive::read(const RarcEntry& entry) const {
    if (entry.directory) {
        throw std::runtime_error("Cannot read a RARC directory as a file");
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].path == entry.path && !entry.directory && replacements_[index].has_value()) {
            return *replacements_[index];
        }
    }
    if (entry.dataOffset > bytes_.size() || entry.size > bytes_.size() - entry.dataOffset) {
        throw std::runtime_error("RARC entry no longer points inside the archive");
    }
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(entry.dataOffset);
    return {begin, begin + static_cast<std::ptrdiff_t>(entry.size)};
}

void RarcArchive::replace(std::string_view path, std::vector<std::uint8_t> data) {
    const auto* entry = find(path);
    if (entry == nullptr) {
        throw std::runtime_error("RARC file does not exist: " + std::string(path));
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (&entries_[index] == entry) {
            replacements_[index] = std::move(data);
            entries_[index].size = replacements_[index]->size();
            return;
        }
    }
    throw std::runtime_error("RARC file does not exist: " + std::string(path));
}

void RarcArchive::insert(std::string_view path, std::vector<std::uint8_t> data) {
    const auto wanted = normalizePath(path);
    if (wanted.empty()) {
        throw std::runtime_error("RARC insert path is empty");
    }
    if (find(wanted) != nullptr) {
        replace(wanted, std::move(data));
        return;
    }

    // Keep the caller's casing for the stored entry (siblings keep theirs too);
    // every lookup path lowercases before comparing, so this stays invisible
    // to find()/read()/replace().
    std::string stored(path);
    for (auto& character : stored) {
        if (character == '\\') {
            character = '/';
        }
    }
    while (stored.size() > 1 && stored.front() == '/') {
        stored.erase(stored.begin());
    }
    if (!rootName_.empty() && !whitehole::util::equalIgnoreCase(stored, rootName_)
        && !whitehole::util::equalIgnoreCase(stored.substr(0, rootName_.size() + 1), rootName_ + "/")) {
        stored.insert(0, "/");
        stored.insert(0, rootName_);
    }

    const auto* parent = find(parentPath(stored));
    if (parent == nullptr || !parent->directory
        || !whitehole::util::equalIgnoreCase(parent->path, parentPath(wanted))) {
        throw std::runtime_error("RARC directory does not exist: " + parentPath(wanted));
    }

    entries_.push_back({stored, false, data.size(), 0});
    replacements_.push_back(std::move(data));
    buildLookup();
}

std::vector<std::uint8_t> RarcArchive::serialize(bool compress) const {
    struct Node {
        std::string path;
        std::string name;
        std::uint32_t parent{0xFFFFFFFFU};
        std::vector<std::uint32_t> directories;
        std::vector<std::size_t> files;
        std::uint32_t nameOffset{0};
        std::uint32_t firstEntry{0};
    };

    std::vector<Node> nodes{{rootName_, rootName_, 0xFFFFFFFFU, {}, {}, 0, 0}};
    std::unordered_map<std::string, std::uint32_t> nodeByPath{{rootName_, 0}};
    for (const auto& entry : entries_) {
        if (!entry.directory) {
            continue;
        }
        const auto parent = parentPath(entry.path);
        const auto found = nodeByPath.find(parent);
        if (found == nodeByPath.end()) {
            throw std::runtime_error("RARC directory has no serialized parent");
        }
        const auto index = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back({entry.path, filename(entry.path), found->second, {}, {}, 0, 0});
        nodes[found->second].directories.push_back(index);
        nodeByPath.emplace(entry.path, index);
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].directory) {
            continue;
        }
        const auto found = nodeByPath.find(parentPath(entries_[index].path));
        if (found == nodeByPath.end()) {
            throw std::runtime_error("RARC file has no serialized parent");
        }
        nodes[found->second].files.push_back(index);
    }

    std::size_t entryCount = 0;
    std::size_t fileCount = 0;
    for (auto& node : nodes) {
        if (node.directories.size() + node.files.size() + 2 > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("RARC directory contains too many children");
        }
        node.firstEntry = static_cast<std::uint32_t>(entryCount);
        entryCount += node.directories.size() + node.files.size() + 2;
        fileCount += node.files.size();
    }
    if (nodes.size() > std::numeric_limits<std::uint32_t>::max()
        || entryCount > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("RARC contains too many nodes or entries");
    }
    if (fileCount > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("RARC contains too many files for 16-bit file IDs");
    }

    std::vector<std::uint32_t> fileNameOffsets(entries_.size(), 0);
    std::size_t stringSize = 5; // ".\0..\0"
    for (auto& node : nodes) {
        if (stringSize > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("RARC string table is too large");
        }
        node.nameOffset = static_cast<std::uint32_t>(stringSize);
        stringSize += node.name.size() + 1;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (!entries_[index].directory) {
            if (stringSize > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("RARC file-name offset exceeds the format limit");
            }
            fileNameOffsets[index] = static_cast<std::uint32_t>(stringSize);
            stringSize += filename(entries_[index].path).size() + 1;
        }
    }
    for (const auto& node : nodes) {
        if (node.nameOffset > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("RARC directory-name offset exceeds the format limit");
        }
    }

    const std::size_t nodeOffset = 0x40;
    const auto entryOffset = nodeOffset + align32(nodes.size() * 0x10U);
    const auto stringOffset = entryOffset + align32(entryCount * 0x14U);
    const auto dataOffset = align32(stringOffset + stringSize);
    std::vector<std::vector<std::uint8_t>> resolvedPayloads(entries_.size());
    std::size_t dataLength = 0;
    for (const auto& node : nodes) {
        for (const auto fileIndex : node.files) {
            resolvedPayloads[fileIndex] = read(entries_[fileIndex]);
            const auto alignedFileSize = align32(resolvedPayloads[fileIndex].size());
            if (alignedFileSize > std::numeric_limits<std::size_t>::max() - dataLength) {
                throw std::runtime_error("RARC file data size overflows the host address space");
            }
            dataLength += alignedFileSize;
        }
    }
    if (dataOffset > std::numeric_limits<std::uint32_t>::max()
        || dataLength > std::numeric_limits<std::uint32_t>::max()
        || dataLength > std::numeric_limits<std::uint32_t>::max() - dataOffset) {
        throw std::runtime_error("RARC output exceeds the 4 GiB format limit");
    }

    BinaryWriter writer(endian_);
    writer.writeU32(0x52415243);
    writer.writeU32(static_cast<std::uint32_t>(dataOffset + dataLength));
    writer.writeU32(0x20);
    writer.writeU32(static_cast<std::uint32_t>(dataOffset - 0x20));
    writer.writeU32(static_cast<std::uint32_t>(dataLength));
    writer.writeU32(static_cast<std::uint32_t>(dataLength));
    writer.writeU32(0);
    writer.writeU32(0);
    writer.writeU32(static_cast<std::uint32_t>(nodes.size()));
    writer.writeU32(static_cast<std::uint32_t>(nodeOffset - 0x20));
    writer.writeU32(static_cast<std::uint32_t>(entryCount));
    writer.writeU32(static_cast<std::uint32_t>(entryOffset - 0x20));
    writer.writeU32(static_cast<std::uint32_t>(dataOffset - stringOffset));
    writer.writeU32(static_cast<std::uint32_t>(stringOffset - 0x20));
    writer.writeU32(metadata_);
    writer.writeU32(0);

    writer.seek(stringOffset);
    writer.writeString(".");
    writer.writeString("..");
    for (const auto& node : nodes) {
        writer.writeString(node.name);
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (!entries_[index].directory) {
            writer.writeString(filename(entries_[index].path));
        }
    }

    std::uint16_t fileId = 0;
    std::size_t dataPosition = 0;
    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const auto& node = nodes[nodeIndex];
        writer.seek(nodeOffset + nodeIndex * 0x10U);
        writer.writeU32(nodeIndex == 0 ? 0x524F4F54 : directoryMagic(node.name));
        writer.writeU32(node.nameOffset);
        writer.writeU16(nameHash(node.name));
        writer.writeU16(static_cast<std::uint16_t>(node.directories.size() + node.files.size() + 2));
        writer.writeU32(node.firstEntry);
        writer.seek(entryOffset + static_cast<std::size_t>(node.firstEntry) * 0x14U);

        const auto writeTypeAndName = [&](std::uint16_t type, std::uint16_t nameOffsetValue) {
            if (endian_ == Endian::big) {
                writer.writeU16(type);
                writer.writeU16(nameOffsetValue);
            } else {
                writer.writeU16(nameOffsetValue);
                writer.writeU16(type);
            }
        };
        for (const auto childIndex : node.directories) {
            const auto& child = nodes[childIndex];
            writer.writeU16(0xFFFF);
            writer.writeU16(nameHash(child.name));
            writeTypeAndName(0x0200, static_cast<std::uint16_t>(child.nameOffset));
            writer.writeU32(childIndex);
            writer.writeU32(0x10);
            writer.writeU32(0);
        }
        for (const auto entryIndex : node.files) {
            const auto& data = resolvedPayloads[entryIndex];
            writer.writeU16(fileId++);
            writer.writeU16(nameHash(filename(entries_[entryIndex].path)));
            writeTypeAndName(0x1100, static_cast<std::uint16_t>(fileNameOffsets[entryIndex]));
            writer.writeU32(static_cast<std::uint32_t>(dataPosition));
            writer.writeU32(static_cast<std::uint32_t>(data.size()));
            writer.writeU32(0);
            const auto nextEntryPosition = writer.position();
            writer.seek(dataOffset + dataPosition);
            writer.writeBytes(data);
            dataPosition += align32(data.size());
            writer.seek(nextEntryPosition);
        }
        writer.writeU16(0xFFFF);
        writer.writeU16(0x002E);
        writeTypeAndName(0x0200, 0);
        writer.writeU32(static_cast<std::uint32_t>(nodeIndex));
        writer.writeU32(0x10);
        writer.writeU32(0);
        writer.writeU16(0xFFFF);
        writer.writeU16(0x00B8);
        writeTypeAndName(0x0200, 2);
        writer.writeU32(node.parent);
        writer.writeU32(0x10);
        writer.writeU32(0);
    }
    writer.seek(dataOffset + dataLength);
    auto output = std::move(writer).take();
    return compress ? yaz0::compress(output) : output;
}

void RarcArchive::extractAll(const std::filesystem::path& destination) const {
    const auto root = destination / safeComponent(rootName_);
    std::filesystem::create_directories(root);
    for (const auto& entry : entries_) {
        std::filesystem::path relative;
        std::size_t start = rootName_.size() + 1;
        while (start < entry.path.size()) {
            const auto end = entry.path.find('/', start);
            relative /= safeComponent(entry.path.substr(start, end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        const auto output = root / relative;
        if (entry.directory) {
            std::filesystem::create_directories(output);
        } else {
            writeFile(output, read(entry));
        }
    }
}

} // namespace whitehole::io
