// The passcode hash: PBKDF2-HMAC-SHA256 over a random salt.
//
// One passcode protects one setting (the "Visible folders" allow-list,
// core/gallery/visibility.h). The console never stores the passcode itself,
// never logs it and never draws it: the state file holds the algorithm, the
// iteration count, a fresh 16-byte salt and the 32-byte output, all lowercase
// hex. sha256.cpp is the only hash implementation in the tree (core/model), so
// HMAC and PBKDF2 are built from it here rather than adding a second SHA-256.
//
// 120000 iterations is the whole KDF budget: the owner types the code with a
// controller and waits about a second, and there is no lockout or retry limit
// (a user decision, DECISIONS.md "Visible folders"). The iteration count is
// stored in the file and checked against a sane range on load, so a file that
// claims 4 iterations is refused instead of trusted.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"

namespace splat {

using Sha256Digest = std::array<uint8_t, 32>;

// The one iteration count a new hash is made with.
constexpr int kPasscodeIterations = 120000;
constexpr size_t kPasscodeSaltBytes = 16;
constexpr size_t kPasscodeHashBytes = 32;
// The accepted range for a stored iteration count. Below the floor the file is
// not a hash worth trusting; above the ceiling a hand-edited file could make
// every unlock take minutes.
constexpr int kPasscodeMinIterations = 1000;
constexpr int kPasscodeMaxIterations = 1000000;
// The passcode the owner may type: 4 to 32 characters, any character the panel
// keyboard offers, no complexity rule (DECISIONS.md).
constexpr size_t kPasscodeMinLength = 4;
constexpr size_t kPasscodeMaxLength = 32;

inline constexpr const char* kPasscodeAlgorithm = "pbkdf2-hmac-sha256";

// One stored passcode: the four fields of the file's "passcode" object.
struct PasscodeHash {
    std::string algorithm = kPasscodeAlgorithm;
    int iterations = kPasscodeIterations;
    std::string salt;  // lowercase hex, 2 * kPasscodeSaltBytes characters
    std::string hash;  // lowercase hex, 2 * kPasscodeHashBytes characters

    // True when every field is in the shape this build writes: the right
    // algorithm, an iteration count inside the accepted range and salt/hash of
    // the right length. A file that fails this is damaged (fail closed), never
    // "no passcode".
    bool usable() const;
};

// HMAC-SHA256 (RFC 2104) with the block size of SHA-256.
Sha256Digest hmac_sha256(const void* key, size_t key_size, const void* data, size_t data_size);
// PBKDF2-HMAC-SHA256 (RFC 8018) producing one 32-byte block. `iterations` < 1
// is treated as 1.
Sha256Digest pbkdf2_hmac_sha256(const std::string& password, const std::string& salt,
                                int iterations);

std::string hex_encode(const uint8_t* data, size_t size);
// False for an odd length or a non-hex character.
bool hex_decode(const std::string& hex, std::vector<uint8_t>* out);

// A fresh salt and the hash of `passcode`. The passcode's length is not checked
// here: Visibility::enable()/change_passcode() apply the 4..32 rule so a
// caller that writes the file cannot skip it.
PasscodeHash make_passcode_hash(const std::string& passcode);

// True when `attempt` hashes to the stored value with its stored salt and
// iteration count. An unusable hash never verifies (fail closed).
bool verify_passcode(const std::string& attempt, const PasscodeHash& stored);

}  // namespace splat
