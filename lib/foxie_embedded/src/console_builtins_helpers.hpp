#pragma once

#include <cstddef>
#include <cstring>

namespace fxe {

inline const char* SafeStr(const char* s) {
    return s ? s : "";
}

inline bool StartsWithStr(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }
    while (*prefix != '\0') {
        if (*s == '\0' || *s != *prefix) {
            return false;
        }
        ++s;
        ++prefix;
    }
    return true;
}

inline size_t AddUniqueMatchIfPrefix(const char* candidate,
                                     const char* prefix,
                                     const char** outMatches,
                                     size_t matchCount,
                                     const size_t outMatchesCap) {
    if (!candidate || candidate[0] == '\0') {
        return matchCount;
    }

    if (prefix && prefix[0] != '\0' && !StartsWithStr(candidate, prefix)) {
        return matchCount;
    }

    for (size_t i = 0; i < matchCount; ++i) {
        if (outMatches[i] && std::strcmp(outMatches[i], candidate) == 0) {
            return matchCount;
        }
    }

    if (matchCount >= outMatchesCap) {
        return matchCount;
    }

    outMatches[matchCount] = candidate;
    return matchCount + 1;
}

} // namespace fxe
