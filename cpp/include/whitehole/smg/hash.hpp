#pragma once

#include <cstdint>
#include <string_view>

namespace whitehole::smg {

[[nodiscard]] std::uint32_t jmapHash(std::string_view value) noexcept;
[[nodiscard]] std::uint32_t superFastHash(std::string_view value, std::uint32_t seed = 0) noexcept;

// BCSV field-name hash, used when the editor creates or renames a column.
// Mirrors Java Bcsv.getNameHash(): a name of the form "[1A2B3C4D]" is the
// literal hash (that is exactly what FieldHashes::nameOf() prints for a hash
// with no known name), anything else hashes the text with jmapHash. Without
// this, renaming a column whose name is unknown would hash the bracketed text
// and silently detach the column from the data the game reads.
[[nodiscard]] std::uint32_t fieldHash(std::string_view name) noexcept;

} // namespace whitehole::smg
