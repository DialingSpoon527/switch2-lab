// SPDX-License-Identifier: MIT
#ifndef P2_COMMAND_PARSER_H
#define P2_COMMAND_PARSER_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define P2_COMMAND_MAX_BYTES 100u
#define P2_COMMAND_NAME_BYTES 64u
#define P2_COMMAND_ACTION_BYTES 16u

static inline bool p2SafeName(const char* value) {
    if (!value) return false;
    for (size_t i = 0; i < P2_COMMAND_NAME_BYTES; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (c == 0) return i != 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return false;
}

static inline bool p2CommandSpace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Parse the byte count supplied by FS, never strlen(data). Reject embedded NUL,
// hidden second commands, truncated tokens, non-ASCII bytes and excess fields.
static inline bool p2ParseCommand(const char* data, size_t size,
                                  char action[P2_COMMAND_ACTION_BYTES],
                                  char name[P2_COMMAND_NAME_BYTES]) {
    action[0] = name[0] = 0;
    if (!data || size == 0 || size > P2_COMMAND_MAX_BYTES) return false;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = (unsigned char)data[i];
        if ((!p2CommandSpace(c) && (c < 0x21 || c > 0x7e)) || c == 0) return false;
    }
    size_t cursor = 0;
    size_t fields = 0;
    while (cursor < size) {
        while (cursor < size && p2CommandSpace((unsigned char)data[cursor])) ++cursor;
        if (cursor == size) break;
        if (fields >= 2) return false;
        const size_t start = cursor;
        while (cursor < size && !p2CommandSpace((unsigned char)data[cursor])) ++cursor;
        const size_t length = cursor - start;
        const size_t capacity = fields == 0 ? P2_COMMAND_ACTION_BYTES : P2_COMMAND_NAME_BYTES;
        char* output = fields == 0 ? action : name;
        if (length >= capacity) return false;
        memcpy(output, data + start, length);
        output[length] = 0;
        ++fields;
    }
    if (fields == 1 && (!strcmp(action, "diag") || !strcmp(action, "list") || !strcmp(action, "help") || !strcmp(action, "cancel"))) return true;
    if (fields != 2 || !p2SafeName(name)) return false;
    if (!strcmp(action, "launch") || !strcmp(action, "stop") || !strcmp(action, "restart")) return true;
    // Application commands 0 and 2 are the p2ping example's ABI, not a generic
    // third-party-plugin protocol. Never send them to an unrelated plugin.
    return (!strcmp(action, "context") || !strcmp(action, "ping")) && !strcmp(name, "p2ping");
}
#endif
