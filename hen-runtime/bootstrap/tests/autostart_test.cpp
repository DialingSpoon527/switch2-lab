// SPDX-License-Identifier: GPL-2.0-only
#include "../source/autostart.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using autostart::Error;
using autostart::Plan;

static Plan valid(const std::string& text) {
    Plan result;
    assert(autostart::parse(text.data(), text.size(), result) == Error::None);
    return result;
}

static void invalid(const std::string& text, Error expected = Error::InvalidName) {
    Plan result = valid("previous\n");
    assert(autostart::parse(text.data(), text.size(), result) == expected);
    assert(result.count == 0);
    for (const auto& name : result.names)
        for (char byte : name) assert(byte == 0);
}

int main() {
    assert(valid("").count == 0);
    assert(valid("# nothing enabled\r\n  \t\r\n").count == 0);
    auto plan = valid("\xef\xbb\xbf# configuration\r\n hotupdate-supervisor \r\n"
                      "p2ping # optional\n\tp2ping\n");
    assert(plan.count == 2);
    assert(std::string(plan.names[0]) == "hotupdate-supervisor");
    assert(std::string(plan.names[1]) == "p2ping");
    plan = valid("p2ping\nhotupdate-supervisor");
    assert(std::string(plan.names[0]) == "p2ping");
    assert(std::string(plan.names[1]) == "hotupdate-supervisor");
    assert(valid("Name_123-test").count == 1);
    assert(valid(std::string(63, 'x')).count == 1);
    invalid(std::string(64, 'x'));
    for (const char* name : {"../other", "/absolute", "a/b", "a\\b", "a:b", "p2ping.nso",
                             "two names", "two\tnames", ".", "..", "*", "\xef\xbb\xbfwrong"})
        invalid(std::string("hotupdate-supervisor\n") + name);
    invalid(std::string("p2ping\0bad", 10));
    invalid(std::string("# comment\0", 10));
    invalid(std::string("\xff\xfe", 2));
    assert(valid("#" + std::string(4095, 'x')).count == 0);
    invalid(std::string(4097, '#'), Error::TooLarge);
    std::string many;
    for (unsigned i = 0; i < 64; ++i) many += "plugin-" + std::to_string(i) + "\n";
    assert(valid(many).count == 64);
    assert(valid(many + "plugin-0\n").count == 64);
    invalid(many + "plugin-64\n", Error::TooManyPlugins);
    // Compatibility: unlisted third-party plugins remain eligible; installing
    // or blocking one plugin does not implicitly opt out any other plugin.
    const auto blocked = valid("screen-streamer\np2ping\n");
    assert(autostart::contains(blocked, "screen-streamer"));
    assert(autostart::contains(blocked, "p2ping"));
    assert(!autostart::contains(blocked, "third-party"));
    assert(!autostart::contains(blocked, "screen-streamer-fast"));
    assert(!autostart::contains(valid(""), "third-party"));
    Plan empty;
    assert(autostart::parse(nullptr, 0, empty) == Error::None);
    assert(autostart::parse(nullptr, 1, empty) == Error::InvalidName);

    // Random byte streams exercise bounded scanning and all-or-nothing output
    // under ASan/UBSan. No kernel or console behavior is simulated here.
    uint32_t seed = 0x27112026;
    for (unsigned trial = 0; trial < 20000; ++trial) {
        seed = seed * 1664525 + 1013904223;
        std::string text(seed % 4200, ' ');
        for (char& byte : text) {
            seed = seed * 1664525 + 1013904223;
            byte = static_cast<char>(seed >> 24);
        }
        Plan result;
        Error error = autostart::parse(text.data(), text.size(), result);
        assert(result.count <= autostart::MaxPlugins);
        if (error != Error::None) assert(result.count == 0);
        for (size_t i = 0; i < result.count; ++i) {
            size_t size = strlen(result.names[i]);
            assert(size && size < autostart::NameSize);
            for (size_t j = 0; j < size; ++j) assert(autostart::nameByte(result.names[i][j]));
        }
    }
    puts("Auto-start parser: boundary, selection, rejection and 20000 random-input checks passed.");
}
