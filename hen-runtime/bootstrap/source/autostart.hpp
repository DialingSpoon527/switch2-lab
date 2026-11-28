// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stddef.h>
#include <string.h>

namespace autostart {

constexpr size_t MaxBytes = 4096;
constexpr size_t MaxPlugins = 64;
constexpr size_t NameSize = 64;

struct Plan {
    char names[MaxPlugins][NameSize] = {};
    size_t count = 0;
};

enum class Error {
    None,
    TooLarge,
    InvalidName,
    TooManyPlugins,
};

inline bool nameByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// Validate the complete blacklist before returning any excluded plugins. A
// malformed later line must never enable plugins by accepting only part of it.
inline Error parse(const char* bytes, size_t size, Plan& out) {
    out = {};
    if (size > MaxBytes) return Error::TooLarge;
    if (!bytes && size) return Error::InvalidName;
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] == '\0') return Error::InvalidName;

    Plan plan;
    size_t at = 0;
    // Accept a UTF-8 BOM, including files saved by Windows PowerShell.
    if (size >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf)
        at = 3;
    while (at < size) {
        size_t end = at;
        while (end < size && bytes[end] != '\n') ++end;
        size_t last = at;
        while (last < end && bytes[last] != '#') ++last;
        size_t first = at;
        while (first < last && (bytes[first] == ' ' || bytes[first] == '\t')) ++first;
        while (last > first && (bytes[last - 1] == ' ' || bytes[last - 1] == '\t' ||
                               bytes[last - 1] == '\r')) --last;
        const size_t length = last - first;
        if (length) {
            if (length >= NameSize) return Error::InvalidName;
            for (size_t i = first; i < last; ++i)
                if (!nameByte(static_cast<unsigned char>(bytes[i])))
                    return Error::InvalidName;
            bool duplicate = false;
            for (size_t i = 0; i < plan.count; ++i)
                if (strlen(plan.names[i]) == length &&
                    memcmp(plan.names[i], bytes + first, length) == 0)
                    duplicate = true;
            if (!duplicate) {
                if (plan.count == MaxPlugins) return Error::TooManyPlugins;
                memcpy(plan.names[plan.count++], bytes + first, length);
            }
        }
        at = end < size ? end + 1 : end;
    }
    out = plan;
    return Error::None;
}

inline bool contains(const Plan& blocked, const char* name) {
    for (size_t i = 0; i < blocked.count; ++i)
        if (strcmp(blocked.names[i], name) == 0) return true;
    return false;
}

} // namespace autostart
