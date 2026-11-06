// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/passcode.h"

#include <algorithm>
#include <cstring>

#include "common/random_bytes.h"
#include "model/sha256.h"

namespace splat {
namespace {

constexpr size_t kBlockBytes = 64;  // SHA-256's block size

// One SHA-256 over two byte ranges, without concatenating them first: the salt
// and the block counter are separate by construction (RFC 8018's U1 = PRF(P,
// S || INT(i))).
Sha256Digest hash_two_ranges(const void* first, size_t first_size, const void* second,
                             size_t second_size) {
    Sha256 hasher;
    if (first_size != 0) hasher.update(first, first_size);
    if (second_size != 0) hasher.update(second, second_size);
    return hasher.finish();
}

// Compares two equal-length byte strings without an early exit. The length is
// not a secret here (both sides are 32 bytes), so it is folded in.
bool constant_time_equal(const Sha256Digest& a, const Sha256Digest& b) {
    unsigned char difference = 0;
    for (size_t index = 0; index < a.size(); ++index) {
        difference |= static_cast<unsigned char>(a[index] ^ b[index]);
    }
    return difference == 0;
}

}  // namespace

bool PasscodeHash::usable() const {
    if (algorithm != kPasscodeAlgorithm) return false;
    if (iterations < kPasscodeMinIterations || iterations > kPasscodeMaxIterations) return false;
    if (salt.size() != 2 * kPasscodeSaltBytes) return false;
    if (hash.size() != 2 * kPasscodeHashBytes) return false;
    std::vector<uint8_t> decoded;
    return hex_decode(salt, &decoded) && hex_decode(hash, &decoded);
}

Sha256Digest hmac_sha256(const void* key, size_t key_size, const void* data, size_t data_size) {
    const auto* key_bytes = static_cast<const uint8_t*>(key);
    std::array<uint8_t, kBlockBytes> block{};
    if (key_size > kBlockBytes) {
        // A long key is replaced by its own hash, then zero-padded.
        const Sha256Digest hashed = Sha256::of(key, key_size);
        std::memcpy(block.data(), hashed.data(), hashed.size());
    } else if (key_size != 0) {
        std::memcpy(block.data(), key_bytes, key_size);
    }

    std::array<uint8_t, kBlockBytes> inner{};
    std::array<uint8_t, kBlockBytes> outer{};
    for (size_t index = 0; index < kBlockBytes; ++index) {
        inner[index] = static_cast<uint8_t>(block[index] ^ 0x36);
        outer[index] = static_cast<uint8_t>(block[index] ^ 0x5c);
    }

    const Sha256Digest inner_hash =
        hash_two_ranges(inner.data(), inner.size(), data, data_size);
    Sha256 outer_hasher;
    outer_hasher.update(outer.data(), outer.size());
    outer_hasher.update(inner_hash.data(), inner_hash.size());
    return outer_hasher.finish();
}

Sha256Digest pbkdf2_hmac_sha256(const std::string& password, const std::string& salt,
                                int iterations) {
    const int rounds = std::max(1, iterations);
    // dkLen == hLen, so the output is exactly the first block: T = F(P, S, c, 1)
    // with F = U1 xor U2 xor ... xor Uc. The salt and the block counter are one
    // PRF input, so the first call hashes salt||INT(1) (big-endian, as RFC 8018
    // spells it).
    const uint8_t block_index[4] = {0, 0, 0, 1};
    std::string first = salt;
    first.append(reinterpret_cast<const char*>(block_index), sizeof block_index);
    Sha256Digest u = hmac_sha256(password.data(), password.size(), first.data(), first.size());
    Sha256Digest result = u;
    for (int round = 1; round < rounds; ++round) {
        u = hmac_sha256(password.data(), password.size(), u.data(), u.size());
        for (size_t index = 0; index < result.size(); ++index) {
            result[index] = static_cast<uint8_t>(result[index] ^ u[index]);
        }
    }
    return result;
}

std::string hex_encode(const uint8_t* data, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t index = 0; index < size; ++index) {
        out[2 * index] = kHex[data[index] >> 4];
        out[2 * index + 1] = kHex[data[index] & 0x0f];
    }
    return out;
}

bool hex_decode(const std::string& hex, std::vector<uint8_t>* out) {
    if (out == nullptr || hex.size() % 2 != 0) return false;
    std::vector<uint8_t> bytes(hex.size() / 2, 0);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index < bytes.size(); ++index) {
        const int high = nibble(hex[2 * index]);
        const int low = nibble(hex[2 * index + 1]);
        if (high < 0 || low < 0) return false;
        bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    *out = std::move(bytes);
    return true;
}

PasscodeHash make_passcode_hash(const std::string& passcode) {
    PasscodeHash stored;
    stored.algorithm = kPasscodeAlgorithm;
    stored.iterations = kPasscodeIterations;
    stored.salt = random_hex(kPasscodeSaltBytes);
    std::vector<uint8_t> salt;
    hex_decode(stored.salt, &salt);
    const Sha256Digest digest = pbkdf2_hmac_sha256(
        passcode, std::string(salt.begin(), salt.end()), stored.iterations);
    stored.hash = hex_encode(digest.data(), digest.size());
    return stored;
}

bool verify_passcode(const std::string& attempt, const PasscodeHash& stored) {
    if (!stored.usable()) return false;
    std::vector<uint8_t> salt;
    std::vector<uint8_t> expected;
    if (!hex_decode(stored.salt, &salt) || !hex_decode(stored.hash, &expected)) return false;
    if (expected.size() != kPasscodeHashBytes) return false;
    const Sha256Digest digest = pbkdf2_hmac_sha256(
        attempt, std::string(salt.begin(), salt.end()), stored.iterations);
    Sha256Digest wanted{};
    std::memcpy(wanted.data(), expected.data(), wanted.size());
    return constant_time_equal(digest, wanted);
}

}  // namespace splat
