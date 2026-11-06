// The passcode hash: known-answer tests for HMAC-SHA256 and PBKDF2-HMAC-SHA256,
// the "never the passcode itself" rule, and the cost of the 120000 iterations a
// console unlock pays on the UI thread.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "common/passcode.h"

namespace {

std::string hex_of(const splat::Sha256Digest& digest) {
    return splat::hex_encode(digest.data(), digest.size());
}

}  // namespace

TEST_CASE("passcode HMAC-SHA256 matches RFC 4231 case 2") {
    const std::string key = "Jefe";
    const std::string data = "what do ya want for nothing?";
    const splat::Sha256Digest digest =
        splat::hmac_sha256(key.data(), key.size(), data.data(), data.size());
    CHECK(hex_of(digest) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("passcode PBKDF2-HMAC-SHA256 matches the published vectors") {
    // RFC 7914 / the widely published PBKDF2-HMAC-SHA256 vectors: password
    // "password", salt "salt", dkLen 32.
    struct Vector {
        int iterations;
        const char* expected;
    };
    const Vector vectors[] = {
        {1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
        {2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
        {4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"},
    };
    for (const Vector& vector : vectors) {
        const splat::Sha256Digest digest =
            splat::pbkdf2_hmac_sha256("password", "salt", vector.iterations);
        CHECK(hex_of(digest) == vector.expected);
    }
}

TEST_CASE("passcode hashes carry a fresh salt and hide the passcode") {
    const splat::PasscodeHash first = splat::make_passcode_hash("2468");
    const splat::PasscodeHash second = splat::make_passcode_hash("2468");
    CHECK(first.usable());
    CHECK(second.usable());
    CHECK(first.iterations == splat::kPasscodeIterations);
    CHECK(first.salt.size() == 2 * splat::kPasscodeSaltBytes);
    CHECK(first.hash.size() == 2 * splat::kPasscodeHashBytes);
    // The same passcode twice must not produce the same file.
    CHECK(first.salt != second.salt);
    CHECK(first.hash != second.hash);
    // The passcode string never appears in either field.
    CHECK(first.salt.find("2468") == std::string::npos);
    CHECK(first.hash.find("2468") == std::string::npos);
}

TEST_CASE("passcode verify accepts the passcode and rejects a one-character change") {
    const splat::PasscodeHash stored = splat::make_passcode_hash("family");
    CHECK(splat::verify_passcode("family", stored));
    CHECK_FALSE(splat::verify_passcode("familY", stored));
    CHECK_FALSE(splat::verify_passcode("famil", stored));
    CHECK_FALSE(splat::verify_passcode("", stored));

    // An unusable hash never verifies, whatever is typed: fail closed.
    splat::PasscodeHash broken = stored;
    broken.algorithm = "md5";
    CHECK_FALSE(splat::verify_passcode("family", broken));
    broken = stored;
    broken.iterations = 1;
    CHECK_FALSE(broken.usable());
    CHECK_FALSE(splat::verify_passcode("family", broken));
    broken = stored;
    broken.salt.clear();
    CHECK_FALSE(splat::verify_passcode("family", broken));
}

TEST_CASE("passcode hex round-trips and refuses garbage") {
    const std::vector<uint8_t> bytes = {0x00, 0x0f, 0xa5, 0xff};
    const std::string text = splat::hex_encode(bytes.data(), bytes.size());
    CHECK(text == "000fa5ff");
    std::vector<uint8_t> decoded;
    REQUIRE(splat::hex_decode(text, &decoded));
    CHECK(decoded == bytes);
    CHECK_FALSE(splat::hex_decode("abc", &decoded));   // odd length
    CHECK_FALSE(splat::hex_decode("zz", &decoded));    // not hex
}

TEST_CASE("passcode 120000 iterations stay under five seconds") {
    // The KDF runs on the UI thread when the owner submits: the measured time is
    // recorded in the report, and the bound is loose on purpose (a slow CI must
    // not fail a correct implementation).
    const auto start = std::chrono::steady_clock::now();
    const splat::PasscodeHash stored = splat::make_passcode_hash("2468");
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("120000 iterations: " << seconds * 1000.0 << " ms");
    CHECK(stored.usable());
    CHECK(seconds < 5.0);

    const auto verify_start = std::chrono::steady_clock::now();
    CHECK(splat::verify_passcode("2468", stored));
    const double verify_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - verify_start).count();
    MESSAGE("one verify: " << verify_seconds * 1000.0 << " ms");
    CHECK(verify_seconds < 5.0);
}
