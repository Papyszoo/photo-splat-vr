#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "model/checkpoint.h"
#include "model/sha256.h"
#include "test_util.h"

using splat::Sha256;

namespace {

std::string hex_of(const std::string& text) {
    return Sha256::of_hex(text.data(), text.size());
}

}  // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 test vectors") {
    // Empty message.
    CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // "abc".
    CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Two-block message (448 bits).
    CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex_of("The quick brown fox jumps over the lazy dog") ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    // The classic one-million-'a' vector, which also crosses the 4 GiB bit-count
    // boundary only in longer runs, so the padding path is exercised.
    std::string million(1000000, 'a');
    CHECK(hex_of(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 streaming equals one-shot for awkward chunk sizes") {
    std::string data;
    for (int i = 0; i < 1000; ++i) data.push_back(static_cast<char>((i * 37) & 0xff));

    const std::string expected = hex_of(data);
    for (size_t chunk : {size_t{1}, size_t{3}, size_t{63}, size_t{64}, size_t{65}, size_t{199}}) {
        Sha256 hash;
        for (size_t offset = 0; offset < data.size(); offset += chunk) {
            const size_t size = std::min(chunk, data.size() - offset);
            hash.update(data.data() + offset, size);
        }
        CHECK(Sha256::hex(hash.finish()) == expected);
    }
    // Updating with zero bytes must not change anything.
    Sha256 empty_update;
    empty_update.update(nullptr, 0);
    CHECK(Sha256::hex(empty_update.finish()) == hex_of(""));
}

TEST_CASE("SHA-256 covers 128-byte block boundaries exactly") {
    // 55, 56, 63, 64 and 65 byte messages hit every padding branch.
    const std::string expected[] = {
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",  // 55 x 'a'
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",  // 56 x 'a'
        "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",  // 63 x 'a'
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",  // 64 x 'a'
        "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0",  // 65 x 'a'
    };
    const size_t lengths[] = {55, 56, 63, 64, 65};
    for (size_t i = 0; i < 5; ++i) {
        CHECK(hex_of(std::string(lengths[i], 'a')) == expected[i]);
    }
}

TEST_CASE("sha256_file streams a file and reports open failures") {
    splat_test::TempDir temp("sha256-file");
    const auto path = temp.path() / "blob.bin";
    std::string payload;
    for (int i = 0; i < 5000; ++i) payload.push_back(static_cast<char>('A' + (i % 26)));
    {
        std::ofstream out(path, std::ios::binary);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    const auto digest = splat::sha256_file(path, 97);
    REQUIRE_MESSAGE(digest.ok(), digest.message());
    CHECK(digest.value() == hex_of(payload));
    CHECK(digest.value().size() == 64);

    const auto missing = splat::sha256_file(temp.path() / "absent.bin");
    CHECK_FALSE(missing.ok());
}

TEST_CASE("the pinned checkpoint hash is a lowercase SHA-256") {
    const std::string pinned = splat::kSharpCheckpointSha256;
    CHECK(pinned.size() == 64);
    for (char character : pinned) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool valid = digit || lower;
        CHECK_MESSAGE(valid, "unexpected character in the pinned hash");
    }
    CHECK(std::string(splat::kSharpCheckpointName) == "sharp_2572gikvuh.pt");
}
