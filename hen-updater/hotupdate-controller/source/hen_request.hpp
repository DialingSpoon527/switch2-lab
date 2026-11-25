// SPDX-License-Identifier: GPL-2.0-only
// Strict, dependency-free HEN updater request parsing. This has NO RO commands.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hotupdate::henonly {
enum class Request { Status, DryRun, QueueHen, ApplyManifest, Cancel, Invalid };
inline bool exactly(const char* data, std::size_t n, const char* keyword) {
    const std::size_t length = std::strlen(keyword);
    return n == length && std::memcmp(data, keyword, length) == 0;
}
inline Request parseRequest(const char* data, std::size_t n) {
    if (!data) return Request::Invalid;
    // Allow a single LF or CRLF at the end; no whitespace or trailing bytes.
    if (n && data[n - 1] == '\n') {
        --n;
        if (n && data[n - 1] == '\r') --n;
    }
    if (exactly(data, n, "STATUS")) return Request::Status;
    if (exactly(data, n, "DEFER_DRYRUN_V1")) return Request::DryRun;
    if (exactly(data, n, "DEFER_HEN_RESTART_V1")) return Request::QueueHen;
    if (exactly(data, n, "APPLY_HEN_PACKAGE_V1")) return Request::ApplyManifest;
    if (exactly(data, n, "CANCEL_DEFERRED_V2")) return Request::Cancel;
    return Request::Invalid;
}
inline int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline bool parseManifest(const char* data, std::size_t n, std::uint8_t digest[32]) {
    if (!data || !digest || !n || n > 128) return false;
    // Accept exactly two lines, both LF and CRLF, no extra directives.
    const char* header = "target=hen";
    constexpr std::size_t header_len = 10;
    if (n < header_len + 1 + 7 + 64 || std::memcmp(data, header, header_len))
        return false;
    std::size_t at = header_len;
    if (at < n && data[at] == '\r') ++at;
    if (at >= n || data[at++] != '\n') return false;
    if (at + 7 + 64 > n || std::memcmp(data + at, "sha256=", 7)) return false;
    at += 7;
    std::uint8_t temporary[32] = {};
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = nibble(data[at + i*2]);
        const int lo = nibble(data[at + i*2 + 1]);
        if (hi < 0 || lo < 0) return false;
        temporary[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    at += 64;
    if (at < n && data[at] == '\r') {
        ++at;
        if (at >= n || data[at] != '\n') return false;
    }
    if (at < n && data[at] == '\n') ++at;
    if (at != n) return false;
    std::memcpy(digest, temporary, 32);
    return true;
}
} // namespace hotupdate::henonly
