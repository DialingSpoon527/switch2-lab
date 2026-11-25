// SPDX-License-Identifier: GPL-2.0-only
// Host-only tests of HEN updater's one-target command/manifest vocabulary.
#include "../source/hen_request.hpp"
#include <cassert>
#include <cstring>
#include <string>
#include <iostream>
using namespace hotupdate::henonly;
static void expectCommand(const char* s, Request expected) {
    assert(parseRequest(s, std::strlen(s)) == expected);
}
int main() {
    expectCommand("STATUS", Request::Status);
    expectCommand("STATUS\n", Request::Status);
    expectCommand("STATUS\r\n", Request::Status);
    expectCommand("DEFER_DRYRUN_V1\n", Request::DryRun);
    expectCommand("DEFER_HEN_RESTART_V1", Request::QueueHen);
    expectCommand("APPLY_HEN_PACKAGE_V1\r\n", Request::ApplyManifest);
    expectCommand("CANCEL_DEFERRED_V2\n", Request::Cancel);
    expectCommand("DEFER_RO_RESTART_V1", Request::Invalid);
    expectCommand("APPLY_PACKAGE_V1", Request::Invalid);
    expectCommand("RESTART_RO_V1", Request::Invalid);
    expectCommand("STATUS\nSTATUS", Request::Invalid);
    expectCommand(" STATUS", Request::Invalid);
    expectCommand("STATUS\r", Request::Invalid);
    assert(parseRequest(nullptr, 0) == Request::Invalid);
    const std::string hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string manifest = "target=hen\nsha256=" + hash + "\n";
    const std::string win_manifest = "target=hen\r\nsha256=" + hash + "\r\n";
    std::uint8_t digest[32] = {};
    assert(parseManifest(manifest.data(), manifest.size(), digest));
    assert(digest[0] == 0x01 && digest[1] == 0x23 && digest[31] == 0xef);
    assert(parseManifest(win_manifest.data(), win_manifest.size(), digest));
    const std::string dangling_cr = "target=hen\nsha256=" + hash + "\r";
    assert(!parseManifest(dangling_cr.data(), dangling_cr.size(), digest));
    const std::string no_newline = "target=hen\nsha256=" + hash;
    assert(parseManifest(no_newline.data(), no_newline.size(), digest));
    const std::string other = "target=ro\nsha256=" + hash;
    assert(!parseManifest(other.data(), other.size(), digest));
    assert(!parseManifest((manifest+"extra").data(), manifest.size()+5, digest));
    const std::string short_hash = "target=hen\nsha256=abc";
    assert(!parseManifest(short_hash.data(), short_hash.size(), digest));
    const std::string bad_hex = "target=hen\nsha256=" + std::string(63,'a') + "z";
    assert(!parseManifest(bad_hex.data(), bad_hex.size(), digest));
    const std::string spaces = "target=hen\nsha256=" + hash + " \n";
    assert(!parseManifest(spaces.data(), spaces.size(), digest));
    assert(!parseManifest(nullptr, 0, digest));
    assert(!parseManifest(manifest.data(), manifest.size(), nullptr));
    std::cout << "HEN request and SHA256 manifest parser: PASS\n";
}
