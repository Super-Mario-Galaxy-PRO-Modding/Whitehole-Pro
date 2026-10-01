#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace whitehole::util {

inline std::string toLower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

// The counterpart of toLower, added for canonicalLayerName(): SMG1 archives
// store every path lowercase and the canonical spelling the code uses is
// capitalised, so something has to fold the other direction.
inline std::string toUpper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

inline std::string replaceSlashes(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

inline std::string trimSlashes(std::string_view value) {
    auto begin = value.find_first_not_of('/');
    if (begin == std::string_view::npos) {
        return {};
    }
    auto end = value.find_last_not_of('/');
    return std::string(value.substr(begin, end - begin + 1));
}

inline bool equalIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index]))
            != std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace whitehole::util
